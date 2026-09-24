/* alien_mass~.c  --  scanned synthesis
 *
 * A ring of masses joined by springs is set vibrating at a SLOW rate --
 * a few hertz, not audio rate -- and a phase accumulator scans the
 * shape of the ring as a wavetable at audio rate.
 *
 * The consequence is the whole point: pitch and timbre are completely
 * decoupled.  The pitch comes from the scanning speed, the timbre from
 * the physics, and because the physics never settles into a repeat, the
 * tone evolves continuously without an LFO and never sounds programmed.
 * Verplank, Mathews and Shaw, 2000.
 *
 *   inlet 0   signal/float   fundamental in Hz
 *   inlet 1   signal/float   MACRO 0..100 (wrapped mod 100) -- one knob that
 *                            morphs the whole voice: at 0 the ring is a heavy,
 *                            slow, tightly-damped sine that barely moves --
 *                            dull and mellow; sweeping up loosens the damping,
 *                            stiffens and speeds the springs, and re-strikes the
 *                            ring with brighter rest shapes (sine -> pulse ->
 *                            random) so at 100 it is a fast, stiff, nearly
 *                            undamped ring that clangs and rings inharmonically.
 *                            It sweeps damp/stiff/centre/speed/count and the
 *                            strike shape together along a hand-tuned trajectory.
 *
 *   outlet 0  signal
 *
 * build:
 *   gcc -shared -fPIC -O2 -o alien_mass~.pd_linux alien_mass~.c -lm
 *   (macOS: -bundle -undefined dynamic_lookup -o alien_mass~.pd_darwin)
 */

#include "m_pd.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAXM     256
#define MINM     8
#define PHYSDIV  32          /* physics runs every 32 audio samples */
#define NSHAPE   4

/* hard safety bound on every mass position/velocity; anything past this and
 * we treat the chain as blown up and reset it */
#define STATEMAX  8.0
#define BLOWUP    1.0e6

static t_class *alien_mass_tilde_class;

typedef struct _alien_mass {
    t_object  x_obj;
    t_float   x_f;

    double   x_pos[MAXM], x_vel[MAXM];
    float    x_tab[MAXM + 1];

    double   x_sr, x_ph;
    int      x_n, x_div;
    int      x_shape;           /* current rest shape, for change detection */
    unsigned x_rng;
} t_alien_mass;

static unsigned rn(t_alien_mass *x)
{
    x->x_rng ^= x->x_rng << 13; x->x_rng ^= x->x_rng >> 17;
    x->x_rng ^= x->x_rng << 5;  return x->x_rng;
}

/* flush a denormal / non-finite double to a finite, safe value */
static inline double safe_d(double v)
{
    if (!isfinite(v)) return 0.0;
    if (v >  STATEMAX) return  STATEMAX;
    if (v < -STATEMAX) return -STATEMAX;
    /* denormal flush */
    if (v > -1.0e-15 && v < 1.0e-15) return 0.0;
    return v;
}

/* ------------------------------------------------------------------ */

/* (re)strike the ring into a rest shape, injecting energy so the macro
 * sweep keeps the ring alive as it moves between shapes.  n is already
 * clamped to [MINM,MAXM] by the caller. */
static void mass_strike(t_alien_mass *x, int idx)
{
    int n = x->x_n, i;
    if (n < MINM) n = MINM;
    if (n > MAXM) n = MAXM;
    x->x_n = n;
    idx = (idx % NSHAPE + NSHAPE) % NSHAPE;
    for (i = 0; i < n; i++) {
        double t = (double)i / (double)n;
        double v;
        switch (idx) {
            case 0: v = sin(t * 2.0 * M_PI); break;                 /* sine  */
            case 1: v = sin(t * 4.0 * M_PI); break;                 /* two   */
            case 2: v = (t < 0.1) ? 1.0 : 0.0; break;               /* pulse */
            default: v = ((rn(x) & 0xffff) / 32768.0 - 1.0); break; /* rand  */
        }
        x->x_pos[i] = v;
        x->x_vel[i] = 0.0;
        x->x_tab[i] = (float)v;
    }
    x->x_tab[n] = x->x_tab[0];
    x->x_shape = idx;
}

/* reset the whole chain to a quiet, finite state (used on blow-up) */
static void mass_reset(t_alien_mass *x)
{
    int i;
    for (i = 0; i < MAXM; i++) { x->x_pos[i] = 0.0; x->x_vel[i] = 0.0; }
    mass_strike(x, x->x_shape < 0 ? 0 : x->x_shape);
}

