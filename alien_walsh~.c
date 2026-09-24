/* alien_walsh~.c  --  Walsh function synthesis
 *
 * Additive synthesis on a square-wave basis instead of a sinusoidal one.
 * Walsh functions take only the values +1 and -1, are mutually
 * orthogonal, and are indexed by "sequency" (sign changes per period)
 * rather than by frequency.  Summing them gives a sound that is
 * unmistakably digital -- hollow, hard-edged, and impossible to get
 * from sine-based additive synthesis at any setting.
 *
 * WAL(n, t) is built from Rademacher functions -- square waves at
 * octave-related rates -- selected by the bits of the GRAY CODE of n.
 * That indirection is what makes the set orthogonal and sequency-ordered.
 *
 *   inlet 0   signal/float   fundamental in Hz
 *   inlet 1   signal/float   MACRO 0..100 (wrapped mod 100) -- one knob that
 *                            morphs the whole voice: a few low-sequency
 *                            terms with a soft 1/(i+1) rolloff at 0 give a
 *                            hollow, mellow near-square tone; climbing to
 *                            100 piles on more terms, flattens the spectrum,
 *                            tilts hard toward high sequency and drives the
 *                            waveshaper, ending in a buzzing, edgy, aliased
 *                            digital blast. It sweeps n/coefs/skew/drive
 *                            together along a hand-tuned trajectory.
 *
 *   outlet 0  signal
 *
 * Walsh functions have unbounded bandwidth, so this runs 8x oversampled
 * with a 3-pole decimation filter.  It will still be bright.  That is
 * the point, but keep it away from tweeters until you have heard it.
 *
 * build:
 *   gcc -shared -fPIC -O2 -o alien_walsh~.pd_linux alien_walsh~.c -lm
 *   (macOS: -bundle -undefined dynamic_lookup -o alien_walsh~.pd_darwin)
 */

#include "m_pd.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define OS    8
#define MAXN  64

static t_class *alien_walsh_tilde_class;

typedef struct _alien_walsh {
    t_object x_obj;
    t_float  x_f;
    double   x_sr, x_ph;
    double   x_d1, x_d2, x_d3;

    /* per-block cached morph state (rebuilt only when the macro changes) */
    double   x_lastmac;              /* last macro value used to build cache */
    int      x_n;                    /* current term count (<= MAXN)         */
    double   x_drive;                /* cached waveshaper amount             */
    double   x_coef[MAXN];           /* precomputed, normalised coefficients */
} t_alien_walsh;

/* Rademacher function k: a square wave with 2^(k+1) sign changes */
static inline int rademacher(double t, int k)
{
    return (((int)(t * (double)(1 << (k+1)))) & 1) ? -1 : 1;
}

/* WAL(n,t): product of the Rademacher functions selected by the bits
 * of the Gray code of n.  Gray coding is what orders them by sequency. */
static inline double walsh(int n, double t)
{
    int g = n ^ (n >> 1);
    int v = 1, k = 0;
    while (g) {
        if (g & 1) v *= rademacher(t, k);
        g >>= 1; k++;
    }
    return (double)v;
}

static inline double softclip(double x)
{
    if (x >  3.0) return 1.0;
    if (x < -3.0) return -1.0;
    return x * (27.0 + x*x) / (27.0 + 9.0*x*x);
}

/* ------------------------------------------------------------------ */

/* map the 0..1 macro onto the whole parameter set (curated morph).
 * n     : how many Walsh functions to sum   (few -> many)
 * tilt  : coefficient spectrum shape 0..1   (soft rolloff -> flat/bright)
 * skew  : bias toward high sequency 0..1    (mellow -> edgy)
 * drive : waveshaper amount 0..1            (clean -> saturated only up top) */
static inline void walsh_macro(double t, int *n, double *tilt,
                               double *skew, double *drive)
{
    *n     = (int)(3.0 + 45.0 * t * t);           /* few -> many terms      */
    if (*n < 1)    *n = 1;
    if (*n > MAXN) *n = MAXN;
    *tilt  = t;                                    /* rolloff -> flat        */
    *skew  = t * t;                                /* mellow -> high seq.    */
    *drive = (t < 0.55) ? 0.0 : (t - 0.55) / 0.45; /* grit only up top       */
}

/* Rebuild the discrete morph state (term count, coefficient table, drive)
 * from one macro sample.  This is the expensive part, so it runs at most
 * once per DSP block -- only when the macro has actually moved.  The
 * coefficient table is pre-normalised so the raw Walsh sum is near unity
 * regardless of term count / tilt / skew. */
