/* alien_kuramoto~.c  --  synthesis by spontaneous synchronisation
 *
 * A bank of oscillators, each at a slightly different natural frequency,
 * each pulled toward the phase of all the others:
 *
 *     dtheta_i/dt = omega_i + (K/N) * sum_j sin(theta_j - theta_i)
 *
 * At low coupling they drift apart and you hear beating and noise.  Turn
 * the coupling up and, at a sharp threshold, they lock into a single
 * fused tone.  Between the two is a region where partial clusters form
 * and break -- chorus that is generated rather than simulated.
 *
 * The naive form costs N^2 per sample.  This uses the mean-field
 * identity instead: the whole coupling sum is recoverable from the
 * order parameter, one complex average over the bank, so the cost is
 * O(N).  That is what makes 64 oscillators viable at audio rate.
 *
 *     sum_j sin(theta_j - theta_i)
 *         = (sum_j sin theta_j) cos theta_i - (sum_j cos theta_j) sin theta_i
 *
 * The magnitude of that average is r, the coherence: 0 when the bank is
 * scattered, 1 when it is locked.  It comes out of the second outlet as
 * a signal, which is both a modulation source and an honest readout of
 * what the system is doing -- good for driving visuals.
 *
 *   inlet 0   signal/float   centre frequency in Hz
 *   inlet 1   signal/float   MACRO 0..100 (wrapped mod 100) -- one knob that
 *                            morphs the whole voice.  At 0 a few strongly
 *                            coupled oscillators lock into a narrow, mellow
 *                            sine tone; sweeping up loosens the coupling,
 *                            widens the spread, adds oscillators and shifts
 *                            the spread shape (linear -> golden), until at
 *                            100 a large, weakly coupled, wide, driven pulse
 *                            bank shimmers as an incoherent bright cluster.
 *                            It sweeps coupling, spread, n, dist, wave, fm
 *                            and drive together along a hand-tuned path.
 *
 *   outlet 0  signal         the bank
 *   outlet 1  signal         coherence r, 0..1
 *
 * build:
 *   gcc -shared -fPIC -O2 -o alien_kuramoto~.pd_linux alien_kuramoto~.c -lm
 */

#include "m_pd.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAXOSC 64

static t_class *alien_kuramoto_tilde_class;

typedef struct _alien_kuramoto {
    t_object x_obj;
    t_float  x_f;

    double   x_ph[MAXOSC];      /* phases, radians */
    double   x_ratio[MAXOSC];   /* natural frequency multipliers */
    double   x_sr;
    double   x_r;               /* smoothed coherence */

    int      x_n;               /* current oscillator count */
    int      x_dist;            /* current spread shape (0..3) */
    double   x_lastspread;      /* spread the ratios were built for, cents */
    unsigned x_rng;

    /* per-block cache of the continuous macro-driven parameters, read
     * once from the macro's first sample so perform stays O(N) cheap */
    double   x_bcoupling;
    double   x_bspread;
    double   x_bfm;
    double   x_bdrive;
    int      x_bwave;
} t_alien_kuramoto;

static unsigned rn(t_alien_kuramoto *x)
{
    x->x_rng ^= x->x_rng << 13; x->x_rng ^= x->x_rng >> 17;
    x->x_rng ^= x->x_rng << 5;  return x->x_rng;
}
static double frnd(t_alien_kuramoto *x)
{
    return (double)(rn(x) & 0xffffff) / (double)0x1000000;
}

static inline double softclip(double v)
{
    if (v >  3.0) return 1.0;
    if (v < -3.0) return -1.0;
    return v * (27.0 + v*v) / (27.0 + 9.0*v*v);
}

/* rebuild the natural-frequency multipliers for n oscillators, a given
 * spread in cents and one of four spread shapes (0 linear, 1 random,
 * 2 harmonic, 3 golden) */
static void kur_spread(t_alien_kuramoto *x, int n, double spread, int dist)
{
    double s = spread / 1200.0;
    int i;
    for (i = 0; i < n; i++) {
        double t = (n > 1) ? ((double)i / (double)(n - 1)) * 2.0 - 1.0 : 0.0;
        double d;
        if      (dist == 1) d = (frnd(x) * 2.0 - 1.0) * s;                        /* random   */
        else if (dist == 2) d = log((double)(i + 1)) / log(2.0) * s * 4.0;        /* harmonic */
        else if (dist == 3) d = (fmod((i + 1) * 0.6180339887, 1.0) * 2.0 - 1.0) * s; /* golden */
        else                d = t * s;                                           /* linear   */
        x->x_ratio[i] = pow(2.0, d);
    }
}

