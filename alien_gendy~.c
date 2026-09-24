/* alien_gendy~.c  --  dynamic stochastic synthesis (Xenakis GENDYN)
 *
 * The waveform is a polyline of N breakpoints.  Every cycle, each
 * breakpoint takes a small random step in amplitude and in duration,
 * bounded by mirror barriers, and the new polyline is what you hear on
 * the next cycle.  The shape never settles and never repeats, but it is
 * never random either -- it wanders, because each step starts from where
 * the last one left off.  Xenakis, GENDYN, 1991.
 *
 * Two barriers, as in the original: amplitude is mirrored into -1..1 and
 * duration into a set window, so the walk is bounded without being
 * clipped -- it bounces rather than sticks.
 *
 *   inlet 0   signal/float   frequency in Hz (the cycle rate / pitch)
 *   inlet 1   signal/float   MACRO 0..100 (wrapped mod 100) -- one knob that
 *                            morphs the whole voice along a hand-tuned
 *                            trajectory.  At 0 the walk is almost frozen and
 *                            gravity locks a few breakpoints onto a smooth
 *                            sine, so you hear a near-pure, mellow tone.  As
 *                            it rises the gravity releases, the amplitude and
 *                            duration steps widen, the breakpoint count and
 *                            drive climb and the interpolation sharpens from
 *                            cubic to linear -- so at 100 it is a dense,
 *                            free-wandering, aggressive GENDY spray.  It sweeps
 *                            n/astep/dstep/grav/target/interp/drive together.
 *
 *   outlet 0  signal
 *   outlet 1  list           the breakpoint amplitudes on [dump(, for
 *                            drawing the living waveform in GEM
 *
 * build:
 *   gcc -shared -fPIC -O2 -o alien_gendy~.pd_linux alien_gendy~.c -lm
 *   (macOS: -bundle -undefined dynamic_lookup -o alien_gendy~.pd_darwin)
 */

#include "m_pd.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAXBP  64
#define DMIN   0.1      /* smallest allowed segment duration weight */
#define DMAX   1.0

static t_class *alien_gendy_tilde_class;

typedef struct _alien_gendy {
    t_object x_obj;
    t_float  x_f;

    double   x_sr;
    double   x_ph;              /* cycle phase 0..1 */
    int      x_seg;            /* current segment index */

    double   x_amp[MAXBP];      /* breakpoint amplitudes, -1..1 */
    double   x_dur[MAXBP];      /* breakpoint duration weights */
    double   x_cum[MAXBP + 1];  /* cumulative normalised durations, [0]=0 [n]=1 */
    int      x_n;

    double   x_dc0, x_dc1;      /* DC-blocker history */
    unsigned x_rng;
    int      x_lastn;           /* last breakpoint count, to detect change */

    /* per-block cache of the curated macro parameters */
    double   x_bl_astep, x_bl_dstep, x_bl_grav, x_bl_drive;
    int      x_bl_target, x_bl_interp;
} t_alien_gendy;

static unsigned rn(t_alien_gendy *x)
{
    x->x_rng ^= x->x_rng << 13; x->x_rng ^= x->x_rng >> 17;
    x->x_rng ^= x->x_rng << 5;  return x->x_rng;
}
static double frnd(t_alien_gendy *x)  /* -1..1 */
{
    return (double)(rn(x) & 0xffffff) / (double)0x800000 - 1.0;
}

static inline double softclip(double v)
{
    if (v >  3.0) return 1.0;
    if (v < -3.0) return -1.0;
    return v * (27.0 + v*v) / (27.0 + 9.0*v*v);
}

/* mirror a value into [lo,hi] by reflection */
static double mirror(double v, double lo, double hi)
{
    double span = hi - lo, t;
    if (span <= 0.0) return lo;
    t = fmod(v - lo, 2.0 * span);
    if (t < 0.0) t += 2.0 * span;
    if (t > span) t = 2.0 * span - t;
    return lo + t;
}

static double target_val(int target, int i, int n)
{
    double t = (n > 1) ? (double)i / (double)n : 0.0;
    switch (target) {
        case 1:  return sin(2.0 * M_PI * t);       /* sine */
        case 2:  return (t < 0.5) ? 1.0 : -1.0;    /* square */
        case 3:  return 2.0 * t - 1.0;             /* saw */
        default: return 0.0;                        /* flat */
    }
}

static void gendy_recum(t_alien_gendy *x)
{
    int i, n = x->x_n;
    double total = 0.0;
    for (i = 0; i < n; i++) total += x->x_dur[i];
    if (total < 1e-9) total = 1.0;
    x->x_cum[0] = 0.0;
    for (i = 0; i < n; i++)
        x->x_cum[i + 1] = x->x_cum[i] + x->x_dur[i] / total;
    x->x_cum[n] = 1.0;
}

