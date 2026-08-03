/* rrv10.cpp — Schwung audio FX wrapper around the BOSS HG61H20R36F reverb
 * gate array emulation (BossEmu.cpp, LGPL-2.1+, (C) Sergey V. Mikayev / MUNT).
 *
 * The emulation is cycle-accurate against a dump of the unit's program ROM and
 * needs no tuning; nearly everything here is about running it at the RIGHT RATE
 * and getting audio in and out of it.
 *
 * RATE (the one thing an off-the-shelf port gets wrong)
 * ----------------------------------------------------
 * The chip consumes 256 clock cycles per audio sample (PROCESSING_LOOP_CYCLES)
 * and the RRV-10 clocks it from an 8.000 MHz crystal, so its native rate is
 * 31250 Hz — which the service manual also states outright ("Sampling
 * Frequency: 31.25 kHz").  Driving one chip cycle per HOST sample instead, as
 * the reference JUCE plugin does, runs the emulation 41% fast: every decay
 * comes out 29% short and the whole response shifts up 5.97 semitones.
 *
 * So we resample.  See resampler_poly.h for why the internal rate is 31237.5
 * rather than 31250 (0.69 cents flat, in exchange for a 17/24 ratio) and why
 * 24 taps is transparent here.  Running the chip at its true, slower rate
 * removes ~29% of the emulator's work, which more than pays for the resampler:
 * the correct version measured CHEAPER end-to-end than the naive one.
 *
 * ROM
 * ---
 * The 16 KB program ROM is Roland's and is not redistributable, so it is not in
 * this repo — the user supplies it via the host's `assets` mechanism and it
 * lands at <module_dir>/roms/rrv10.bin.  With no ROM the effect reports its
 * status and passes audio through dry rather than failing to load.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "host/audio_fx_api_v2.h"
#include "BossEmu.h"
#include "resampler_poly.h"

static const host_api_v1_t *g_host = NULL;

static void v2_log(const char *msg)
{
    char line[160];
    snprintf(line, sizeof(line), "rrv10: %s", msg);
    if (g_host && g_host->log) g_host->log(line);
}

/* ---- rates ------------------------------------------------------------- */
/* Host rate is 44100 (see docs/MODULES.md "Audio Specifications"). 17/24 of
 * that is 31237.5 Hz, our stand-in for the chip's true 31250 Hz. */
#define RP_DOWN_L 17
#define RP_DOWN_M 24
#define RP_UP_L   24
#define RP_UP_M   17
#define RP_CUTOFF_HZ 13000.0    /* above the RRV-10's 10 kHz effect band,
                                 * below the 15618 Hz intermediate Nyquist */

/* The emulator's own I/O scaling, carried over from the reference plugin: chip
 * samples are 16-bit and the input is halved internally, so 16383 keeps a
 * full-scale float inside range without clipping the accumulator. */
#define EMU_SCALE 16383.0f

/* Enough slack that a pop never outruns the pushes. The two stages are exactly
 * rate-matched (24 in -> 17 chip -> 24 out) so this never drifts; it only
 * absorbs the instantaneous jitter of which frame emits. Measured worst
 * transient deficit is 2, so 4 is comfortable — 4 frames is 0.09 ms. */
#define FIFO_CAP   64
#define FIFO_PRIME 4

#define ROM_BYTES 16384

typedef struct {
    /* --- emulation --- */
    unsigned char rom[ROM_BYTES];
    int           rom_ok;
    char          rom_status[96];
    BossEmu      *emu;

    /* --- rate conversion --- */
    rp_stage_t dn, up;
    float fifoL[FIFO_CAP], fifoR[FIFO_CAP];
    int   fifo_head, fifo_tail, fifo_count;

    /* --- parameters (native units, as declared in chain_params) --- */
    int mode;           /* 0..8  — panel order, see kModeNames  */
    int time;           /* 0..15 — Decay/Gate Time              */
    int pre_eq;         /* 0..100, 50 = flat                    */
    int mix;            /* 0 = dry only .. 100 = wet only       */
    int editor;         /* canvas bank-editor persisted bank    */

    /* --- smoothed control signals (zipper-free level/tone moves) --- */
    float sm_wet, sm_dry, sm_pre_eq;

    /* --- pre-EQ one-pole state --- */
    float eqL, eqR;
} rrv10_t;

/* Panel order, verified three ways: only ROM banks 0-8 are non-zero, the unit's
 * documentation lists these nine, and each program's measured impulse response
 * matches its name (6 is discrete taps, 7 rises then stops, 8 is flat then
 * gates). */
static const char *kModeNames[9] = {
    "Room 1", "Room 2", "Hall 1", "Hall 2",
    "Plate 1", "Plate 2", "M-Tap 1", "M-Tap 2", "Gate"
};

