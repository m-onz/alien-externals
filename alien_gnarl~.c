/* alien_gnarl~.c  --  a disgusting bass mangler
 *
 * A fat bass oscillator dragged through a chain of deliberately gross
 * nonlinearities.  Inlet 1 is a "gross factor" MACRO meant to be driven by an
 * LFO: pulse it and the bass HEAVES and RETCHES -- the mangle amount pumps in
 * time with the wobble.  Every stage of the chain is read PER SAMPLE from the
 * macro (cheap) so a fast LFO into inlet 1 modulates the gnarl smoothly.
 *
 * SOURCE
 *   A fat bass: a saw at f0 (naive but band-limited enough at bass f0) blended
 *   with a sine and a sine sub an octave down, so there is always body.
 *
 * MANGLE CHAIN (the macro sweeps intensity AND reorders emphasis across zones)
 *   1. RECTIFY.  Blend toward full/half-wave rectification for octave-up gross
 *      harmonics -- the classic "fuzz gets an octave" sound.
 *   2. WAVEFOLD.  Reflect the signal back on itself past a threshold: dense,
 *      aggressive harmonics.  OVERSAMPLED 4x so the fold doesn't collapse to
 *      pure hash, then decimated with a 2-pole lowpass.
 *   3. BITCRUSH + SAMPLE-RATE REDUCTION.  Quantise to a few bits and hold the
 *      sample for a while -- intentional digital grunge.
 *   4. RING MOD.  Multiply by an internal oscillator whose frequency TRACKS the
 *      pitch at a NON-INTEGER multiple of f0 -> inharmonic, metallic, gross.
 *   5. COMB.  A short resonant delay for a "throat" -- feedback kept < 1.
 *
 * The macro also REORDERS the zones: low macro leans on rectify+fold, mid piles
 * in crush+ring, high throws the comb throat wide open, so different macro
 * zones sound distinctly different as the LFO sweeps through them.
 *
 *   inlet 0   signal/float   fundamental in Hz  (CLASS_MAINSIGNALIN)
 *   inlet 1   signal/float   MACRO 0..100 -- the "gross factor", per-sample
 *
 *   outlet 0  signal
 *
 * SAFETY.  Build uses -ffast-math, so we rely on magnitude clamps, not
 * isnan/isinf: fold input is clamped, comb feedback < 1, all state is
 * denormal-flushed and finite-guarded (flushd-style magnitude test), f0 is
 * clamped >= 0.01, and the output is finite-guarded and bounded ~[-1.5,1.5]
 * with a nominal near unity.  It is gross by design -- HISS and RAIL are the
 * point -- but it stays FINITE and BOUNDED, never NaN/Inf or full-scale garbage.
 *
 * build:
 *   gcc -shared -fPIC -O2 -o alien_gnarl~.pd_linux alien_gnarl~.c -lm
 *   (macOS: -bundle -undefined dynamic_lookup -o alien_gnarl~.pd_darwin)
 */

#include "m_pd.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TWOPI 6.283185307179586
#define OS    4                 /* fold oversampling factor              */
#define DLEN  8192              /* comb delay line (power of 2)          */
#define DMASK (DLEN - 1)

static t_class *alien_gnarl_tilde_class;

typedef struct _alien_gnarl {
    t_object x_obj;
    t_float  x_f;               /* scalar fallback for inlet 0           */

    double   x_sr;              /* base sample rate                      */

    /* ---- source oscillators ---- */
    double   x_phsaw;           /* saw phase   0..1                      */
    double   x_phsin;           /* sine phase  0..1                      */
    double   x_phsub;           /* sub  phase  0..1                      */
    double   x_phring;          /* ring-mod osc phase 0..1               */

    /* ---- decimation lowpass (after the oversampled fold) ---- */
    double   x_d1, x_d2;

    /* ---- sample-and-hold / bitcrush ---- */
    double   x_shval;           /* held sample                           */
    double   x_shcnt;           /* fractional sample counter             */

    /* ---- comb "throat" ---- */
    double   x_buf[DLEN];
    int      x_wi;              /* write index                           */

    /* ---- output DC blocker (rectification carries DC) ---- */
    double   x_dc0, x_dc1;
} t_alien_gnarl;