/* one stochastic step of the whole polyline, run at each cycle wrap */
static void gendy_evolve(t_alien_gendy *x, double astep, double dstep,
                         double grav, int target)
{
    int i, n = x->x_n;
    if (astep < 0.0) astep = 0.0;
    if (dstep < 0.0) dstep = 0.0;
    for (i = 0; i < n; i++) {
        double a = x->x_amp[i] + astep * frnd(x);
        a = mirror(a, -1.0, 1.0);
        if (grav > 0.0)
            a += (target_val(target, i, n) - a) * grav;
        x->x_amp[i] = a;

        x->x_dur[i] = mirror(x->x_dur[i] + dstep * frnd(x), DMIN, DMAX);
    }
    gendy_recum(x);
}

static void gendy_scatter(t_alien_gendy *x)
{
    int i;
    for (i = 0; i < x->x_n; i++) {
        x->x_amp[i] = frnd(x);
        x->x_dur[i] = DMIN + (DMAX - DMIN) * (0.5 + 0.5 * frnd(x));
    }
    gendy_recum(x);
    x->x_ph = 0.0; x->x_seg = 0;
}

/* grow the polyline to n breakpoints, initialising any new ones */
static void gendy_setn(t_alien_gendy *x, int n)
{
    int i, old = x->x_n;
    n = (n < 2) ? 2 : (n > MAXBP ? MAXBP : n);
    if (n == old) return;
    for (i = old; i < n; i++) {          /* init any new breakpoints */
        x->x_amp[i] = frnd(x);
        x->x_dur[i] = 0.5 * (DMIN + DMAX);
    }
    x->x_n = n;
    gendy_recum(x);
    x->x_seg = 0;
}

/* ------------------------------------------------------------------ */

/* map the 0..1 macro onto the whole parameter set (curated morph) */
static inline void gendy_macro(double t, int *n, double *astep, double *dstep,
                               double *grav, int *target, int *interp,
                               double *drive)
{
    /* few, smooth breakpoints down low; many jagged ones up top.
       clamp hard so the count can never exceed the allocated arrays */
    *n     = (int)(6.0 + 52.0 * t);
    if (*n < 2)     *n = 2;
    if (*n > MAXBP) *n = MAXBP;
    /* the walk: nearly frozen at 0, wide and noisy at 1 */
    *astep = 0.004 + 0.7 * t * t;
    *dstep = 0.6 * t * t * t;                 /* spacing stays fixed until late */
    /* gravity locks the shape to a target at the bottom, releases toward the top */
    *grav  = (t < 0.5) ? (0.9 * (1.0 - t / 0.5)) : 0.0;
    *target = 1;                              /* smooth sine anchor */
    *interp = (t < 0.6) ? 1 : 0;              /* cubic (dark) low, linear (sharp) high */
    *drive = (t < 0.6) ? 0.0 : (t - 0.6) / 0.4;   /* grit only up top */
}