/* ---- helpers ----------------------------------------------------------- */

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* Equal-power wet/dry. Reverb tails are largely uncorrelated with the dry
 * signal, so a linear crossfade audibly dips through the middle of the sweep
 * while a sin/cos pair holds the summed power roughly constant. */
static void mix_gains(int mix, float *wet, float *dry)
{
    float m = clampi(mix, 0, 100) * 0.01f * (float)M_PI * 0.5f;
    *wet = sinf(m);
    *dry = cosf(m);
}

static void rrv10_apply_program(rrv10_t *p)
{
    if (p->emu) p->emu->setParameters(p->mode, p->time, 7); /* level: analog on
                                                             * the real unit, so
                                                             * RV_2 mode ignores it */
}

static void fifo_reset(rrv10_t *p)
{
    memset(p->fifoL, 0, sizeof(p->fifoL));
    memset(p->fifoR, 0, sizeof(p->fifoR));
    p->fifo_head = 0;
    p->fifo_tail = 0;
    p->fifo_count = 0;
    for (int i = 0; i < FIFO_PRIME; i++) {
        p->fifoL[p->fifo_head] = 0.0f;
        p->fifoR[p->fifo_head] = 0.0f;
        p->fifo_head = (p->fifo_head + 1) % FIFO_CAP;
        p->fifo_count++;
    }
}

static int load_rom(rrv10_t *p, const char *module_dir)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/roms/rrv10.bin", module_dir ? module_dir : ".");

    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(p->rom_status, sizeof(p->rom_status), "missing: roms/rrv10.bin");
        return 0;
    }
    size_t n = fread(p->rom, 1, ROM_BYTES + 1, f);
    fclose(f);

    if (n != ROM_BYTES) {
        snprintf(p->rom_status, sizeof(p->rom_status),
                 "bad size: %d bytes, expected %d", (int)n, ROM_BYTES);
        return 0;
    }
    snprintf(p->rom_status, sizeof(p->rom_status), "ok");
    return 1;
}

/* ---- lifecycle --------------------------------------------------------- */

static void *v2_create_instance(const char *module_dir, const char *config_json)
{
    (void)config_json;
    rrv10_t *p = (rrv10_t *)calloc(1, sizeof(rrv10_t));
    if (!p) return NULL;

    p->mode = 2;              /* Hall 1 — a useful default reverb        */
    p->time = 8;
    p->pre_eq = 50;           /* flat                                    */
    p->mix = 30;              /* a send-ish default for an insert        */
    p->editor = 0;
    mix_gains(p->mix, &p->sm_wet, &p->sm_dry);
    p->sm_pre_eq = 0.50f;

    p->rom_ok = load_rom(p, module_dir);
    if (p->rom_ok) {
        /* RV_2 mode: this is a standalone Boss unit, not the MT-32 variant —
         * it selects the 1 KB-per-mode ROM layout and the analog output level. */
        p->emu = new BossEmu(p->rom, ROM_BYTES, BossEmu::RV_2_EMU_MODE);
        rrv10_apply_program(p);
        v2_log("ROM loaded, emulation active");
    } else {
        v2_log(p->rom_status);
    }

    rp_init(&p->dn, RP_DOWN_L, RP_DOWN_M, RP_CUTOFF_HZ, 44100.0);
    rp_init(&p->up, RP_UP_L,   RP_UP_M,   RP_CUTOFF_HZ, 44100.0 * 17.0 / 24.0);
    fifo_reset(p);

    return p;
}

static void v2_destroy_instance(void *instance)
{
    rrv10_t *p = (rrv10_t *)instance;
    if (!p) return;
    delete p->emu;
    free(p);
}

/* ---- audio ------------------------------------------------------------- */

