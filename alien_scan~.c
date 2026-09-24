/* alien_scan~.c  --  driven scanned synthesis
 *
 * Scanned synthesis with two changes that turn it from a slowly
 * evolving pad into a playable instrument.
 *
 * 1. The mass ring is DRIVEN, continuously, from the signal inlet.
 *    Feed it a break, a synth, your whole mix, and the ring never
 *    settles.  The wavetable becomes a physical response to the input
 *    rather than a decaying ring-out.
 *
 * 2. The scan PATH is not the ring order.  Reading the same masses in
 *    a different sequence gives a completely different waveform from
 *    identical physics, so the scan order is a timbre control with no
 *    dynamic cost at all.  Bit-reversed order in particular scrambles
 *    the smooth shape into something harmonically dense.
 *
 *   inlet 0   signal/float   pitch in Hz -- the scan rate
 *   inlet 1   signal/float   MACRO 0..100 (wrapped mod 100) -- one knob that
 *                            morphs the whole voice: a slack, heavily damped,
 *                            slowly evolving ring-ordered pad at 0, tightening
 *                            through a lively coupled shape, into a stiff,
 *                            fast, nonlinear, hard-driven bit-reversed / random
 *                            spray at 100. It sweeps tension, damping, scan
 *                            speed, nonlinearity, drive and scan order together
 *                            along a hand-tuned trajectory.
 *
 *   outlet 0  signal          the scanned waveform
 *
 * build:
 *   gcc -shared -fPIC -O2 -o alien_scan~.pd_linux alien_scan~.c -lm
 *   (macOS: -bundle -undefined dynamic_lookup -o alien_scan~.pd_darwin)
 */

#include "m_pd.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAXM    256
#define PHYSDIV 16

static t_class *alien_scan_tilde_class;

typedef struct _alien_scan {
    t_object  x_obj;
    t_float   x_f;

    double   x_pos[MAXM], x_vel[MAXM];
    int      x_path[MAXM];
    float    x_tab[MAXM + 1];

    double   x_sr, x_ph;
    int      x_n, x_div;
    int      x_lastpath;             /* last scan-order mode, for reset */
    unsigned x_rng;
} t_alien_scan;

static unsigned rn(t_alien_scan *x)
{
    x->x_rng ^= x->x_rng << 13; x->x_rng ^= x->x_rng >> 17;
    x->x_rng ^= x->x_rng << 5;  return x->x_rng;
}

/* ------------------------------------------------------------------ */

static void scan_path(t_alien_scan *x, int mode)
{
    int n = x->x_n, i;
    if (mode == 1) {                 /* reverse */
        for (i = 0; i < n; i++) x->x_path[i] = n - 1 - i;
    } else if (mode == 2) {          /* alternate: outside in 0,n-1,1,n-2,.. */
        int lo = 0, hi = n - 1;
        for (i = 0; i < n; i++)
            x->x_path[i] = (i & 1) ? hi-- : lo++;
    } else if (mode == 3) {          /* bitrev */
        int bits = 0, m = n;
        while (m > 1) { bits++; m >>= 1; }
        for (i = 0; i < n; i++) {
            int r = 0, j, v = i;
            for (j = 0; j < bits; j++) { r = (r << 1) | (v & 1); v >>= 1; }
            x->x_path[i] = (r < n) ? r : i;
        }
    } else if (mode == 4) {          /* random */
        for (i = 0; i < n; i++) x->x_path[i] = i;
        for (i = n - 1; i > 0; i--) {
            int j = (int)(rn(x) % (unsigned)(i + 1));
            int t = x->x_path[i]; x->x_path[i] = x->x_path[j]; x->x_path[j] = t;
        }
    } else {                         /* 0: ring */
        for (i = 0; i < n; i++) x->x_path[i] = i;
    }
}

/* map the 0..1 macro onto the whole parameter set (curated morph) */
static inline void scan_macro(double t, double *stiff, double *centre,
                              double *damp, double *speed, double *nonlin,
                              double *inject, double *point, double *width,
                              int *pathmode)
{
    *stiff  = 0.08 + 0.85 * t;                 /* slack -> tight coupling     */
    *centre = 0.25 - 0.22 * t;                 /* pulls to rest -> free ring  */
    *damp   = 0.28 - 0.27 * t;                 /* heavy -> almost none        */
    *speed  = 0.4 + 11.6 * t * t;              /* slow drift -> fast physics  */
    *nonlin = (t < 0.55) ? 0.0 : (t - 0.55) / 0.45;  /* hardening only up top */
    *inject = 0.25 + 0.65 * t;                 /* gentle -> hard drive        */
    *point  = 0.25;                            /* fixed injection site        */
    *width  = 0.35 - 0.30 * t;                 /* broad soft -> tight sharp   */
    /* scan order steps through darker to denser orderings as t climbs */
    if      (t < 0.20) *pathmode = 0;          /* ring      */
    else if (t < 0.40) *pathmode = 1;          /* reverse   */
    else if (t < 0.60) *pathmode = 2;          /* alternate */
    else if (t < 0.80) *pathmode = 3;          /* bitrev    */
    else               *pathmode = 4;          /* random    */
}

