/* Native (host-arch) unit tests for the RRV-10 module.
 *
 * The load-bearing test is test_rate_is_31250: it asserts the multi-tap delay's
 * first echo lands where the HARDWARE puts it, which is the one thing a naive
 * port of this emulation gets wrong. Run: ./scripts/test.sh
 */
#define RRV10_TEST 1
#include "../src/dsp/rrv10.cpp"

#include <stdio.h>
#include <math.h>
#include <vector>

static int g_fail = 0;

static void ok(int cond, const char *what)
{
    printf("  %s %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) g_fail++;
}

/* The repo root, so <dir>/roms/rrv10.bin resolves. */
static const char *MODDIR = "..";

/* Render `n` frames through the effect, feeding an impulse on frame 0. */
static void impulse_response(void *inst, std::vector<float> &outL, int n)
{
    outL.resize(n);
    const int BLK = 128;
    std::vector<int16_t> buf(BLK * 2);
    for (int b = 0; b < n / BLK; b++) {
        for (int i = 0; i < BLK; i++) {
            int g = b * BLK + i;
            int16_t v = (g == 0) ? 24000 : 0;
            buf[i * 2 + 0] = v;
            buf[i * 2 + 1] = v;
        }
        v2_process_block(inst, buf.data(), BLK);
        for (int i = 0; i < BLK; i++) outL[b * BLK + i] = buf[i * 2] / 32768.0f;
    }
}

/* ---- the rate test ----------------------------------------------------- */
/* Mode 6 (Multi-Tap 1) places its first echo at exactly 1100*t - 601 CHIP
 * cycles, measured off the ROM across t=2..14. The chip runs at 31250 Hz, so at
 * the 44100 Hz output that echo must appear at (1100*t-601) * 44100/31250.
 *
 * If the emulation were driven one cycle per host sample -- the common mistake,
 * and what the reference JUCE plugin does -- the echo would arrive at
 * (1100*t-601) samples instead: 41% early. This test tells the two apart. */
static void test_rate_is_31250(void)
{
    printf("test_rate_is_31250\n");
    void *inst = v2_create_instance(MODDIR, NULL);
    if (!inst) { ok(0, "instance created"); return; }

    v2_set_param(inst, "mode", "6");          /* Multi-Tap 1 (delay) */
    v2_set_param(inst, "time", "8");
    v2_set_param(inst, "effect_level", "100");
    v2_set_param(inst, "direct_level", "0");

    std::vector<float> ir;
    impulse_response(inst, ir, 1 << 15);

    /* Locate the first echo: peak after the direct-path region. */
    int peak = 0;
    float pv = 0.0f;
    for (int i = 200; i < (int)ir.size(); i++) {
        float a = fabsf(ir[i]);
        if (a > pv) { pv = a; peak = i; }
    }

    const double chip_cycles = 1100.0 * 8 - 601.0;              /* = 8199 */
    const double expect_44k  = chip_cycles * 44100.0 / 31250.0; /* = 11570.9 */
    const double naive_44k   = chip_cycles;                     /* if rate ignored */

    printf("    first echo at %d samples (%.1f ms)\n", peak, peak / 44.1);
    printf("    correct-rate expectation %.0f, naive-rate would be %.0f\n",
           expect_44k, naive_44k);

    /* +-1% of the correct position, and unambiguously not the naive one. */
    ok(fabs(peak - expect_44k) < expect_44k * 0.01, "echo at the hardware's position");
    ok(fabs(peak - naive_44k) > naive_44k * 0.10,   "echo is NOT at the naive position");

    v2_destroy_instance(inst);
}

/* ---- everything else --------------------------------------------------- */

static void test_rom_loads(void)
{
    printf("test_rom_loads\n");
    void *inst = v2_create_instance(MODDIR, NULL);
    ok(inst != NULL, "instance created");
    if (!inst) return;
    char buf[128];
    v2_get_param(inst, "rom_status", buf, sizeof(buf));
    printf("    rom_status = \"%s\"\n", buf);
    ok(strcmp(buf, "ok") == 0, "ROM loaded and sized correctly");
    v2_destroy_instance(inst);
}

static void test_missing_rom_passes_audio(void)
{
    printf("test_missing_rom_passes_audio\n");
    void *inst = v2_create_instance("/nonexistent-module-dir", NULL);
    ok(inst != NULL, "instance still created without a ROM");
    if (!inst) return;

    char buf[128];
    v2_get_param(inst, "rom_status", buf, sizeof(buf));
    ok(strstr(buf, "missing") != NULL, "rom_status reports the missing file");

    int16_t blk[256];
    for (int i = 0; i < 128; i++) { blk[i * 2] = 1234; blk[i * 2 + 1] = -4321; }
    v2_process_block(inst, blk, 128);
    ok(blk[0] == 1234 && blk[1] == -4321, "audio passes through untouched (not silence)");

    v2_destroy_instance(inst);
}

static void test_reverb_actually_decays(void)
{
    printf("test_reverb_actually_decays\n");
    void *inst = v2_create_instance(MODDIR, NULL);
    if (!inst) { ok(0, "instance created"); return; }
    v2_set_param(inst, "mode", "0");          /* Room 1 */
    v2_set_param(inst, "time", "8");
    v2_set_param(inst, "effect_level", "100");
    v2_set_param(inst, "direct_level", "0");

    std::vector<float> ir;
    impulse_response(inst, ir, 1 << 16);

    /* Energy in an early window vs a late one. The chip has a limit-cycle floor
     * around -65 dB, so this asserts a decay, not silence. */
    double early = 0, late = 0;
    for (int i = 2000; i < 8000; i++) early += ir[i] * ir[i];
    for (int i = 50000; i < 56000; i++) late += ir[i] * ir[i];
    double drop = 10.0 * log10((early + 1e-30) / (late + 1e-30));
    printf("    early/late energy ratio %.1f dB\n", drop);
    ok(early > 1e-9, "reverb produces output");
    ok(drop > 12.0,  "reverb decays over time");

    v2_destroy_instance(inst);
}

static void test_params_round_trip(void)
{
    printf("test_params_round_trip\n");
    void *inst = v2_create_instance(MODDIR, NULL);
    if (!inst) { ok(0, "instance created"); return; }
    char buf[64];

    v2_set_param(inst, "mode", "7");
    v2_get_param(inst, "mode", buf, sizeof(buf));
    ok(atoi(buf) == 7, "mode round-trips");

    v2_get_param(inst, "mode_name", buf, sizeof(buf));
    ok(strcmp(buf, "M-Tap 2") == 0, "mode_name matches the panel");

    v2_set_param(inst, "mode", "99");         /* out of range */
    v2_get_param(inst, "mode", buf, sizeof(buf));
    ok(atoi(buf) == 8, "mode clamps to the 9 real programs");

    v2_set_param(inst, "time", "15");
    v2_get_param(inst, "time", buf, sizeof(buf));
    ok(atoi(buf) == 15, "time round-trips");

    v2_destroy_instance(inst);
}

static void test_state_save_restore(void)
{
    printf("test_state_save_restore\n");
    void *a = v2_create_instance(MODDIR, NULL);
    void *b = v2_create_instance(MODDIR, NULL);
    if (!a || !b) { ok(0, "instances created"); return; }

    v2_set_param(a, "mode", "5");
    v2_set_param(a, "time", "11");
    v2_set_param(a, "pre_eq", "77");
    v2_set_param(a, "effect_level", "33");
    v2_set_param(a, "direct_level", "22");

    char state[512];
    v2_get_param(a, "state", state, sizeof(state));
    v2_set_param(b, "state", state);

    char x[64], y[64];
    const char *keys[] = {"mode", "time", "pre_eq", "effect_level", "direct_level"};
    int all = 1;
    for (int i = 0; i < 5; i++) {
        v2_get_param(a, keys[i], x, sizeof(x));
        v2_get_param(b, keys[i], y, sizeof(y));
        if (strcmp(x, y) != 0) { printf("    %s: %s != %s\n", keys[i], x, y); all = 0; }
    }
    ok(all, "every param survives state save/restore");

    v2_destroy_instance(a);
    v2_destroy_instance(b);
}

static void test_metadata_json_is_wellformed(void)
{
    printf("test_metadata_json_is_wellformed\n");
    void *inst = v2_create_instance(MODDIR, NULL);
    if (!inst) { ok(0, "instance created"); return; }
    char buf[4096];

    int n = v2_get_param(inst, "chain_params", buf, sizeof(buf));
    ok(n > 0 && n < (int)sizeof(buf), "chain_params fits its buffer");
    int depth = 0, maxd = 0;
    for (int i = 0; i < n; i++) {
        if (buf[i] == '[' || buf[i] == '{') { depth++; if (depth > maxd) maxd = depth; }
        if (buf[i] == ']' || buf[i] == '}') depth--;
    }
    ok(depth == 0 && maxd >= 2, "chain_params brackets balance");
    ok(strstr(buf, "\"unit\":\"%\"") != NULL, "percent unit emitted literally, not as %%");

    n = v2_get_param(inst, "ui_hierarchy", buf, sizeof(buf));
    ok(n > 0, "ui_hierarchy served");
    depth = 0;
    for (int i = 0; i < n; i++) {
        if (buf[i] == '{') depth++;
        if (buf[i] == '}') depth--;
    }
    ok(depth == 0, "ui_hierarchy braces balance");
    ok(strstr(buf, "\"editor\"") != NULL, "ui_hierarchy exposes the canvas editor");

    v2_destroy_instance(inst);
}

int main(void)
{
    printf("=== RRV-10 module tests ===\n");
    test_rom_loads();
    test_rate_is_31250();
    test_reverb_actually_decays();
    test_missing_rom_passes_audio();
    test_params_round_trip();
    test_state_save_restore();
    test_metadata_json_is_wellformed();
    printf("=== %s ===\n", g_fail ? "FAILURES" : "all tests passed");
    return g_fail ? 1 : 0;
}