static void kur_scatter(t_alien_kuramoto *x, int n)
{
    int i;
    for (i = 0; i < n; i++) x->x_ph[i] = frnd(x) * 2.0 * M_PI;
}

/* ------------------------------------------------------------------ */

/* map the 0..1 macro onto the whole parameter set (curated morph).
 *
 * At t=0 a few strongly coupled sines in a narrow linear spread lock into
 * a mellow fused tone.  As t rises the coupling drops through and past the
 * locking threshold, the spread widens, oscillators are added, the spread
 * shape walks linear -> random -> harmonic -> golden, the waveform sharpens
 * sine -> triangle -> pulse, and coherence-fm and drive come in up top --
 * ending in a wide, incoherent, bright, driven shimmer. */
static inline void kur_macro(double t, int *n, double *coupling,
                             double *spread, int *dist, int *wave,
                             double *fm, double *drive)
{
    /* strongly locked at 0, well below threshold at 1 */
    *coupling = 5.0 - 4.6 * t;                 /* ~5 -> ~0.4                  */
    *spread   = 6.0 + 794.0 * t;               /* narrow -> ~2 semitones wide */
    *n        = (int)(4.0 + 44.0 * t);         /* few -> many oscillators     */
    *dist     = (int)(t * 3.999);              /* 0 -> 3 across the sweep      */
    *wave     = (t < 0.45) ? 0 : (t < 0.75 ? 1 : 2);
    *fm       = (t < 0.4) ? 0.0 : (t - 0.4) / 0.6;   /* brighten as it opens  */
    *drive    = (t < 0.6) ? 0.0 : (t - 0.6) / 0.4;   /* grit only up top      */
}

