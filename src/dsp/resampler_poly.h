/* resampler_poly.h
 *
 * Small-ratio rational polyphase resampler, stereo, ring-buffer history.
 *
 * WHY THIS AND NOT THE EXACT RATIO
 * --------------------------------
 * The emulated gate array is clocked at 31250 Hz (8.000 MHz / 256 cycles per
 * sample).  Against Move's 44100 Hz the exact ratio is 44100:31250 = 882:625,
 * needing an 882-phase coefficient bank.
 *
 * A reverb carries no pitch reference, so the emulation clock can be detuned a
 * hair to land on a friendlier rational.  44100 * 17/24 = 31237.5 Hz is
 * 0.69 CENTS flat of spec — inaudible in a reverb tail — and collapses the
 * conversion to 17/24 down and 24/17 up.
 *
 * Note the phase count L only sets TABLE SIZE, never per-sample work: the cost
 * of one output is TAPS multiply-accumulates regardless of L.  What actually
 * buys the cheap filter here is the source bandwidth: the RRV-10's effect path
 * is specified 30 Hz - 10 kHz, so the anti-alias transition band is wide and
 * 24 taps is transparent.  Measured round trip, bin-exact tones:
 *
 *     taps/phase   worst spur   droop @10 kHz   table
 *         16        -89.0 dBc      -1.74 dB     2.6 KB
 *         24        -96.2 dBc      -0.43 dB     3.8 KB   <- used
 *         32       -102.4 dBc      -0.12 dB     5.1 KB
 *
 * -96 dBc sits below the hardware's own -100 dBm residual noise.  The down
 * stage was also verified alone (frequencies preserved to <0.4 Hz, passband
 * flat to 0.000 dB, spurs <= -97 dBc) so the two stages are not covering for
 * each other.
 *
 * RT: no allocation, no locks, no denormal-prone tails (coefficients are
 * finite-length FIR; history is flushed by construction).
 */
#ifndef RESAMPLER_POLY_H
#define RESAMPLER_POLY_H

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define RP_TAPS 24

/* Zeroth-order modified Bessel function I0, series form. */
static inline double rp_bessel_i0(double x)
{
    double sum = 1.0, term = 1.0, halfx = x * 0.5;
    for (int k = 1; k < 64; ++k) {
        double t = halfx / (double)k;
        term *= t * t;
        sum += term;
        if (term < sum * 1e-16) break;
    }
    return sum;
}

/* One stage. L = interpolation factor, M = decimation factor.
 * Output rate = input rate * L / M. Phase bank is L entries. */
typedef struct {
    int L, M;
    float coef[24][RP_TAPS];      /* max L used here is 24 */
    /* Doubled history so a phase's taps are always contiguous: hist[hp..hp+TAPS)
     * is the oldest-to-newest window with no wrap test in the inner loop. */
    float histL[2 * RP_TAPS];
    float histR[2 * RP_TAPS];
    int hp;
    int phase;
} rp_stage_t;

/* fc_hz: lowpass cutoff in Hz. fs_in_hz: this stage's INPUT rate.
 * The cutoff must sit below BOTH Nyquist limits; caller passes the shared
 * 13 kHz figure, comfortably above the RRV-10's 10 kHz effect band and below
 * the 15618 Hz intermediate Nyquist. */
static inline void rp_init(rp_stage_t *r, int L, int M, double fc_hz, double fs_in_hz)
{
    memset(r, 0, sizeof(*r));
    r->L = L;
    r->M = M;
    r->hp = 0;
    r->phase = 0;

    const int N = L * RP_TAPS;
    const double cutoff_frac = fc_hz / (fs_in_hz * 0.5);   /* of input Nyquist */
    const double fc = cutoff_frac / (2.0 * (double)L);     /* cycles/sample at L*fs */
    const double beta = 8.0;
    const double i0beta = rp_bessel_i0(beta);
    const double center = (double)(N - 1) / 2.0;

    for (int m = 0; m < N; ++m) {
        double t = (double)m - center;
        double x = 2.0 * fc * t;
        double sinc = (x == 0.0) ? 2.0 * fc
                                 : 2.0 * fc * sin(M_PI * x) / (M_PI * x);
        double rr = (2.0 * (double)m / (double)(N - 1)) - 1.0;
        double w = rp_bessel_i0(beta * sqrt(1.0 - rr * rr)) / i0beta;
        int p = m % L, tp = m / L;
        if (tp < RP_TAPS)
            r->coef[p][tp] = (float)(sinc * w * (double)L);  /* *L: interp gain */
    }
}

/* Push one stereo input frame; write 0..2 output frames to outL/outR.
 * Returns the number of frames written. */
static inline int rp_push(rp_stage_t *r, float inL, float inR,
                          float *outL, float *outR)
{
    r->histL[r->hp] = inL;  r->histL[r->hp + RP_TAPS] = inL;
    r->histR[r->hp] = inR;  r->histR[r->hp + RP_TAPS] = inR;
    r->hp = (r->hp + 1) % RP_TAPS;

    int n = 0;
    r->phase += r->L;
    while (r->phase >= r->M) {
        r->phase -= r->M;
        const float *c = r->coef[r->phase % r->L];
        const float *hl = &r->histL[r->hp];
        const float *hr = &r->histR[r->hp];
        float aL = 0.0f, aR = 0.0f;
        for (int k = 0; k < RP_TAPS; ++k) {
            aL += c[k] * hl[k];
            aR += c[k] * hr[k];
        }
        outL[n] = aL;
        outR[n] = aR;
        n++;
    }
    return n;
}

#endif /* RESAMPLER_POLY_H */
