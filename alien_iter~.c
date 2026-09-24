/* alien_iter~.c  --  iterated-map oscillator (digital chaos)
 *
 * A discrete chaotic map iterated at a rate you set: the map spits out a new
 * number each step, and that sequence IS the waveform.  Where [alien_chaos~]
 * integrates smooth differential equations (Chua, Lorenz), this iterates
 * DISCRETE maps -- grittier, harder, unmistakably digital.  A bifurcation
 * parameter walks each map through its period-doubling cascade: a clean
 * pitched tone, then period-2, 4, 8..., then full chaos (pitched noise).
 * The bifurcation is audible as the timbre tearing apart.
 *
 *   inlet 0   signal/float   iteration rate in Hz -- the pitch
 *   inlet 1   signal/float   MACRO 0..100 (wrapped mod 100) -- one knob that
 *                            morphs the whole voice: a soft, linear-interpolated
 *                            periodic tone at 0, marching through the six maps
 *                            (logistic, tent, sine, henon, circle, gauss) while
 *                            driving each deeper into its chaotic cascade,
 *                            switching to raw stepped output and adding grit,
 *                            up to a hard, aliased, driven digital chaos at 100.
 *                            It sweeps map/bifurcation/mode/drive together along
 *                            a hand-tuned trajectory.
 *
 *   outlet 0  signal
 *
 * build:
 *   gcc -shared -fPIC -O2 -o alien_iter~.pd_linux alien_iter~.c -lm
 *   (macOS: -bundle -undefined dynamic_lookup -o alien_iter~.pd_darwin)
 */

#include "m_pd.h"
#include <math.h>
#include <string.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TWOPI (2.0 * M_PI)
#define NMAP  6

/* This object is compiled with -ffast-math, under which the optimiser
 * assumes operands are never NaN/Inf and happily deletes  x != x  and
 * isfinite() guards.  Inspect the raw IEEE-754 bits instead so the check
 * survives.  A double is non-finite iff its 11 exponent bits are all 1. */
static inline int dbad(double v)
{
    uint64_t u;
    memcpy(&u, &v, sizeof u);
    return ((u >> 52) & 0x7ffULL) == 0x7ffULL;   /* Inf or NaN */
}

/* flush denormals (and tiny values) to zero -- runaway denormals are a
 * classic per-sample CPU spike that shows up as audio stutter */
static inline double dflush(double v)
{
    return (fabs(v) < 1e-20) ? 0.0 : v;
}

static t_class *alien_iter_tilde_class;

typedef struct _alien_iter {
    t_object x_obj;
    t_float  x_f;

    double   x_sr;
    double   x_phase;          /* clock phase for the iteration rate */
    double   x_x, x_y;         /* map state */
    double   x_prev, x_cur;    /* last two map outputs (for interp) */
    double   x_dc0, x_dc1;     /* DC blocker */
    int      x_map, x_mode;
    int      x_lastmap;        /* last map, to re-seed on change */

    unsigned x_rng;
} t_alien_iter;

static unsigned rn(t_alien_iter *x)
{
    x->x_rng ^= x->x_rng << 13; x->x_rng ^= x->x_rng >> 17;
    x->x_rng ^= x->x_rng << 5;  return x->x_rng;
}
static double frnd(t_alien_iter *x){ return (double)(rn(x)&0xffffff)/(double)0x1000000; }

static inline double softclip(double v)
{
    if (v >  3.0) return 1.0;
    if (v < -3.0) return -1.0;
    return v * (27.0 + v*v) / (27.0 + 9.0*v*v);
}

static void iter_reset(t_alien_iter *x)
{
    x->x_x = 0.5 + 0.01 * (frnd(x) - 0.5);
    x->x_y = 0.0;
    x->x_prev = x->x_cur = 0.0;
}