static void v2_process_block(void *instance, int16_t *io, int frames)
{
    rrv10_t *p = (rrv10_t *)instance;
    if (!p) return;

    /* No ROM: the effect is a no-op rather than silence, so a chain with a
     * missing asset still passes audio. */
    if (!p->rom_ok || !p->emu) return;

    /* Gains resolved once per block; only the smoothing runs per sample, so the
     * sin/cos pair is not in the inner loop. */
    float tgt_wet, tgt_dry;
    mix_gains(p->mix, &tgt_wet, &tgt_dry);
    const float tgt_pre_eq = p->pre_eq * 0.01f;
    const float sm = 0.002f;   /* ~one-pole, ~8 ms at 44.1k — below zipper
                                * threshold, above audible lag */

    float emuL[2], emuR[2], outL[2], outR[2];

    for (int i = 0; i < frames; i++) {
        float dryL = io[i * 2 + 0] / 32768.0f;
        float dryR = io[i * 2 + 1] / 32768.0f;

        p->sm_wet += (tgt_wet - p->sm_wet) * sm;
        p->sm_dry += (tgt_dry - p->sm_dry) * sm;
        p->sm_pre_eq += (tgt_pre_eq - p->sm_pre_eq) * sm;

        /* Pre-equalizer, ahead of the converter as on the hardware. The manual:
         * clockwise "cuts the lower frequency, creating brighter sound",
         * counter-clockwise "cuts the higher frequency creating warmer sound".
         * So < 0.5 is a lowpass and > 0.5 a highpass, both one-pole. */
        float sendL = dryL, sendR = dryR;
        float e = p->sm_pre_eq;
        if (e < 0.5f) {
            float k = 1.0f - (0.5f - e) * 2.0f;
            p->eqL += k * (dryL - p->eqL);
            p->eqR += k * (dryR - p->eqR);
            sendL = p->eqL;
            sendR = p->eqR;
        } else if (e > 0.5f) {
            float k = (e - 0.5f) * 2.0f;
            p->eqL += k * (dryL - p->eqL);
            p->eqR += k * (dryR - p->eqR);
            sendL = dryL - p->eqL;
            sendR = dryR - p->eqR;
        } else {
            p->eqL = dryL;
            p->eqR = dryR;
        }

        /* 44100 -> 31237.5, run the chip, 31237.5 -> 44100. */
        int nd = rp_push(&p->dn, sendL, sendR, emuL, emuR);
        for (int j = 0; j < nd; j++) {
            short inL  = (short)(emuL[j] * EMU_SCALE);
            short inR  = (short)(emuR[j] * EMU_SCALE);
            short wetL = 0, wetR = 0;
            p->emu->process(&inL, &inR, &wetL, &wetR, 1);

            int nu = rp_push(&p->up, wetL / EMU_SCALE, wetR / EMU_SCALE, outL, outR);
            for (int k = 0; k < nu; k++) {
                if (p->fifo_count >= FIFO_CAP) continue;   /* unreachable; guard */
                p->fifoL[p->fifo_head] = outL[k];
                p->fifoR[p->fifo_head] = outR[k];
                p->fifo_head = (p->fifo_head + 1) % FIFO_CAP;
                p->fifo_count++;
            }
        }

        float wL = 0.0f, wR = 0.0f;
        if (p->fifo_count > 0) {
            wL = p->fifoL[p->fifo_tail];
            wR = p->fifoR[p->fifo_tail];
            p->fifo_tail = (p->fifo_tail + 1) % FIFO_CAP;
            p->fifo_count--;
        }

        float oL = wL * p->sm_wet + dryL * p->sm_dry;
        float oR = wR * p->sm_wet + dryR * p->sm_dry;

        if (oL >  1.0f) oL =  1.0f;
        if (oL < -1.0f) oL = -1.0f;
        if (oR >  1.0f) oR =  1.0f;
        if (oR < -1.0f) oR = -1.0f;

        io[i * 2 + 0] = (int16_t)(oL * 32767.0f);
        io[i * 2 + 1] = (int16_t)(oR * 32767.0f);
    }
}

/* ---- params ------------------------------------------------------------ */

static void v2_set_param(void *instance, const char *key, const char *val)
{
    rrv10_t *p = (rrv10_t *)instance;
    if (!p || !key || !val) return;

    if (strcmp(key, "mode") == 0) {
        p->mode = clampi(atoi(val), 0, 8);
        rrv10_apply_program(p);
    } else if (strcmp(key, "time") == 0) {
        p->time = clampi(atoi(val), 0, 15);
        rrv10_apply_program(p);
    } else if (strcmp(key, "pre_eq") == 0) {
        p->pre_eq = clampi(atoi(val), 0, 100);
    } else if (strcmp(key, "mix") == 0) {
        p->mix = clampi(atoi(val), 0, 100);
    } else if (strcmp(key, "editor") == 0) {
        /* The canvas owns the real bound; clamp generously. */
        p->editor = clampi(atoi(val), 0, 31);
    } else if (strcmp(key, "state") == 0) {
        /* Minimal scanner — module.json is parsed by a small reader on the host
         * side and state blobs are ours, so a targeted sscanf per key is enough
         * and avoids pulling in a JSON parser. */
        const char *s;
        int v;
        #define RRV10_RESTORE(k, field, lo, hi)                              \
            if ((s = strstr(val, "\"" k "\":")) != NULL &&                   \
                sscanf(s + strlen(k) + 3, "%d", &v) == 1)                    \
                p->field = clampi(v, lo, hi)
        RRV10_RESTORE("mode",   mode,   0, 8);
        RRV10_RESTORE("time",   time,   0, 15);
        RRV10_RESTORE("pre_eq", pre_eq, 0, 100);
        RRV10_RESTORE("mix",    mix,    0, 100);
        RRV10_RESTORE("editor", editor, 0, 31);
        #undef RRV10_RESTORE
        rrv10_apply_program(p);
    }
}