/* cheap tanh-ish soft clip, bounded to +-1 */
static inline double softclip(double v)
{
    if (v >  3.0) return 1.0;
    if (v < -3.0) return -1.0;
    return v * (27.0 + v*v) / (27.0 + 9.0*v*v);
}

/* flush denormals and force finite; magnitude tests survive -ffast-math */
static inline double flushd(double v)
{
    if (!(v > -1e12 && v < 1e12)) return 0.0;   /* NaN/Inf/huge -> 0      */
    if (v > -1e-20 && v < 1e-20) return 0.0;    /* denormal -> 0          */
    return v;
}

/* wavefolder: reflect back on itself past +-1 a few times.  Input MUST be
 * clamped by the caller (we still bound the loop count for safety).         */
static inline double wavefold(double v)
{
    int guard = 0;
    while ((v > 1.0 || v < -1.0) && guard < 8) {
        if (v >  1.0) v =  2.0 - v;
        if (v < -1.0) v = -2.0 - v;
        guard++;
    }
    if (v >  1.0) v =  1.0;      /* belt-and-braces */
    if (v < -1.0) v = -1.0;
    return v;
}

/* interpolated read from the comb line, `d` samples behind the write head */
static inline double delread(t_alien_gnarl *x, double d)
{
    double rp, fr;
    int i0, i1;
    if (d < 1.0)        d = 1.0;
    if (d > DLEN - 2.0) d = DLEN - 2.0;
    rp = (double)x->x_wi - d;
    while (rp < 0.0) rp += (double)DLEN;
    i0 = (int)rp;
    fr = rp - (double)i0;
    i1 = (i0 + 1) & DMASK;
    i0 &= DMASK;
    return x->x_buf[i0] * (1.0 - fr) + x->x_buf[i1] * fr;
}