/* iterate one step, return output in ~[-1,1] */
static double iter_step(t_alien_iter *x, double p)
{
    double o;
    switch (x->x_map) {
        case 1: { /* tent */
            double mu = 1.2 + p * 0.799;
            double xv = x->x_x;
            x->x_x = mu * (xv < 0.5 ? xv : 1.0 - xv);
            if (x->x_x < 0) x->x_x = 0; if (x->x_x > 1) x->x_x = 1;
            o = 2.0 * x->x_x - 1.0;
        } break;
        case 2: { /* sine map */
            double a = 0.7 + p * 0.3;
            x->x_x = a * sin(M_PI * x->x_x);
            o = 2.0 * x->x_x - 1.0;
        } break;
        case 3: { /* Henon -- diverges to +/-Inf outside its stable band */
            double a = 1.0 + p * 0.42, b = 0.3;
            double nx = 1.0 - a * x->x_x * x->x_x + x->x_y;
            x->x_y = b * x->x_x;
            x->x_x = nx;
            o = x->x_x * 0.5;
        } break;
        case 4: { /* circle map */
            double K = p * 8.0, omega = 0.5;
            x->x_x += omega - (K / TWOPI) * sin(TWOPI * x->x_x);
            x->x_x -= floor(x->x_x);
            o = sin(TWOPI * x->x_x);
        } break;
        case 5: { /* Gauss */
            double beta = -1.0 + p * 1.0;
            x->x_x = exp(-6.2 * x->x_x * x->x_x) + beta;
            o = x->x_x;
        } break;
        default: { /* logistic */
            double r = 3.2 + p * 0.799;
            x->x_x = r * x->x_x * (1.0 - x->x_x);
            if (x->x_x < 0) x->x_x = 0; if (x->x_x > 1) x->x_x = 1;
            o = 2.0 * x->x_x - 1.0;
        } break;
    }

    /* --- blow-up containment, every step ---
     * If the state (or output) went non-finite, re-seed and bail.  Henon and
     * Gauss can escape their attractor and race off to +/-Inf; a single Inf
     * poisons the recurrence forever (Inf-Inf = NaN, floor(NaN) = NaN...),
     * which under -ffast-math the naive  o != o  test never catches. */
    if (dbad(x->x_x) || dbad(x->x_y) || dbad(o)) {
        iter_reset(x);
        return 0.0;
    }
    /* Clamp the iterated state to a finite safe range so it can never grow
     * without bound before the next non-finite check, and flush denormals. */
    if (x->x_x >  1e6) x->x_x =  1e6;
    if (x->x_x < -1e6) x->x_x = -1e6;
    if (x->x_y >  1e6) x->x_y =  1e6;
    if (x->x_y < -1e6) x->x_y = -1e6;
    x->x_x = dflush(x->x_x);
    x->x_y = dflush(x->x_y);
    return o;
}

/* ------------------------------------------------------------------ */

/* map the 0..1 macro onto the whole parameter set (curated morph) */
static inline void iter_macro(double t, int *map, double *param,
                              int *mode, double *drive)
{
    /* six maps in equal sub-ranges, marching logistic..gauss */
    int m = (int)(t * NMAP);
    if (m >= NMAP) m = NMAP - 1;
    if (m < 0) m = 0;
    *map = m;

    /* bifurcation deepens across the whole sweep: periodic -> chaotic  */
    *param = t;

    /* smooth interpolated tone at the bottom, raw stepped grit up top  */
    *mode  = (t < 0.5) ? 1 : 0;

    /* drive stays clean until the last third, then bites               */
    *drive = (t < 0.66) ? 0.0 : (t - 0.66) / 0.34;
}