static int v2_get_param(void *instance, const char *key, char *buf, int buf_len)
{
    rrv10_t *p = (rrv10_t *)instance;
    if (!p || !key || !buf) return -1;

    if (strcmp(key, "mode") == 0)         return snprintf(buf, buf_len, "%d", p->mode);
    if (strcmp(key, "time") == 0)         return snprintf(buf, buf_len, "%d", p->time);
    if (strcmp(key, "pre_eq") == 0)       return snprintf(buf, buf_len, "%d", p->pre_eq);
    if (strcmp(key, "mix") == 0)          return snprintf(buf, buf_len, "%d", p->mix);
    if (strcmp(key, "editor") == 0)       return snprintf(buf, buf_len, "%d", p->editor);

    if (strcmp(key, "name") == 0)         return snprintf(buf, buf_len, "RRV-10");
    if (strcmp(key, "rom_status") == 0)   return snprintf(buf, buf_len, "%s", p->rom_status);
    if (strcmp(key, "mode_name") == 0)
        return snprintf(buf, buf_len, "%s", kModeNames[clampi(p->mode, 0, 8)]);

    /* Served live from the plugin: the Master/Send FX bus reads chain_params
     * from the .so rather than the cached module.json, so labels and ranges
     * only refresh if they are here too. Keep in sync with module.json. */
    if (strcmp(key, "chain_params") == 0) {
        return snprintf(buf, buf_len,
            "["
            "{\"key\":\"mode\",\"name\":\"Mode\",\"type\":\"enum\",\"default\":2,"
              "\"options\":[\"Room 1\",\"Room 2\",\"Hall 1\",\"Hall 2\",\"Plate 1\","
              "\"Plate 2\",\"M-Tap 1\",\"M-Tap 2\",\"Gate\"]},"
            "{\"key\":\"time\",\"name\":\"Decay/Gate\",\"type\":\"int\",\"min\":0,\"max\":15,\"default\":8},"
            "{\"key\":\"pre_eq\",\"name\":\"Pre-EQ\",\"type\":\"int\",\"min\":0,\"max\":100,\"default\":50},"
            /* unit "%%" with an explicit max:100 tells the shared formatter the
             * values are already in display range, not normalised 0..1. */
            "{\"key\":\"mix\",\"name\":\"Mix\",\"type\":\"int\",\"min\":0,\"max\":100,\"default\":30,\"unit\":\"%%\"},"
            "{\"key\":\"editor\",\"name\":\"Bank Editor\",\"type\":\"canvas\","
              "\"canvas_script\":\"canvas.js#bank_editor\",\"show_footer\":false,\"show_value\":false}"
            "]");
    }

    if (strcmp(key, "state") == 0) {
        return snprintf(buf, buf_len,
            "{\"mode\":%d,\"time\":%d,\"pre_eq\":%d,\"mix\":%d,\"editor\":%d}",
            p->mode, p->time, p->pre_eq, p->mix, p->editor);
    }

    if (strcmp(key, "ui_hierarchy") == 0) {
        const char *h =
            "{\"modes\":null,\"levels\":{"
              "\"root\":{"
                "\"name\":\"RRV-10\","
                "\"knobs\":[\"mode\",\"time\",\"pre_eq\",\"mix\"],"
                "\"params\":[{\"key\":\"editor\",\"label\":\"Bank Editor\"},"
                            "\"mode\",\"time\",\"pre_eq\",\"mix\"]"
              "}"
            "}}";
        int len = (int)strlen(h);
        if (len < buf_len) { strcpy(buf, h); return len; }
        return -1;
    }

    return -1;
}

/* ---- entry point ------------------------------------------------------- */

#ifndef RRV10_TEST
static audio_fx_api_v2_t g_fx_api_v2;

extern "C" audio_fx_api_v2_t *move_audio_fx_init_v2(const host_api_v1_t *host)
{
    g_host = host;
    memset(&g_fx_api_v2, 0, sizeof(g_fx_api_v2));
    g_fx_api_v2.api_version    = AUDIO_FX_API_VERSION_2;
    g_fx_api_v2.create_instance  = v2_create_instance;
    g_fx_api_v2.destroy_instance = v2_destroy_instance;
    g_fx_api_v2.process_block    = v2_process_block;
    g_fx_api_v2.set_param        = v2_set_param;
    g_fx_api_v2.get_param        = v2_get_param;
    v2_log("RRV-10 v2 plugin initialized");
    return &g_fx_api_v2;
}
#endif