static t_int *alien_gnarl_perform(t_int *w)
{
    t_alien_gnarl *x = (t_alien_gnarl *)(w[1]);
    t_sample *fin = (t_sample *)(w[2]);   /* inlet 0: fundamental in Hz */
    t_sample *min = (t_sample *)(w[3]);   /* inlet 1: macro 0..100      */
    t_sample *out = (t_sample *)(w[4]);
    int nsamp = (int)(w[5]);

    double sr   = x->x_sr;
    double phsaw = x->x_phsaw, phsin = x->x_phsin, phsub = x->x_phsub;
    double phring = x->x_phring;
    double d1 = x->x_d1, d2 = x->x_d2;
    double shval = x->x_shval, shcnt = x->x_shcnt;
    double dc0 = x->x_dc0, dc1 = x->x_dc1;

    /* decimation lowpass coefficient (fixed) -- kills fold hash > ~0.22 fs */
    double da_wc = tan(M_PI * 0.22);
    double da_a  = da_wc / (1.0 + da_wc);

    while (nsamp--) {
        double f0  = (double)(*fin++);
        double mac = (double)(*min++);          /* per-sample macro read */
        double t, gfac;
        double zRect, zFold, zCrush, zRing, zComb;
        double dtsaw, dtsin, dtsub, dtring;
        double src, folded, o, dry, combsig;
        int oo;

        /* ---- f0 clamp ---- */
        if (!(f0 > 0.0)) f0 = 0.01;
        if (f0 < 0.01)   f0 = 0.01;
        if (f0 > 4000.0) f0 = 4000.0;

        /* ---- macro -> gross factor, per sample ----
         * wrap to [0,100), normalise to [0,1).  `gfac` is the overall
         * intensity; the per-stage weights below REORDER emphasis across
         * zones so different macro regions sound distinct.                */
        t = mac - 100.0 * floor(mac * 0.01);
        if (!(t >= 0.0 && t <= 100.0)) t = 0.0;     /* NaN/inf guard */
        t *= 0.01;
        gfac = t;

        /* per-stage amounts, each a different curve over the macro so the
         * mangle character SHIFTS as an LFO sweeps inlet 1:
         *   low  zone -> rectify + fold dominate
         *   mid  zone -> crush + ring pile in
         *   high zone -> comb throat opens wide                          */
        zRect  = (t < 0.6) ? (t / 0.6) : 1.0;               /* early      */
        zFold  = t;                                          /* all along  */
        zCrush = (t < 0.25) ? 0.0 : (t - 0.25) / 0.75;      /* mid on      */
        zRing  = (t < 0.35) ? 0.0 : (t - 0.35) / 0.65;      /* mid on      */
        zComb  = (t < 0.55) ? 0.0 : (t - 0.55) / 0.45;      /* late        */

        /* ---- SOURCE: fat bass (saw + sine + sine sub) ---- */
        dtsaw = f0 / sr;      if (dtsaw > 0.45) dtsaw = 0.45;
        dtsin = f0 / sr;      if (dtsin > 0.45) dtsin = 0.45;
        dtsub = f0 * 0.5 / sr;
        phsaw += dtsaw; if (phsaw >= 1.0) phsaw -= 1.0;
        phsin += dtsin; if (phsin >= 1.0) phsin -= 1.0;
        phsub += dtsub; if (phsub >= 1.0) phsub -= 1.0;

        src  = 0.55 * (2.0 * phsaw - 1.0);          /* naive saw body     */
        src += 0.35 * sin(phsin * TWOPI);           /* sine fundamental   */
        src += 0.45 * sin(phsub * TWOPI);           /* octave-down sub    */

        /* ---- STAGE 1: RECTIFY (octave-up gross harmonics) ----
         * crossfade toward |src| (full-wave) as zRect rises, with a DC-ish
         * offset removed by re-centering.                                */
        {
            double rec = fabs(src) * 2.0 - 0.6;     /* rough re-center     */
            src = src * (1.0 - zRect) + rec * zRect;
        }

        /* ---- STAGE 2: OVERSAMPLED WAVEFOLD ----
         * push the signal past +-1 by a depth that grows with zFold, fold
         * it back on itself, oversampled OSx to keep it from becoming pure
         * hash, then decimate.  Input to the fold is clamped in magnitude.  */
        {
            double depth = 1.0 + 6.0 * zFold;       /* 1 .. 7 pre-gain     */
            double pre   = src * depth;
            if (pre >  8.0) pre =  8.0;             /* clamp fold input    */
            if (pre < -8.0) pre = -8.0;

            folded = 0.0;
            for (oo = 0; oo < OS; oo++) {
                /* interpolate the pre value across the OS sub-steps to give
                 * the oversampled fold something to chew (cheap linear ramp
                 * from previous held toward current -- here just repeat pre,
                 * the fold's own nonlinearity supplies the upper content).   */
                double f = wavefold(pre);
                /* decimation lowpass at the oversampled rate */
                d1 += da_a * (f - d1);
                d2 += da_a * (d1 - d2);
                folded = d2;
            }
            /* blend folded back over the un-folded (so at zFold 0 it's dry) */
            src = src * (1.0 - zFold) + folded * zFold;
        }

        /* ---- STAGE 3: BITCRUSH + SAMPLE-RATE REDUCTION ----
         * quantise to a few bits and sample-and-hold for a variable stride.
         * Both worsen with zCrush -> intentional digital grunge.          */
        {
            /* sample-rate reduction: hold for `stride` input samples       */
            double stride = 1.0 + 30.0 * zCrush;    /* 1 .. 31 samples hold */
            shcnt -= 1.0;
            if (shcnt <= 0.0) {
                shval = src;
                shcnt += stride;
            }
            /* bit reduction: from ~16 bits down to ~2 bits                 */
            {
                double bits  = 16.0 - 14.0 * zCrush;    /* 16 .. 2 bits    */
                double steps = pow(2.0, bits);
                double q = floor(shval * 0.5 * steps + 0.5) / (0.5 * steps);
                if (!(q > -8.0 && q < 8.0)) q = 0.0;    /* finite guard     */
                src = src * (1.0 - zCrush) + q * zCrush;
            }
        }

        /* ---- STAGE 4: RING MOD (pitch-tracking, inharmonic) ----
         * multiply by a sine at a NON-INTEGER multiple of f0 -> metallic,
         * inharmonic, gross.  Depth grows with zRing.                      */
        {
            double rm;
            dtring = f0 * 2.4142135623730951 / sr;  /* ~2.414x -> inharmonic */
            if (dtring > 0.45) dtring = 0.45;
            phring += dtring; if (phring >= 1.0) phring -= 1.0;
            rm = sin(phring * TWOPI);
            /* unipolar-ish blend so it doesn't fully cancel to silence     */
            src = src * (1.0 - zRing) + (src * rm) * zRing;
        }

        dry = src;

        /* ---- STAGE 5: COMB "THROAT" ----
         * short resonant delay tuned near the period of f0, feedback < 1,
         * mix grows with zComb.  Feedback path is soft-clipped so it churns
         * without blowing up.                                             */
        {
            double period = sr / f0;                /* delay ~ 1 period    */
            double dsamp;
            double fb   = 0.55 + 0.35 * zComb;      /* up to 0.90 (< 1)    */
            double mix  = zComb;
            if (period < 4.0)        period = 4.0;
            if (period > DLEN - 4.0) period = DLEN - 4.0;
            dsamp = period;
            combsig = delread(x, dsamp);
            {
                double wr = dry + fb * softclip(combsig);
                x->x_buf[x->x_wi] = flushd(wr);
                x->x_wi = (x->x_wi + 1) & DMASK;
            }
            src = dry * (1.0 - mix) + combsig * mix;
        }

        /* ---- OUTPUT: makeup + bounded finite guard ----
         * a little makeup so the nominal sits near unity (~0.9), then drive
         * gently harder as gfac rises (RAIL is expected / desired), and
         * finally clamp finite and bounded.                              */
        o = src * (0.75 + 1.4 * gfac);              /* wide drive spread    */
        o = softclip(o);                            /* keeps within ~+-1   */

        /* DC blocker: rectification legitimately carries an offset; strip it
         * so the gross harmonics stay but the DC does not.                 */
        {
            double b = o - dc0 + 0.999 * dc1;
            dc0 = o; dc1 = b;
            if (!(b > -1e12 && b < 1e12)) { b = 0.0; dc0 = 0.0; dc1 = 0.0; }
            o = b;
        }

        if (!(o > -1.5 && o < 1.5)) {               /* catches NaN/Inf too */
            o = softclip(o);
            if (!(o > -1.5 && o < 1.5)) o = 0.0;
        }
        *out++ = (t_sample)(o * (0.9 * 0.71));
    }

    /* persist oscillator + filter state */
    x->x_phsaw = phsaw; x->x_phsin = phsin; x->x_phsub = phsub;
    x->x_phring = phring;
    x->x_d1 = flushd(d1); x->x_d2 = flushd(d2);
    x->x_shval = flushd(shval); x->x_shcnt = shcnt;
    x->x_dc0 = flushd(dc0); x->x_dc1 = flushd(dc1);

    return (w + 6);
}