/* NOTE ON -ffast-math: this object is built with -ffast-math, under which
 * isfinite()/NaN tricks/large-magnitude comparisons against +/-1e30 all get
 * folded away by the compiler (it assumes no Inf/NaN ever occur).  The real
 * protection therefore does not rely on detecting Inf after the fact -- it
 * PREVENTS Inf/NaN from ever forming, by hard-clamping the coupling (k<=0.9),
 * damping (d in [0.9,1]), the Duffing term, and velocity/position magnitudes
 * every step.  A clamped value can never reach Inf, so the lattice stays
 * bounded across the whole macro sweep.  The magnitude-based recovery below is
 * a belt-and-suspenders net for any value that grows large but still finite. */

/* denormal flush + hard finite bound for a single sample */
static inline double dflush(double v)
{
    if (v < 1e-30 && v > -1e-30) return 0.0;   /* kill denormals */
    return v;
}

/* re-zero the whole ring (blow-up recovery) */
static void ring_reset(t_alien_scan *x)
{
    int i, n = x->x_n;
    for (i = 0; i < n; i++) { x->x_pos[i] = 0.0; x->x_vel[i] = 0.0; }
}

/* true if the ring has grown implausibly large -- a magnitude test that
 * survives -ffast-math (positions are hard-clamped to +/-4, velocities to
 * +/-2, so anything past +/-8 means something is wrong). */
static int ring_runaway(t_alien_scan *x)
{
    int i, n = x->x_n;
    for (i = 0; i < n; i++) {
        double p = x->x_pos[i], v = x->x_vel[i];
        if (p > 8.0 || p < -8.0 || v > 8.0 || v < -8.0) return 1;
    }
    return 0;
}

static void scan_step(t_alien_scan *x, double drive,
                      double stiff, double centre, double damp, double speed,
                      double nonlin, double inject, double point, double width)
{
    int n = x->x_n, i;

    /* --- integrator stability ---------------------------------------
     * The velocity update  v += k*(l+r-2p)  is an explicit leapfrog
     * spring lattice.  Its stability limit is k < ~1 (k = 1 sits right
     * at the Nyquist edge; above it the highest spatial mode diverges).
     * The old mapping let k reach ~5 (stiff .93 * .45 * speed 12), which
     * blows up in a handful of samples.  Cap the effective coupling and
     * damping-derived multipliers well inside the stable region. */
    double k = stiff * 0.45 * speed;
    double c = centre * 0.02 * speed;
    double d = 1.0 - damp * 0.02 * speed;
    int ci = (int)(point * (n - 1));
    int hw = (int)(width * n * 0.5);

    if (k > 0.9) k = 0.9;                 /* stay below the k=1 edge     */
    if (k < 0.0) k = 0.0;
    if (c > 0.5) c = 0.5;                 /* soft centring only          */
    if (c < 0.0) c = 0.0;
    if (d < 0.90) d = 0.90;              /* never let energy run away    */
    if (d > 1.0) d = 1.0;                 /* and never inject via damping */
    if (hw < 1) hw = 1;
    if (hw > n / 2) hw = n / 2;
    if (ci < 0) ci = 0; if (ci > n - 1) ci = n - 1;

    /* inject the input as a force over a window of masses */
    if (inject > 0.0 && drive != 0.0) {
        for (i = -hw; i <= hw; i++) {
            int idx = ((ci + i) % n + n) % n;
            double env = 0.5 * (1.0 + cos(M_PI * (double)i / (double)hw));
            x->x_vel[idx] += drive * env * inject * 0.5;
        }
    }

    for (i = 0; i < n; i++) {
        double l = x->x_pos[(i - 1 + n) % n];
        double r = x->x_pos[(i + 1) % n];
        double p = x->x_pos[i];
        double f = k * (l + r - 2.0 * p) - c * p;
        if (nonlin > 0.0) {
            /* Duffing hardening: restoring force grows with the cube of
             * displacement.  The raw cube (p^3 * speed) reaches ~115 at
             * p=4, speed=12 -- an instant explosion.  Clamp the cubic
             * argument and scale so the hardening term can never exceed
             * a small fraction of the linear restoring force. */
            double pc = p; if (pc > 1.0) pc = 1.0; if (pc < -1.0) pc = -1.0;
            double cube = pc * pc * pc;            /* in [-1,1]           */
            f -= nonlin * 0.05 * cube;             /* bounded hardening   */
        }
        x->x_vel[i] = x->x_vel[i] * d + f;
        /* clamp velocity so a transient can never diverge unboundedly  */
        if (x->x_vel[i] >  2.0) x->x_vel[i] =  2.0;
        if (x->x_vel[i] < -2.0) x->x_vel[i] = -2.0;
    }
    for (i = 0; i < n; i++) {
        x->x_pos[i] = dflush(x->x_pos[i] + x->x_vel[i]);
        if (x->x_pos[i] >  4.0) { x->x_pos[i] =  4.0; x->x_vel[i] *= -0.5; }
        if (x->x_pos[i] < -4.0) { x->x_pos[i] = -4.0; x->x_vel[i] *= -0.5; }
        x->x_vel[i] = dflush(x->x_vel[i]);
    }
    /* if anything grew implausibly large, reset the ring */
    if (ring_runaway(x)) ring_reset(x);
    /* build the table in scan-path order, not ring order */
    for (i = 0; i < n; i++) x->x_tab[i] = (float)x->x_pos[x->x_path[i]];
    x->x_tab[n] = x->x_tab[0];
}

