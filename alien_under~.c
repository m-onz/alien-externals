/* alien_under~.c  --  subharmonic / undertone synthesiser (organ from below)
 *
 * The mirror image of additive synthesis.  Additive stacks OVERTONES above a
 * fundamental (f, 2f, 3f, 4f...); this stacks SUBHARMONICS below it
 * (f, f/2, f/3, f/4 ... f/N).  The undertone series is what you get if you
 * reflect the harmonic series through the fundamental -- an "organ from below",
 * a chord that grows downward into the floor.  Ominous, uncanny, and physically
 * uncommon (real vibrating bodies overwhelmingly favour overtones), which is
 * exactly why it sounds alien.
 *
 * The incoming pitch is the TOP of the stack.  Each subharmonic k rings at
 * f/k, so higher k means deeper.  A spectral tilt slope shapes how loud the
 * deep partials sit against the top; negative tilt lets the sub-basement take
 * over.  Each subharmonic is an independent phase accumulator running at f/k,
 * so the partials never lock into a buzzy single cycle, and a per-partial
 * detune smears them into slow beating / chorus.
 *
 *   inlet 0   signal/float   fundamental pitch in Hz -- the TOP of the stack
 *                            (subharmonics are generated BELOW it)
 *   inlet 1   signal/float   MACRO 0..100 (wrapped mod 100) -- one knob that
 *                            morphs the whole voice: at 0 a single mellow sine
 *                            near the fundamental (few partials, positive tilt,
 *                            no detune or drive); climbing through a widening
 *                            undertone chord it opens downward into the floor,
 *                            switching sine->saw->square->pulse, deepening the
 *                            stack, tilting the deep partials louder, adding
 *                            chorus detune and finally overdrive, until at 100
 *                            it is a full 16-partial pulse-wave sub-organ with
 *                            heavy negative tilt, chorus and grit.
 *
 *   outlet 0  signal
 *
 * Bounded by construction: the sum is normalised by the summed partial
 * amplitude, DC-blocked, then softclipped, so it never NaNs or runs away.
 *
 * build:
 *   gcc -shared -fPIC -O2 -o alien_under~.pd_linux alien_under~.c -lm
 *   (macOS: -bundle -undefined dynamic_lookup -o alien_under~.pd_darwin)
 */

#include "m_pd.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TWOPI   (2.0 * M_PI)
#define MAXSUB  16
#define NWAVE   4

static t_class *alien_under_tilde_class;

typedef struct _alien_under {
    t_object x_obj;
    t_float  x_f;

    double   x_sr;
    double   x_freq;                 /* glided fundamental */
    double   x_phase[MAXSUB];        /* independent accumulators, one per f/k */
    double   x_det[MAXSUB];          /* per-partial detune factor */
    double   x_dc0, x_dc1;           /* DC blocker */
    double   x_lastspread;           /* detune table is recomputed on change */
    double   x_lastmac;              /* last macro value (block-rate morph) */
} t_alien_under;

static inline double softclip(double v)
{
    /* unity slope near 0, saturates toward +/-1; hard safety limit at +/-1.5 */
    if (v >  3.0) return 1.0;
    if (v < -3.0) return -1.0;
    v = v * (27.0 + v*v) / (27.0 + 9.0*v*v);
    if (v >  1.5) v =  1.5;
    if (v < -1.5) v = -1.5;
    return v;
}

/* one cycle of the chosen waveform from a phase in [0,1) */
static inline double waveshape(int w, double ph)
{
    switch (w) {
        case 1:  return 2.0 * ph - 1.0;                 /* saw */
        case 2:  return (ph < 0.5) ? 1.0 : -1.0;        /* square */
        case 3:  return (ph < 0.15) ? 1.0 : -1.0;       /* narrow pulse */
        default: return sin(TWOPI * ph);                /* sine */
    }
}

/* recompute per-partial detune multipliers from spread */
static void under_recalc(t_alien_under *x, double sp)
{
    int i;
    for (i = 0; i < MAXSUB; i++) {
        /* small, deterministic-per-partial detune, up to ~+/-3% at spread 1 */
        double d = sin((double)(i + 1) * 1.618);
        x->x_det[i] = 1.0 + sp * 0.03 * d;
    }
}

/* ------------------------------------------------------------------ */

/* map the 0..1 macro onto the whole parameter set (curated morph).
 * wave is discrete: sine -> saw -> square -> pulse across four sub-ranges. */
static inline void under_macro(double t, int *n, double *tilt, double *spread,
                               double *sub, double *drive, int *wave)
{
    /* few, then deep: 1 partial at 0 up to the full 16 at 100 */
    *n = (int)(1.0 + 15.0 * t + 0.5);
    if (*n < 1) *n = 1; if (*n > MAXSUB) *n = MAXSUB;

    /* tilt: +1 (deep partials faded, mellow) down to -1.5 (sub-basement roars) */
    *tilt = 1.0 - 2.5 * t;

    /* detune / chorus grows steadily toward the top */
    *spread = t * t;

    /* blend from fundamental-only into full undertone stack (open early) */
    *sub = (t < 0.75) ? (t / 0.75) : 1.0;

    /* grit only up top */
    *drive = (t < 0.6) ? 0.0 : (t - 0.6) / 0.4;

    /* discrete waveform: sine / saw / square / pulse across the sweep */
    if      (t < 0.25) *wave = 0;   /* sine   */
    else if (t < 0.50) *wave = 1;   /* saw    */
    else if (t < 0.75) *wave = 2;   /* square */
    else               *wave = 3;   /* pulse  */
}