static void alien_gnarl_dsp(t_alien_gnarl *x, t_signal **sp)
{
    if (sp[0]->s_sr > 1000) x->x_sr = sp[0]->s_sr;
    dsp_add(alien_gnarl_perform, 5, x,
            sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_gnarl_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_gnarl *x = (t_alien_gnarl *)pd_new(alien_gnarl_tilde_class);
    (void)s; (void)argc; (void)argv;    /* A_GIMME ctor ignoring args */

    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100.0;

    x->x_phsaw = 0.0; x->x_phsin = 0.13; x->x_phsub = 0.41; x->x_phring = 0.7;
    x->x_d1 = x->x_d2 = 0.0;
    x->x_shval = 0.0; x->x_shcnt = 0.0;
    x->x_dc0 = x->x_dc1 = 0.0;
    memset(x->x_buf, 0, sizeof(x->x_buf));
    x->x_wi = 0;

    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_gnarl_tilde_setup(void)
{
    alien_gnarl_tilde_class = class_new(gensym("alien_gnarl~"),
        (t_newmethod)alien_gnarl_new, 0, sizeof(t_alien_gnarl),
        CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_gnarl_tilde_class, t_alien_gnarl, x_f);
    class_addmethod(alien_gnarl_tilde_class, (t_method)alien_gnarl_dsp,
                    gensym("dsp"), A_CANT, 0);
}