/* map the 0..1 macro onto the whole parameter set (curated morph).
 * 0   -> heavy, slow, tightly damped: dull sine that barely evolves
 * 100 -> fast, stiff, nearly undamped: bright inharmonic clang
 *
 * The integrator is explicit (semi-implicit Euler on a discrete wave
 * equation).  Its stability limit is set by the largest eigenvalue of the
 * ring Laplacian, which is 4, so the update  v += k*laplace - c*pos  is
 * stable only while roughly  4*k + c < 4  AND there is a little damping.
 * We therefore keep k comfortably below 0.25 across the WHOLE sweep so
 * 4*k stays under 1, well inside the stable region, and never let damping
 * reach zero.  Brightness/clang comes from stiffer+faster+less-damped, not
 * from pushing the solver unstable. */
static inline int mass_macro(double t, double *stiff, double *centre,
                             double *damp, double *speed, int *count)
{
    if (!(t >= 0.0 && t <= 1.0)) t = 0.0;   /* NaN/inf guard */

    *stiff  = 0.04 + 0.16 * t;              /* soft  -> stiff (k stays < 0.25) */
    *centre = 0.02 + 0.18 * t;              /* saggy -> firm restoring          */
    *damp   = 0.35 * (1.0 - t) + 0.010;     /* dead  -> lightly damped          */
    *speed  = 0.40 + 2.60 * t;              /* crawl -> fast evolution          */

    /* mass count grows with brightness: more masses -> denser, brighter ring */
    {
        int n = 16 + (int)(96.0 * t + 0.5);     /* 16 .. 112 */
        if (n < MINM) n = MINM;
        if (n > MAXM) n = MAXM;
        *count = n;
    }

    /* rest / strike shape steps up with brightness */
    if (t < 0.30) return 0;                 /* sine  */
    if (t < 0.55) return 1;                 /* two   */
    if (t < 0.80) return 2;                 /* pulse */
    return 3;                               /* rand  */
}

/* ------------------------------------------------------------------ */

static void mass_step(t_alien_mass *x, double stiff, double centre,
                      double damp, double speed)
{
    int n = x->x_n, i;
    int blew = 0;

    /* dt is folded into the coefficients; keeping the effective spring
     * gain k well under the 0.25 stability ceiling (4*k < 1) is what keeps
     * the explicit integrator from running away. */
    double k = stiff * speed;
    double c = centre * 0.02 * speed;
    double d = 1.0 - damp * 0.02 * speed;

    /* clamp coefficients into the provably-stable region regardless of what
     * the macro (or a mad automation) asked for */
    if (k < 0.0)   k = 0.0;
    if (k > 0.24)  k = 0.24;       /* 4*k <= 0.96 < 1 */
    if (c < 0.0)   c = 0.0;
    if (c > 0.20)  c = 0.20;
    if (d > 0.9999) d = 0.9999;    /* always a touch of damping */
    if (d < 0.80)   d = 0.80;      /* never over-damp into stall */

    for (i = 0; i < n; i++) {
        double l = x->x_pos[(i - 1 + n) % n];
        double r = x->x_pos[(i + 1) % n];
        double f = k * (l + r - 2.0 * x->x_pos[i]) - c * x->x_pos[i];
        double v = x->x_vel[i] * d + f;
        if (!isfinite(v)) { blew = 1; v = 0.0; }
        if (v >  BLOWUP || v < -BLOWUP) blew = 1;
        x->x_vel[i] = v;
    }
    for (i = 0; i < n; i++) {
        double p = x->x_pos[i] + x->x_vel[i];
        if (!isfinite(p)) blew = 1;
        p = safe_d(p);                 /* clamp + denormal flush */
        /* soft reflective bound: bleed energy at the wall so we can never
         * pump the chain up to the hard limit */
        if (p >=  STATEMAX) { p =  STATEMAX; x->x_vel[i] *= -0.5; }
        if (p <= -STATEMAX) { p = -STATEMAX; x->x_vel[i] *= -0.5; }
        x->x_vel[i] = safe_d(x->x_vel[i]);
        x->x_pos[i] = p;
        x->x_tab[i] = (float)p;
    }
    x->x_tab[n] = x->x_tab[0];

    if (blew) mass_reset(x);
}

/* ------------------------------------------------------------------ */