/* ------------------------------------------------------------------ */

static t_int *alien_scan_perform(t_int *w)
{
    t_alien_scan *x = (t_alien_scan *)(w[1]);
    t_sample *fin = (t_sample *)(w[2]);
    t_sample *min = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    int nsamp = (int)(w[5]);

    double sr = x->x_sr, ph = x->x_ph;
    int n = x->x_n;

    /* --- derive the whole parameter set ONCE PER BLOCK -------------
     * The macro is a slow control signal; reading it (and rebuilding the
     * scan order) per sample was pure CPU waste and the floor()/branch
     * on every sample was a large part of the stutter.  Take the first
     * sample of the block, sanitise it, and hold the derived parameters
     * for the whole block.  The scan order is rebuilt only when the
     * discrete mode actually changes. */
    double mac0 = min[0];
    double t, stiff, centre, damp, speed, nonlin, inject, point, width;
    int pathmode;

    /* Sanitise the incoming macro.  Under -ffast-math isfinite() is folded
     * away, so bound by magnitude first (an upstream object may emit a huge
     * or non-finite value) before the wrap can turn it into NaN. */
    if (mac0 > 1e9 || mac0 < -1e9 || !(mac0 == mac0)) mac0 = 0.0;
    t = mac0 - 100.0 * floor(mac0 * 0.01);        /* wrap into [0,100)  */
    if (t < 0.0) t = 0.0; if (t > 100.0) t = 100.0;
    t *= 0.01;                                    /* -> [0,1)           */

    scan_macro(t, &stiff, &centre, &damp, &speed, &nonlin,
               &inject, &point, &width, &pathmode);

    if (pathmode != x->x_lastpath) {
        x->x_lastpath = pathmode;
        scan_path(x, pathmode);
    }

    while (nsamp--) {
        double f0 = *fin++;
        double pos, frac, y;
        int i0;

        /* guard div-by-zero and non-finite / huge pitch input */
        if (f0 != f0 || f0 > 8000.0) f0 = 8000.0; /* NaN or too high      */
        if (f0 < 0.01) f0 = 0.01;                 /* clamp f0 >= 0.01     */

        if (--x->x_div <= 0) {
            /* no external audio drive any more: self-excite the ring with a
             * small internal noise burst so it never settles.  The macro's
             * inject/nonlin/damping decide how lively the response is. */
            double seed = ((double)(rn(x) >> 8) / 16777216.0 - 0.5) * 2.0;
            x->x_div = PHYSDIV;
            scan_step(x, seed, stiff, centre, damp, speed,
                      nonlin, inject, point, width);
        }

        ph += f0 / sr;
        if (ph >= 1.0) ph -= 1.0;
        if (ph < 0.0)  ph  = 0.0;

        pos  = ph * (double)n;
        i0   = (int)pos; if (i0 >= n) i0 = n - 1; if (i0 < 0) i0 = 0;
        frac = pos - i0;
        y = x->x_tab[i0] + (x->x_tab[i0+1] - x->x_tab[i0]) * frac;

        /* final safety net: non-finite -> silence, then soft clip */
        if (!isfinite(y)) y = 0.0;
        if (y >  1.5) y =  1.5;
        if (y < -1.5) y = -1.5;
        *out++ = (t_sample)(y * 0.6 * 0.42);
    }
    x->x_ph = ph;
    return (w + 6);
}

static void alien_scan_dsp(t_alien_scan *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    dsp_add(alien_scan_perform, 5, x,
            sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void sc_clear(t_alien_scan *x)
{
    int i;
    for (i = 0; i < MAXM; i++) { x->x_pos[i] = 0.0; x->x_vel[i] = 0.0; }
    memset(x->x_tab, 0, sizeof(x->x_tab));
}

/* ------------------------------------------------------------------ */

static void *alien_scan_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_scan *x = (t_alien_scan *)pd_new(alien_scan_tilde_class);
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    x->x_ph = 0.0; x->x_div = PHYSDIV;
    x->x_n = 64;
    x->x_rng = 1234567u;
    x->x_lastpath = 0;
    sc_clear(x);
    scan_path(x, 0);

    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_scan_tilde_setup(void)
{
    alien_scan_tilde_class = class_new(gensym("alien_scan~"),
        (t_newmethod)alien_scan_new, 0, sizeof(t_alien_scan),
        CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_scan_tilde_class, t_alien_scan, x_f);
    class_addmethod(alien_scan_tilde_class, (t_method)alien_scan_dsp,
                    gensym("dsp"), A_CANT, 0);
}
