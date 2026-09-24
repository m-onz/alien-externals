/* alien_cheby~.c  --  Chebyshev waveshaper, harmonics drawn from a pattern
 *
 * A waveshaper built on Chebyshev polynomials of the first kind.  Their
 * defining property is the whole trick:
 *
 *     T_k(cos x) = cos(k x)
 *
 * so feeding a cosine of amplitude 1 through T_k produces the k-th
 * harmonic, exactly, and nothing else.  Sum the T_k with weights and the
 * weights ARE the harmonic amplitudes of the output.  You draw a
 * spectrum and the shaper renders it.
 *
 * Here the shaper is wrapped into a self-contained VOICE: inlet 0 is a
 * frequency in Hz that drives an INTERNAL cosine oscillator, and that cosine
 * is what gets shaped.  A cosine is the ideal driver -- T_k(cos ph) = cos(k
 * ph) -- so the weights render as the output harmonic amplitudes exactly.
 * Two things make it usable: drive scales the driver (the harmonic identity
 * holds at amplitude 1, so below 1 upper harmonics fade and above 1 it
 * saturates), and a DC blocker on the output removes the offset even
 * harmonics carry.
 *
 *   inlet 0   signal/float   fundamental in Hz (drives the internal cosine)
 *   inlet 1   signal/float   MACRO 0..100 (wrapped mod 100) -- one knob that
 *                            morphs the whole voice: at 0 a soft near-clean
 *                            tone (fundamental plus a whisper of low
 *                            harmonics, gentle drive, mostly dry); at 100 a
 *                            bright saturated buzz (a full stack of harmonics
 *                            out to the ceiling, hard drive, fully wet). It
 *                            sweeps the harmonic count, spectrum tilt, drive
 *                            and dry/wet mix together along a hand-tuned
 *                            trajectory.
 *
 *   outlet 0  signal
 *
 * build:
 *   gcc -shared -fPIC -O2 -o alien_cheby~.pd_linux alien_cheby~.c -lm
 *   (macOS: -bundle -undefined dynamic_lookup -o alien_cheby~.pd_darwin)
 */

#include "m_pd.h"
#include <math.h>
#include <string.h>

#define MAXH  32
#define TWOPI 6.283185307179586

/* Post-shaper makeup gain.  Weights are L1-normalised so the raw shaper peaks
 * near 1 regardless of harmonic count; this trims the wet path to sit under
 * unity instead of riding the soft-clip ceiling.                             */
#define POSTGAIN 0.7

static t_class *alien_cheby_tilde_class;

typedef struct _alien_cheby {
    t_object x_obj;
    t_float  x_f;

    double   x_w[MAXH];         /* harmonic weights (working, normalised) */
    int      x_n;               /* live harmonic count (the "table" size) */
    int      x_nlast;           /* last quantised count -> rebuild trigger */

    double   x_sr;              /* sample rate (for the internal oscillator) */
    double   x_ph;             /* oscillator phase, radians [0, 2pi)         */
    double   x_dc0, x_dc1;      /* DC-blocker history */
} t_alien_cheby;

static inline double softclip(double v)
{
    if (v >  3.0) return 1.0;
    if (v < -3.0) return -1.0;
    return v * (27.0 + v*v) / (27.0 + 9.0*v*v);
}

/* ------------------------------------------------------------------ */

/* map the 0..1 macro onto the whole parameter set (curated morph).
 * The harmonic COUNT is quantised to an int here; the caller only
 * rebuilds the weight table when that integer changes.  drive and mix
 * are continuous and safe to update every sample.                     */
static inline void cheby_macro(double t, int *n, double *drive, double *mix)
{
    *n     = (int)(1.0 + (MAXH - 1) * t * t + 0.5);  /* 1 -> 32 harmonics */
    if (*n < 1)    *n = 1;
    if (*n > MAXH) *n = MAXH;
    *drive = 0.6 + 3.0 * t;              /* soft -> saturating           */
    *mix   = 0.5 + 0.5 * t;              /* half-dry -> fully wet         */
}

/* build the normalised weight table for n harmonics: a 1/k spectrum
 * tilted brighter as it grows.  Rebuilt only when n changes.          */
static void cheby_build(t_alien_cheby *x, int n)
{
    int i;
    double s = 0.0;
    /* tilt: at few harmonics roll off fast (mellow); as n grows let the
     * upper partials sit up (brighter).  bias in [0.5 .. 1.0].         */
    double bias = 0.5 + 0.5 * (double)(n - 1) / (double)(MAXH - 1);
    for (i = 0; i < MAXH; i++) x->x_w[i] = 0.0;
    for (i = 0; i < n; i++) x->x_w[i] = pow(1.0 / (i + 1), 2.0 - bias);
    for (i = 0; i < n; i++) s += fabs(x->x_w[i]);
    if (s > 1e-9)
        for (i = 0; i < n; i++) x->x_w[i] /= s;
    x->x_n = n;
    x->x_nlast = n;
}

/* ------------------------------------------------------------------ */

