/* alien_dsf~.c  --  discrete summation formula oscillator
 *
 * A band-limited stack of partials with a single brightness control,
 * computed in closed form rather than one sine at a time.  The identity
 * behind it (Moorer, 1976) sums a geometric series of cosines exactly:
 *
 *     sum_{k=0}^{N} a^k sin(theta + k*beta)
 *
 * collapses to a handful of terms.  So one evaluation gives you N
 * partials for the price of about four sines, and the rolloff a is a
 * continuous "how many harmonics" knob -- a = 0 is a pure sine, a near 1
 * is a bright buzz, and nothing in between aliases because N is chosen
 * each sample to keep the top partial under Nyquist.
 *
 * The partials sit at f0 + k*(ratio*f0).  So [ratio( is a formant / comb
 * control: ratio 1 is the harmonic series (a saw-ish buzz), ratio 2 is
 * odd harmonics (a square/clarinet), and non-integer ratios give moving
 * inharmonic and formant tones a normal oscillator cannot.
 *
 *   inlet 0   signal/float   fundamental in Hz
 *   inlet 1   signal/float   MACRO 0..100 (wrapped mod 100) -- one knob that
 *                            morphs the whole voice: dim harmonic sine at 0,
 *                            through a bright buzz, into an inharmonic driven
 *                            spray at 100. It sweeps bright/ratio/n/drive
 *                            together along a hand-tuned trajectory.
 *
 *   outlet 0  signal
 *
 * build:
 *   gcc -shared -fPIC -O2 -o alien_dsf~.pd_linux alien_dsf~.c -lm
 *   (macOS: -bundle -undefined dynamic_lookup -o alien_dsf~.pd_darwin)
 */

#include "m_pd.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TWOPI (2.0 * M_PI)

static t_class *alien_dsf_tilde_class;

typedef struct _alien_dsf {
    t_object x_obj;
    t_float  x_f;

    double   x_sr;
    double   x_tc;      /* carrier phase (fundamental), radians */
    double   x_tm;      /* modulator phase (spacing),   radians */
} t_alien_dsf;

static inline double softclip(double v)
{
    if (v >  3.0) return 1.0;
    if (v < -3.0) return -1.0;
    return v * (27.0 + v*v) / (27.0 + 9.0*v*v);
}

/* ------------------------------------------------------------------ */

/* map the 0..1 macro onto the whole parameter set (curated morph) */
static inline void dsf_macro(double t, double *a, double *ratio,
                             int *nmax, double *drive)
{
    *a     = 0.02 + 0.96 * t;                 /* dim sine -> bright buzz     */
    if (*a > 0.999) *a = 0.999;               /* keep denominators alive      */
    if (*a < 0.0)   *a = 0.0;
    *ratio = 1.0 + 3.0 * t * t;               /* harmonic -> inharmonic      */
    *nmax  = (int)(8.0 + 120.0 * t);          /* few -> many partials        */
    if (*nmax < 1)   *nmax = 1;               /* hard clamp the partial count */
    if (*nmax > 256) *nmax = 256;
    *drive = (t < 0.7) ? 0.0 : (t - 0.7) / 0.3;   /* grit only up top        */
}

static t_int *alien_dsf_perform(t_int *w)
{
    t_alien_dsf *x = (t_alien_dsf *)(w[1]);
    t_sample *fin = (t_sample *)(w[2]);
    t_sample *min = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    int nsamp = (int)(w[5]);

    double sr = x->x_sr, nyq = sr * 0.5;

    while (nsamp--) {
        double f0 = *fin++, mac = *min++;
        double t, a, ratio, drive, fm, theta, beta, D, y;
        int N, nmax;

        /* wrap the macro into [0,100) then normalise to [0,1) */
        t = mac - 100.0 * floor(mac * 0.01);
        if (!(t >= 0.0 && t <= 100.0)) t = 0.0;   /* NaN/inf guard */
        t *= 0.01;
        dsf_macro(t, &a, &ratio, &nmax, &drive);

        if (f0 < 0.01)   f0 = 0.01;
        if (f0 > 8000.0) f0 = 8000.0;

        fm = ratio * f0;
        if (fm < 0.01) fm = 0.01;

        /* highest partial is f0 + N*fm; keep it below Nyquist */
        N = (int)((nyq - f0) / fm);
        if (N < 1) N = 1;
        if (N > nmax) N = nmax;

        x->x_tc += TWOPI * f0 / sr;
        x->x_tm += TWOPI * fm / sr;
        /* robust wrap (fmod tolerates any sr / big increment)            */
        if (x->x_tc >= TWOPI || x->x_tc < 0.0) x->x_tc -= TWOPI * floor(x->x_tc / TWOPI);
        if (x->x_tm >= TWOPI || x->x_tm < 0.0) x->x_tm -= TWOPI * floor(x->x_tm / TWOPI);
        theta = x->x_tc; beta = x->x_tm;

        /* closed-form band-limited sum:
         *   S = [ sin(theta) - a sin(theta-beta)
         *         - a^{N+1}( sin(theta+(N+1)beta) - a sin(theta+N beta) ) ] / D
         *   D = 1 - 2a cos(beta) + a^2                                        */
        D = 1.0 - 2.0 * a * cos(beta) + a * a;
        if (D < 1e-6) D = 1e-6;
        {
            double aN1 = pow(a, N + 1);
            double num = sin(theta) - a * sin(theta - beta)
                       - aN1 * (sin(theta + (N + 1) * beta)
                                - a * sin(theta + N * beta));
            y = num / D;
            /* normalise: the series peaks near (1 - a^{N+1})/(1 - a) */
            {
                double g = (1.0 - a) / (1.0 - aN1 + 1e-12);
                y *= g;
            }
        }

        if (drive > 0.0)
            y = softclip(y * (1.0 + drive * 6.0)) / (1.0 + drive * 2.0);

        y *= 0.5;                       /* nominal headroom               */
        y = softclip(y);                /* always-on band-limit the peak  */
        if (!isfinite(y)) y = 0.0;      /* crash net: never emit NaN/Inf  */
        if (y >  1.5) y =  1.5;         /* hard bound as a last resort    */
        if (y < -1.5) y = -1.5;
        *out++ = (t_sample)(y * 1.15);
    }
    return (w + 6);
}

static void alien_dsf_dsp(t_alien_dsf *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    if (x->x_sr < 1000.0) x->x_sr = 44100.0;   /* guard nyq/increment    */
    dsp_add(alien_dsf_perform, 5, x,
            sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_dsf_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_dsf *x = (t_alien_dsf *)pd_new(alien_dsf_tilde_class);
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    x->x_tc = 0.0; x->x_tm = 0.0;

    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_dsf_tilde_setup(void)
{
    alien_dsf_tilde_class = class_new(gensym("alien_dsf~"),
        (t_newmethod)alien_dsf_new, 0, sizeof(t_alien_dsf),
        CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_dsf_tilde_class, t_alien_dsf, x_f);
    class_addmethod(alien_dsf_tilde_class, (t_method)alien_dsf_dsp,
                    gensym("dsp"), A_CANT, 0);
}