static t_int *alien_mass_perform(t_int *w)
{
    t_alien_mass *x = (t_alien_mass *)(w[1]);
    t_sample *fin = (t_sample *)(w[2]);
    t_sample *min = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    int nsamp = (int)(w[5]);

    double sr = x->x_sr, ph = x->x_ph;
    double stiff, centre, damp, speed;
    int shape, count, n;

    /* ---- ONCE PER BLOCK: derive slow-changing structure from the macro's
     * first sample.  Deciding mass-count / rest-shape / re-strike per sample
     * is what made the patch stutter, so it is hoisted here. ---- */
    {
        double mac = (nsamp > 0) ? (double)min[0] : 0.0;
        double t;
        if (!isfinite(mac)) mac = 0.0;
        t = mac - 100.0 * floor(mac * 0.01);      /* wrap into [0,100)   */
        if (!(t >= 0.0 && t <= 100.0)) t = 0.0;
        t *= 0.01;                                 /* normalise to [0,1) */
        shape = mass_macro(t, &stiff, &centre, &damp, &speed, &count);

        /* resize only on actual change */
        if (count != x->x_n) {
            int old = x->x_n;
            x->x_n = count;
            if (count > old) {
                /* fill new cells so the coupling loop reads finite data */
                int i;
                for (i = old; i < count; i++) {
                    x->x_pos[i] = 0.0;
                    x->x_vel[i] = 0.0;
                    x->x_tab[i] = 0.0f;
                }
            }
            x->x_tab[x->x_n] = x->x_tab[0];
        }

        /* re-strike only when the discrete rest shape actually changes */
        if (shape != x->x_shape) mass_strike(x, shape);
    }

    n = x->x_n;
    if (n < MINM) n = MINM;
    if (n > MAXM) n = MAXM;
    x->x_n = n;

    while (nsamp--) {
        double f0 = *fin++;
        double p, pos, frac, y;
        int i0;

        (void)*min++;   /* macro already consumed at block level */

        if (!isfinite(f0)) f0 = 0.01;
        if (f0 < 0.01)    f0 = 0.01;      /* forbid div-by-zero / DC scan */
        if (f0 > 8000.0)  f0 = 8000.0;

        if (--x->x_div <= 0) {
            x->x_div = PHYSDIV;
            mass_step(x, stiff, centre, damp, speed);
            n = x->x_n;                   /* mass_reset may have re-clamped */
        }

        ph += f0 / sr;
        ph -= floor(ph);                  /* robust wrap for any f0/sr      */

        p = ph;
        pos  = p * (double)n;
        i0   = (int)pos;
        if (i0 < 0)   i0 = 0;
        if (i0 >= n)  i0 = n - 1;         /* bounds-safe table index        */
        frac = pos - (double)i0;
        y = x->x_tab[i0] + (x->x_tab[i0 + 1] - x->x_tab[i0]) * frac;

        /* final safety: never emit NaN/Inf, then soft-clip near unity */
        if (!isfinite(y)) y = 0.0;
        y *= 0.87;
        /* smooth saturator, bounded in ~[-1.5,1.5] */
        if (y >  1.5) y =  1.5;
        else if (y < -1.5) y = -1.5;
        else y = y - (y * y * y) * (1.0 / 6.75);   /* gentle soft knee     */

        *out++ = (t_sample)y;
    }
    x->x_ph = ph;
    return (w + 6);
}

static void alien_mass_dsp(t_alien_mass *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    if (x->x_sr < 1000.0) x->x_sr = 44100.0;
    dsp_add(alien_mass_perform, 5, x,
            sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_mass_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_mass *x = (t_alien_mass *)pd_new(alien_mass_tilde_class);
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    x->x_ph = 0.0; x->x_div = PHYSDIV;
    x->x_n = 16;
    x->x_rng = 88172645u;
    memset(x->x_pos, 0, sizeof(x->x_pos));
    memset(x->x_vel, 0, sizeof(x->x_vel));
    memset(x->x_tab, 0, sizeof(x->x_tab));

    x->x_shape = -1;          /* force a strike on the first perform block */
    mass_strike(x, 0);        /* sine */

    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_mass_tilde_setup(void)
{
    alien_mass_tilde_class = class_new(gensym("alien_mass~"),
        (t_newmethod)alien_mass_new, 0, sizeof(t_alien_mass),
        CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_mass_tilde_class, t_alien_mass, x_f);
    class_addmethod(alien_mass_tilde_class, (t_method)alien_mass_dsp,
                    gensym("dsp"), A_CANT, 0);
}