static t_int *alien_kuramoto_perform(t_int *w)
{
    t_alien_kuramoto *x = (t_alien_kuramoto *)(w[1]);
    t_sample *fin  = (t_sample *)(w[2]);
    t_sample *min  = (t_sample *)(w[3]);
    t_sample *out  = (t_sample *)(w[4]);
    t_sample *rout = (t_sample *)(w[5]);
    int nsamp = (int)(w[6]);

    double sr = x->x_sr;
    double twopi = 2.0 * M_PI;
    double invsr = 1.0 / sr;
    int i;

    /* ---- once per block, from the macro's FIRST sample ----
     * The macro drives discrete state (bank size, spread shape, waveform)
     * and quantities that are far too expensive to recompute per sample.
     * Reading them once per block is inaudible on these parameters and is
     * what keeps the object CPU-sane: the old code re-derived the whole
     * bank -- and re-scattered every phase -- on every single sample the
     * continuous macro changed, which both stalled Pd and stopped the
     * oscillators ever locking (constant re-scatter = permanent click). */
    {
        double mac = min[0];
        double t, coupling, spread, fm, drive;
        int n, dist, wave;

        /* wrap the macro into [0,100) then normalise to [0,1) */
        t = mac - 100.0 * floor(mac * 0.01);
        if (!(t >= 0.0 && t < 100.0)) t = 0.0;    /* NaN/inf guard */
        t *= 0.01;
        if (t < 0.0) t = 0.0;
        if (t > 0.999999) t = 0.999999;
        kur_macro(t, &n, &coupling, &spread, &dist, &wave, &fm, &drive);

        /* CLAMP the bank size to the allocated capacity -- this is the
         * hard guarantee against indexing x_ph / x_ratio out of bounds */
        if (n < 1)      n = 1;
        if (n > MAXOSC) n = MAXOSC;
        if (dist < 0)   dist = 0;
        if (dist > 3)   dist = 3;

        /* only rebuild (and re-scatter) when the layout actually moves to
         * a new bank size, spread shape or a meaningfully different width */
        if (n != x->x_n || dist != x->x_dist ||
            fabs(spread - x->x_lastspread) > 0.5) {
            x->x_n = n; x->x_dist = dist; x->x_lastspread = spread;
            kur_spread(x, n, spread, dist);
            kur_scatter(x, n);
        }

        x->x_bcoupling = coupling;
        x->x_bspread   = spread;
        x->x_bfm       = fm;
        x->x_bdrive    = drive;
        x->x_bwave     = wave;
    }

    {
        int   n      = x->x_n;
        int   wave   = x->x_bwave;
        double coupling = x->x_bcoupling;
        double spread   = x->x_bspread;
        double fm       = x->x_bfm;
        double drive    = x->x_bdrive;
        double invn  = 1.0 / (double)n;               /* n >= 1 guaranteed */
        double halfc = spread / 2400.0;
        double dwfac = 2.0 * M_PI * (pow(2.0, halfc) - pow(2.0, -halfc));
        double drv_pre  = 1.0 + drive * 8.0;
        double drv_post = 1.0 / (1.0 + drive * 2.0);

        while (nsamp--) {
            double f0 = *fin++;
            double K, sc = 0.0, ss = 0.0, sum = 0.0, r;
            (void)min++;   /* macro already consumed once per block */

            if (!(f0 > 0.01)) f0 = 0.01;      /* also catches NaN */
            if (f0 > 8000.0)  f0 = 8000.0;

            /* K scaled by the natural-frequency spread (rad/s) so that
             * coupling ~1.0 is the locking threshold at every pitch */
            {
                double dw = dwfac * f0;
                if (dw < 1.0) dw = 1.0;
                K = coupling * dw;
            }
            if (K < 0.0) K = 0.0;

            /* order parameter: one pass to get the mean field (O(N)) */
            for (i = 0; i < n; i++) { sc += cos(x->x_ph[i]); ss += sin(x->x_ph[i]); }
            sc *= invn; ss *= invn;
            r = sqrt(sc*sc + ss*ss);
            if (!isfinite(r)) r = 0.0;
            if (r > 1.0) r = 1.0;
            x->x_r += 0.001 * (r - x->x_r);
            if (!isfinite(x->x_r)) x->x_r = 0.0;

            /* second pass: advance and read out (O(N)) */
            for (i = 0; i < n; i++) {
                double th = x->x_ph[i];
                double cpl = K * (ss * cos(th) - sc * sin(th));
                double v, p, ph;

                ph = th + twopi * f0 * x->x_ratio[i] * invsr + cpl * invsr;
                /* wrap into [0,2pi); fmod handles any-size step safely */
                if (ph >= twopi || ph < 0.0) {
                    ph = fmod(ph, twopi);
                    if (ph < 0.0) ph += twopi;
                }
                if (!isfinite(ph)) ph = 0.0;
                x->x_ph[i] = ph;

                /* timbre opens as the bank locks: sync heard as brightness */
                p = th + fm * x->x_r * 3.0 * sin(th * 2.0);

                if (wave <= 0)      v = sin(p);
                else if (wave == 1) v = asin(0.99 * sin(p)) * (2.0 / M_PI);
                else                v = tanh(3.0 * sin(p));
                sum += v;
            }

            sum *= invn;
            if (drive > 0.0)
                sum = softclip(sum * drv_pre) * drv_post;

            sum *= 0.9;
            if (!isfinite(sum)) sum = 0.0;
            /* final soft limit into ~[-1.5,1.5] */
            if (sum >  1.5) sum =  1.5;
            if (sum < -1.5) sum = -1.5;

            if (!isfinite(r)) r = 0.0;

            *out++  = (t_sample)sum;
            *rout++ = (t_sample)r;
        }
    }
    return (w + 7);
}

static void alien_kuramoto_dsp(t_alien_kuramoto *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    dsp_add(alien_kuramoto_perform, 6, x,
            sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, sp[3]->s_vec,
            (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_kuramoto_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_kuramoto *x = (t_alien_kuramoto *)pd_new(alien_kuramoto_tilde_class);
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    x->x_r = 0.0; x->x_rng = 362436069u;

    /* build an initial bank so the first block has valid state; the
     * macro rebuilds it as soon as it moves off these values */
    x->x_n = 4; x->x_dist = 0; x->x_lastspread = 6.0;
    x->x_bcoupling = 5.0; x->x_bspread = 6.0;
    x->x_bfm = 0.0; x->x_bdrive = 0.0; x->x_bwave = 0;
    kur_spread(x, x->x_n, x->x_lastspread, x->x_dist);
    kur_scatter(x, x->x_n);

    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_kuramoto_tilde_setup(void)
{
    alien_kuramoto_tilde_class = class_new(gensym("alien_kuramoto~"),
        (t_newmethod)alien_kuramoto_new, 0, sizeof(t_alien_kuramoto),
        CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_kuramoto_tilde_class, t_alien_kuramoto, x_f);
    class_addmethod(alien_kuramoto_tilde_class, (t_method)alien_kuramoto_dsp,
                    gensym("dsp"), A_CANT, 0);
}