static t_int *alien_under_perform(t_int *w)
{
    t_alien_under *x = (t_alien_under *)(w[1]);
    t_sample *fin = (t_sample *)(w[2]);
    t_sample *min = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    int nsamp = (int)(w[5]);

    double sr = x->x_sr;
    double inv_sr = 1.0 / sr;
    double nyq = sr * 0.49;

    /* ---- block-rate morph: read the macro ONCE from the first sample ----
     * The whole curated parameter set (partial count, tilt, sub, spread,
     * drive, discrete waveform) is derived per block, and the detune table is
     * re-seeded only when the macro actually moves.  This keeps pow()/sin()
     * table work and the discrete waveform select out of the per-sample loop,
     * which is what was melting the CPU and stuttering the patch. */
    double mac0 = (nsamp > 0) ? (double)min[0] : x->x_lastmac;
    double t, tilt, spread, sub, drive;
    int n, wv, i;
    double amp[MAXSUB];          /* per-partial amplitude, block-rate */
    double gsum = 0.0;

    /* wrap the macro into [0,100) then normalise to [0,1) */
    t = mac0 - 100.0 * floor(mac0 * 0.01);
    if (!(t >= 0.0 && t <= 100.0)) t = 0.0;       /* NaN/inf guard */
    t *= 0.01;
    under_macro(t, &n, &tilt, &spread, &sub, &drive, &wv);
    if (n < 1) n = 1;
    if (n > MAXSUB) n = MAXSUB;                    /* hard capacity clamp */

    /* recompute detune table only when spread actually moves */
    if (spread != x->x_lastspread) { under_recalc(x, spread); x->x_lastspread = spread; }
    x->x_lastmac = mac0;

    /* precompute per-partial amplitude (tilt + sub blend) once per block */
    for (i = 0; i < n; i++) {
        int k = i + 1;
        double a = pow((double)k, -tilt);          /* spectral tilt k^(-tilt) */
        if (k == 1) a *= (1.0 - sub) + sub;        /* fundamental always present */
        else        a *= sub;
        if (!isfinite(a) || a < 0.0) a = 0.0;
        amp[i] = a;
        gsum += a;
    }
    double inv_gsum = (gsum > 1e-6) ? (1.0 / gsum) : 0.0;
    double dgain = (drive > 0.0) ? (1.0 / (1.0 + drive * 1.5)) : 1.0;
    double ddrv  = 1.0 + drive * 6.0;

    while (nsamp--) {
        double ftarget = *fin++;
        double acc = 0.0, y, o;
        (void)*min++;                              /* macro consumed at block rate */

        if (ftarget < 0.01)   ftarget = 0.01;      /* forbid divide-by-zero */
        if (ftarget > 8000.0) ftarget = 8000.0;
        if (!isfinite(ftarget)) ftarget = 220.0;
        x->x_freq = ftarget;

        for (i = 0; i < n; i++) {
            int k = i + 1;                          /* partial index 1..n */
            double subf = x->x_freq / (double)k * x->x_det[i];
            double ph;

            if (subf < 10.0) subf = 10.0;          /* guard the sub-basement */
            if (subf > nyq) continue;              /* skip partials past Nyquist (anti-alias) */

            x->x_phase[i] += subf * inv_sr;
            if (x->x_phase[i] >= 1.0) x->x_phase[i] -= floor(x->x_phase[i]);
            ph = x->x_phase[i];

            acc += amp[i] * waveshape(wv, ph);
        }

        y = acc * inv_gsum;

        /* DC block (square/pulse/saw are not zero-mean, esp. with tilt) */
        o = y - x->x_dc0 + 0.999 * x->x_dc1;
        x->x_dc0 = y; x->x_dc1 = o;

        if (drive > 0.0)
            o = softclip(o * ddrv) * dgain;
        else
            o = softclip(o);

        if (!isfinite(o)) o = 0.0;                 /* NaN / Inf guard */
        *out++ = (t_sample)(o * 0.6);
    }
    return (w + 6);
}

static void alien_under_dsp(t_alien_under *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    dsp_add(alien_under_perform, 5, x,
            sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_under_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_under *x = (t_alien_under *)pd_new(alien_under_tilde_class);
    int i;
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    for (i = 0; i < MAXSUB; i++) x->x_phase[i] = 0.0;
    x->x_dc0 = x->x_dc1 = 0.0;
    x->x_freq = 220.0;
    x->x_lastspread = -1.0;
    x->x_lastmac = 0.0;
    under_recalc(x, 0.0);

    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);   /* macro */
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_under_tilde_setup(void)
{
    alien_under_tilde_class = class_new(gensym("alien_under~"),
        (t_newmethod)alien_under_new, 0, sizeof(t_alien_under),
        CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_under_tilde_class, t_alien_under, x_f);
    class_addmethod(alien_under_tilde_class, (t_method)alien_under_dsp, gensym("dsp"), A_CANT, 0);
}