static t_int *alien_cheby_perform(t_int *w)
{
    t_alien_cheby *x = (t_alien_cheby *)(w[1]);
    t_sample *fin = (t_sample *)(w[2]);   /* inlet 0: fundamental in Hz */
    t_sample *min = (t_sample *)(w[3]);   /* inlet 1: macro 0..100      */
    t_sample *out = (t_sample *)(w[4]);
    int nsamp = (int)(w[5]);

    double sr = x->x_sr;
    double ph = x->x_ph;
    double inc;

    /* ---- ONCE PER BLOCK: derive the morph from the macro's FIRST sample.
     * The quantised harmonic count is computed here, exactly once, so the
     * (expensive) weight-table rebuild is gated on the block, never per
     * sample.  drive/mix are held constant for the block too -- smooth
     * enough at Pd block rates and completely stutter-free.               */
    double mac0 = (double)(*min);
    double f0   = (double)(*fin);
    double t, drive, mix;
    int nq, n;

    if (sr < 1000.0) sr = 44100.0;
    if (!isfinite(mac0)) mac0 = 0.0;
    /* wrap the macro into [0,100) then normalise to [0,1) */
    t = mac0 - 100.0 * floor(mac0 * 0.01);
    if (!(t >= 0.0 && t <= 100.0)) t = 0.0;       /* NaN/inf guard */
    t *= 0.01;

    /* inlet 0 is a FREQUENCY: bound it and turn it into a phase increment for
     * the internal cosine oscillator that drives the shaper.  A cosine is the
     * natural driver -- T_k(cos ph) = cos(k ph) -- so the weights become the
     * output harmonic amplitudes exactly.  (Held constant for the block.)    */
    if (!isfinite(f0)) f0 = 0.0;
    if (f0 < 0.0)      f0 = 0.0;
    if (f0 > 12000.0)  f0 = 12000.0;
    inc = TWOPI * f0 / sr;

    cheby_macro(t, &nq, &drive, &mix);
    if (nq < 1)    nq = 1;
    if (nq > MAXH) nq = MAXH;                      /* array-bounds clamp */
    /* rebuild the weight table only when the quantised count CHANGES */
    if (nq != x->x_nlast) cheby_build(x, nq);
    n = x->x_n;
    if (n < 1) n = 1;
    if (n > MAXH) n = MAXH;                        /* belt-and-braces */

    if (drive < 0.0)  drive = 0.0;
    if (drive > 8.0)  drive = 8.0;                 /* bound the drive */
    if (mix   < 0.0)  mix = 0.0;
    if (mix   > 1.0)  mix = 1.0;

    while (nsamp--) {
        double t0, t1, tk, y, o, xc, xv, m;
        int i;

        /* advance the internal oscillator; robust phase wrap */
        ph += inc;
        if (ph >= TWOPI || ph < 0.0) ph -= TWOPI * floor(ph / TWOPI);

        /* the driver is a cosine, already inside [-1,1] -- the shaper's
         * domain -- so it needs no extra clamp, and the DRY half of the mix
         * shares this same bounded copy (never a raw out-of-range input).    */
        xc = cos(ph);

        xv = xc * drive;
        if (xv >  1.0) xv =  1.0;      /* keep inside the polynomials' domain */
        if (xv < -1.0) xv = -1.0;

        /* y = sum_{k=1}^{n} w_k T_k(xv), Chebyshev recurrence
         * T_0 = 1, T_1 = xv, T_k = 2 xv T_{k-1} - T_{k-2}          */
        t0 = 1.0; t1 = xv;
        y  = x->x_w[0] * t1;           /* k = 1 */
        for (i = 1; i < n; i++) {
            tk = 2.0 * xv * t1 - t0;   /* T_{i+1} */
            t0 = t1; t1 = tk;
            y += x->x_w[i] * tk;       /* k = i+1 */
        }
        if (!isfinite(y)) y = 0.0;     /* blow-up guard */

        /* Gain compensation / normalisation.  The weights are L1-normalised
         * (sum of magnitudes = 1), so T_k(+/-1) = +/-1 makes the raw shaper
         * output reach ~1 at full input regardless of harmonic count -- more
         * harmonics no longer mean more level.  A modest makeup below unity
         * keeps the wet path comfortably under the ceiling.                  */
        y = softclip(POSTGAIN * y);

        /* dry/wet mix, both halves on the BOUNDED input */
        m = mix * y + (1.0 - mix) * xc;

        /* DC blocker on the FINAL mix: removes both the even-harmonic offset
         * of the shaper AND any DC the dry passthrough carries.              */
        o = m - x->x_dc0 + 0.999 * x->x_dc1;
        x->x_dc0 = m; x->x_dc1 = o;
        if (!isfinite(o)) { o = 0.0; x->x_dc0 = 0.0; x->x_dc1 = 0.0; }

        /* final bounded soft clip: nominal near unity, never runaway */
        if (o >  1.5) o =  1.5;
        if (o < -1.5) o = -1.5;
        if (!isfinite(o)) o = 0.0;
        *out++ = (t_sample)(o * 0.55);
    }
    if (!isfinite(ph)) ph = 0.0;   /* keep the oscillator phase sane */
    x->x_ph = ph;
    return (w + 6);
}

static void alien_cheby_dsp(t_alien_cheby *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    if (x->x_sr < 1000.0) x->x_sr = 44100.0;
    dsp_add(alien_cheby_perform, 5, x,
            sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_cheby_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_cheby *x = (t_alien_cheby *)pd_new(alien_cheby_tilde_class);
    (void)s; (void)argc; (void)argv;
    x->x_dc0 = x->x_dc1 = 0.0;
    x->x_sr = 44100.0;
    x->x_ph = 0.0;
    x->x_nlast = -1;
    cheby_build(x, 1);                  /* valid table for the first block */

    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_cheby_tilde_setup(void)
{
    alien_cheby_tilde_class = class_new(gensym("alien_cheby~"),
        (t_newmethod)alien_cheby_new, 0, sizeof(t_alien_cheby),
        CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_cheby_tilde_class, t_alien_cheby, x_f);
    class_addmethod(alien_cheby_tilde_class, (t_method)alien_cheby_dsp,
                    gensym("dsp"), A_CANT, 0);
}