static void alien_walsh_build(t_alien_walsh *x, double mac)
{
    double t, tilt, skew, drive, norm = 0.0;
    int n, i;

    /* wrap macro into [0,100) then normalise to [0,1); NaN/inf -> 0 */
    if (!(mac > -1e12 && mac < 1e12)) mac = 0.0;
    t = mac - 100.0 * floor(mac * 0.01);
    if (!(t >= 0.0 && t < 100.0)) t = 0.0;
    t *= 0.01;

    walsh_macro(t, &n, &tilt, &skew, &drive);
    if (n < 1)    n = 1;
    if (n > MAXN) n = MAXN;                 /* clamp to array capacity */

    for (i = 0; i < n; i++) {
        double roll = 1.0 / (double)(i + 1);
        double c = roll + tilt * (1.0 - roll);
        if (skew > 0.0)
            c *= 1.0 + skew * ((double)i / (double)n) * 3.0;
        x->x_coef[i] = c;
        norm += fabs(c);
    }
    if (norm < 1e-6) norm = 1.0;
    for (i = 0; i < n; i++)
        x->x_coef[i] /= norm;               /* pre-normalised to ~unity */

    x->x_n       = n;
    x->x_drive   = drive;
    x->x_lastmac = mac;
}

static t_int *alien_walsh_perform(t_int *w)
{
    t_alien_walsh *x = (t_alien_walsh *)(w[1]);
    t_sample *in  = (t_sample *)(w[2]);
    t_sample *min = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    int nsamp = (int)(w[5]);

    double sr = x->x_sr, ph = x->x_ph;
    int i, o, n;
    double drive;

    /* decimation-filter coefficient: fixed, so compute it once per block */
    double wc = tan(M_PI * 0.055);
    double a  = wc / (1.0 + wc);

    /* build the discrete morph state ONCE per block from the first macro
     * sample, and only when it has actually changed (otherwise the up-to-48
     * term coefficient table would be rebuilt every sample -> stutter). */
    if (nsamp > 0 && min[0] != x->x_lastmac)
        alien_walsh_build(x, (double)min[0]);

    n     = x->x_n;
    drive = x->x_drive;
    if (n < 1)    n = 1;
    if (n > MAXN) n = MAXN;                    /* belt-and-braces clamp */

    while (nsamp--) {
        double f0 = *in++, acc = 0.0;
        (void)*min++;                          /* macro read once per block */

        if (!(f0 >= 0.01)) f0 = 0.01;          /* also catches NaN */
        if (f0 > 6000.0)   f0 = 6000.0;

        for (o = 0; o < OS; o++) {
            double s = 0.0, y;
            ph += f0 / sr;
            if (ph >= 1.0) ph -= 1.0;

            for (i = 0; i < n; i++)
                s += x->x_coef[i] * walsh(i, ph);

            /* coef table is pre-normalised: s ~ unity.  Drive pushes it
             * into the soft clipper for grit up top, then compensate. */
            y = softclip(s * (1.0 + drive * 6.0)) / (1.0 + drive);

            x->x_d1 += a * (y - x->x_d1);
            x->x_d2 += a * (x->x_d1 - x->x_d2);
            x->x_d3 += a * (x->x_d2 - x->x_d3);
            acc = x->x_d3;
        }

        /* final safety net: if anything went non-finite or ran away, reset
         * the filter state so a single glitch can't poison the block forever.
         * The magnitude test also catches NaN/Inf even when -ffast-math has
         * defeated isfinite() (NaN fails every comparison, so !(<=1e6) is
         * true; ordinary samples are ~unity and pass straight through). */
        if (!isfinite(acc) || !(acc >= -1e6 && acc <= 1e6)) {
            acc = 0.0;
            x->x_d1 = x->x_d2 = x->x_d3 = 0.0;
        }
        if (acc >  1.5) acc =  1.5;
        if (acc < -1.5) acc = -1.5;

        *out++ = (t_sample)(acc * 0.78);
    }
    x->x_ph = ph;
    return (w + 6);
}

static void alien_walsh_dsp(t_alien_walsh *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr * OS;
    dsp_add(alien_walsh_perform, 5, x, sp[0]->s_vec, sp[1]->s_vec,
            sp[2]->s_vec, (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_walsh_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_walsh *x = (t_alien_walsh *)pd_new(alien_walsh_tilde_class);
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr() * OS;
    if (x->x_sr < 1000) x->x_sr = 44100.0 * OS;
    x->x_ph = 0.0; x->x_d1 = x->x_d2 = x->x_d3 = 0.0;

    /* force a coefficient-table build on the first block */
    x->x_lastmac = -1e30;
    alien_walsh_build(x, 0.0);

    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_walsh_tilde_setup(void)
{
    alien_walsh_tilde_class = class_new(gensym("alien_walsh~"),
        (t_newmethod)alien_walsh_new, 0, sizeof(t_alien_walsh),
        CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_walsh_tilde_class, t_alien_walsh, x_f);
    class_addmethod(alien_walsh_tilde_class, (t_method)alien_walsh_dsp,
                    gensym("dsp"), A_CANT, 0);
}