static t_int *alien_gendy_perform(t_int *w)
{
    t_alien_gendy *x = (t_alien_gendy *)(w[1]);
    t_sample *fin  = (t_sample *)(w[2]);
    t_sample *min  = (t_sample *)(w[3]);
    t_sample *out  = (t_sample *)(w[4]);
    int nsamp = (int)(w[5]);

    double sr = x->x_sr;

    /* ---- read discrete/curated quantities ONCE PER BLOCK ---------------
     * the macro drives a set of DISCRETE choices (breakpoint count,
     * interpolation mode, target) and a handful of walk parameters that are
     * only consumed on a cycle wrap.  Deriving them from the macro's first
     * sample per block keeps the DSP loop cheap and, crucially, stops the
     * polyline from being reseeded on every single sample (the old code
     * called gendy_setn() per sample -> constant stutter and CPU blow-up). */
    {
        double mac0 = *min;
        double t0 = mac0 - 100.0 * floor(mac0 * 0.01);
        int nn, target, interp;
        double astep, dstep, grav, drive;
        if (!(t0 >= 0.0 && t0 <= 100.0)) t0 = 0.0;   /* NaN/inf guard */
        t0 *= 0.01;
        gendy_macro(t0, &nn, &astep, &dstep, &grav, &target, &interp, &drive);

        /* only reallocate/reseed the polyline on an ACTUAL count change */
        if (nn != x->x_lastn) {
            gendy_setn(x, nn);
            x->x_lastn = x->x_n;
        }

        x->x_bl_astep  = astep;
        x->x_bl_dstep  = dstep;
        x->x_bl_grav   = grav;
        x->x_bl_target = target;
        x->x_bl_interp = interp;
        x->x_bl_drive  = drive;
    }

    {
    int   n      = x->x_n;
    int   interp = x->x_bl_interp;
    double drive = x->x_bl_drive;

    while (nsamp--) {
        double f0 = *fin++;
        double ph, y, frac, denom;
        int s;

        (void)*min++;   /* macro already consumed per-block */

        if (f0 < 0.01)   f0 = 0.01;    /* forbid divide-by-zero / runaway */
        if (f0 > 8000.0) f0 = 8000.0;
        if (!isfinite(f0)) f0 = 0.01;

        x->x_ph += f0 / sr;
        if (x->x_ph >= 1.0) {
            x->x_ph -= 1.0;
            if (x->x_ph >= 1.0) x->x_ph = 0.0;   /* very high f0 guard */
            gendy_evolve(x, x->x_bl_astep, x->x_bl_dstep,
                         x->x_bl_grav, x->x_bl_target);
            x->x_seg = 0;
        }
        ph = x->x_ph;

        /* keep the cached segment index in range even if n just changed */
        s = x->x_seg;
        if (s < 0)      s = 0;
        if (s > n - 1)  s = n - 1;
        /* advance to the segment containing ph */
        while (s < n - 1 && ph >= x->x_cum[s + 1]) s++;
        while (s > 0 && ph < x->x_cum[s]) s--;
        x->x_seg = s;

        denom = x->x_cum[s + 1] - x->x_cum[s];
        frac = (denom > 1e-9) ? (ph - x->x_cum[s]) / denom : 0.0;
        if (frac < 0.0) frac = 0.0;
        if (frac > 1.0) frac = 1.0;

        if (interp) {
            /* Catmull-Rom over the four surrounding breakpoints (wrap) */
            double a0 = x->x_amp[(s - 1 + n) % n];
            double a1 = x->x_amp[s];
            double a2 = x->x_amp[(s + 1) % n];
            double a3 = x->x_amp[(s + 2) % n];
            double tt = frac, t2 = tt * tt, t3 = t2 * tt;
            y = 0.5 * ((2.0 * a1) +
                       (-a0 + a2) * tt +
                       (2.0*a0 - 5.0*a1 + 4.0*a2 - a3) * t2 +
                       (-a0 + 3.0*a1 - 3.0*a2 + a3) * t3);
        } else {
            double a1 = x->x_amp[s];
            double a2 = x->x_amp[(s + 1) % n];
            y = a1 + frac * (a2 - a1);
        }

        /* the polyline can drift fully to one side of zero; block DC so
         * it never leaves a standing offset on the output */
        {
            double o = y - x->x_dc0 + 0.999 * x->x_dc1;
            x->x_dc0 = y; x->x_dc1 = o; y = o;
        }
        if (drive > 0.0)
            y = softclip(y * (1.0 + drive * 6.0)) / (1.0 + drive * 2.0);

        /* final safety net: never let a NaN/Inf escape, and keep the
         * output softly bounded to roughly [-1.5, 1.5].  The magnitude
         * test catches blow-ups even under -ffast-math, where isfinite()
         * may be optimised away (NaN fails every comparison, so !(|y|<big)
         * is true for NaN too). */
        if (!isfinite(y) || !(y > -1e6 && y < 1e6)) {
            y = 0.0;
            x->x_dc0 = x->x_dc1 = 0.0;   /* stop a bad value poisoning history */
        }
        y *= 0.9;
        if (y >  1.5) y =  1.5;
        else if (y < -1.5) y = -1.5;
        *out++ = (t_sample)(y * 0.49);
    }
    }
    return (w + 6);
}

static void alien_gendy_dsp(t_alien_gendy *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    if (x->x_sr < 1000.0) x->x_sr = 44100.0;   /* never divide by ~0 */
    dsp_add(alien_gendy_perform, 5, x,
            sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_gendy_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_gendy *x = (t_alien_gendy *)pd_new(alien_gendy_tilde_class);
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    x->x_n = 12;
    x->x_dc0 = x->x_dc1 = 0.0;
    x->x_rng = 22222u; x->x_ph = 0.0; x->x_seg = 0;
    x->x_lastn = 0;             /* force setn on first block */
    x->x_bl_astep = x->x_bl_dstep = x->x_bl_grav = x->x_bl_drive = 0.0;
    x->x_bl_target = 1; x->x_bl_interp = 1;

    gendy_scatter(x);

    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_gendy_tilde_setup(void)
{
    alien_gendy_tilde_class = class_new(gensym("alien_gendy~"),
        (t_newmethod)alien_gendy_new, 0, sizeof(t_alien_gendy),
        CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_gendy_tilde_class, t_alien_gendy, x_f);
    class_addmethod(alien_gendy_tilde_class, (t_method)alien_gendy_dsp,
                    gensym("dsp"), A_CANT, 0);
}