static t_int *alien_iter_perform(t_int *w)
{
    t_alien_iter *x = (t_alien_iter *)(w[1]);
    t_sample *fin = (t_sample *)(w[2]);
    t_sample *min = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    int nsamp = (int)(w[5]);

    double sr = x->x_sr;

    /* --- derive the map index ONCE PER BLOCK from the macro's first sample.
     * The map re-seeds its state on change, so deriving/testing it per sample
     * risked reset thrash (stutter) whenever the macro dithered across a map
     * boundary; and it is wasted per-sample work.  We take the block's first
     * macro sample, wrap it, and re-seed ONLY on a genuine integer map change. */
    {
        double mac0 = (nsamp > 0) ? (double)min[0] : 0.0;
        double t0, p0, d0;
        int map0, mode0;
        if (dbad(mac0)) mac0 = 0.0;
        t0 = mac0 - 100.0 * floor(mac0 * 0.01);
        if (t0 < 0.0 || t0 > 100.0 || dbad(t0)) t0 = 0.0;
        t0 *= 0.01;
        iter_macro(t0, &map0, &p0, &mode0, &d0);
        if (map0 != x->x_lastmap) {
            x->x_map = map0;
            x->x_lastmap = map0;
            iter_reset(x);
        }
    }

    while (nsamp--) {
        double f0 = *fin++, mac = *min++;
        double t, p, drive, y, o;
        int map, mode;

        /* wrap the macro into [0,100) then normalise to [0,1) */
        if (dbad(mac)) mac = 0.0;
        t = mac - 100.0 * floor(mac * 0.01);
        if (t < 0.0 || t > 100.0 || dbad(t)) t = 0.0;   /* NaN/inf guard */
        t *= 0.01;
        /* per-sample continuous morph (param/mode/drive) only; the map index
         * comes from the per-block value fixed above, never re-derived here */
        iter_macro(t, &map, &p, &mode, &drive);
        (void)map;

        if (dbad(f0)) f0 = 0.01;
        if (f0 < 0.01)    f0 = 0.01;   /* forbid divide-by-zero / DC */
        if (f0 > 12000.0) f0 = 12000.0;
        if (p < 0.0) p = 0.0; if (p > 1.0) p = 1.0;
        if (drive < 0.0) drive = 0.0; if (drive > 1.0) drive = 1.0;

        x->x_mode = mode;

        x->x_phase += f0 / sr;
        if (x->x_phase >= 1.0) {
            x->x_phase -= 1.0;
            if (x->x_phase >= 1.0) x->x_phase = 0.0;
            x->x_prev = x->x_cur;
            x->x_cur = iter_step(x, p);
        }
        y = x->x_mode ? (x->x_prev + x->x_phase * (x->x_cur - x->x_prev))
                      : x->x_cur;

        /* DC block (several maps are not zero-mean) */
        o = y - x->x_dc0 + 0.999 * x->x_dc1;
        x->x_dc0 = y; x->x_dc1 = o;
        x->x_dc0 = dflush(x->x_dc0);
        x->x_dc1 = dflush(x->x_dc1);

        if (drive > 0.0)
            o = softclip(o * (1.0 + drive * 6.0)) / (1.0 + drive * 1.5);
        else
            o = softclip(o);
        o *= 0.8;

        /* final safety: never emit non-finite, bound hard to ~[-1.5,1.5] */
        if (dbad(o)) o = 0.0;
        if (o >  1.5) o =  1.5;
        if (o < -1.5) o = -1.5;
        *out++ = (t_sample)(o * 0.89);
    }
    return (w + 6);
}

static void alien_iter_dsp(t_alien_iter *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    dsp_add(alien_iter_perform, 5, x,
            sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_iter_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_iter *x = (t_alien_iter *)pd_new(alien_iter_tilde_class);
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    x->x_phase = 0.0; x->x_dc0 = x->x_dc1 = 0.0;
    x->x_map = 0; x->x_lastmap = 0; x->x_mode = 1;
    x->x_rng = 12345u;
    iter_reset(x);

    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);   /* macro */
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_iter_tilde_setup(void)
{
    alien_iter_tilde_class = class_new(gensym("alien_iter~"),
        (t_newmethod)alien_iter_new, 0, sizeof(t_alien_iter),
        CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_iter_tilde_class, t_alien_iter, x_f);
    class_addmethod(alien_iter_tilde_class, (t_method)alien_iter_dsp, gensym("dsp"), A_CANT, 0);
}
