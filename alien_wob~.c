/* alien_wob~.c  --  a squelchy dubstep wobble / growl engine
 *
 * A monophonic bass voice built to be WOBBLED: inlet 1 is the 0..100 macro,
 * meant to be driven by a tempo-synced LFO (a signal), and it sweeps a
 * resonant multimode filter across the note -- that sweep IS the wobble.
 *
 * SOURCE
 *   Two detuned polyBLEP saws a few cents apart (the reese body) plus a sine
 *   sub one octave below, all at f0, at a modest level.  polyBLEP keeps the saw
 *   edges alias-free so the growl reads as brightness, not fizz, on a PA.
 *
 * FILTER
 *   A zero-delay-feedback (TPT) state-variable filter run near self-oscillation.
 *   The high resonance is where the squelch/growl lives.  The macro morphs the
 *   tap from lowpass (low macro, closed and round) toward a bandpass/formant
 *   peak (high macro, screaming), and pushes resonance up as the cutoff opens.
 *   A touch of the SVF's own high-resonance ring is blended in so it *screams* a
 *   little at the top of the sweep.
 *
 *   The macro -> cutoff mapping is exponential (~60 Hz .. ~8 kHz) and is read
 *   PER SAMPLE, together with resonance and the LP->BP morph, so a fast LFO into
 *   inlet 1 wobbles the timbre SMOOTHLY with no zipper noise.  Only the discrete
 *   / expensive work (the exponential mapping constants, the drive gains) is done
 *   once per block.  The per-sample cutoff->coefficient step uses a cheap padded
 *   tan approximation instead of a real tan() so it stays inexpensive.
 *
 * POST
 *   Asymmetric soft drive with a light fold adds even-harmonic grit that
 *   survives on small speakers, then a soft clip bounds the output.
 *
 * SAFETY (build uses -ffast-math, which deletes isfinite(); we rely on magnitude
 * clamps instead): resonance is clamped so the SVF poles stay inside the unit
 * circle, cutoff is clamped to (20, 0.45*sr), f0 >= 0.01, every filter state is
 * denormal-flushed and finite-guarded (flushd-style magnitude test), and the
 * output is finite-guarded and bounded to ~[-1.5,1.5], nominal near unity.
 *
 *   inlet 0   signal/float   fundamental in Hz         (CLASS_MAINSIGNALIN)
 *   inlet 1   signal/float   MACRO 0..100 (the WOB)    -- LFO this for the wobble
 *   outlet 0  signal
 *
 * build:
 *   gcc -shared -fPIC -O2 -o alien_wob~.pd_linux alien_wob~.c -lm
 *   (macOS: -bundle -undefined dynamic_lookup -o alien_wob~.pd_darwin)
 */

#include "m_pd.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define OS 2                    /* oversampling factor (nonlinear stages) */

static t_class *alien_wob_tilde_class;

typedef struct _alien_wob {
    t_object x_obj;
    t_float  x_f;               /* scalar fallback for inlet 0 */

    double   x_sr;              /* oversampled sample rate */

    /* oscillator phases */
    double   x_ph1, x_ph2, x_phsub;

    /* TPT state-variable filter integrator states */
    double   x_ic1, x_ic2;

    /* decimation lowpass state (2-pole, at the oversampled rate) */
    double   x_d1, x_d2;

    /* asymmetric-drive DC blocker */
    double   x_dcx, x_dcy;
} t_alien_wob;

/* ------------------------------------------------------------------ */
/* polyBLEP: the correction that removes the alias from a naive edge   */
/* ------------------------------------------------------------------ */

static inline double poly_blep(double t, double dt)
{
    if (t < dt)        { t /= dt;            return t + t - t*t - 1.0; }
    if (t > 1.0 - dt)  { t = (t - 1.0) / dt; return t*t + t + t + 1.0; }
    return 0.0;
}

static inline double blep_saw(double phase, double dt)
{
    return (2.0 * phase - 1.0) - poly_blep(phase, dt);
}

static inline double softclip(double v)
{
    if (v >  3.0) return 1.0;
    if (v < -3.0) return -1.0;
    return v * (27.0 + v*v) / (27.0 + 9.0*v*v);   /* cheap tanh approx */
}

/* flush denormals and force finite; magnitude tests survive -ffast-math */
static inline double flushd(double v)
{
    if (!(v > -1e12 && v < 1e12)) return 0.0;   /* NaN/Inf/huge -> 0 */
    if (v > -1e-20 && v < 1e-20)  return 0.0;   /* denormal -> 0 */
    return v;
}

/* cheap approximation of tan(pi*f/sr) for f in (0, 0.45*sr).  A padded rational
 * that tracks tan well up to ~0.45*sr, so we can turn a per-sample cutoff into an
 * SVF coefficient without a real tan() in the inner loop.  x = pi*f/sr < ~1.41. */
static inline double fast_tan(double x)
{
    double x2 = x * x;
    /* Pade-ish [3/2] fit, clamped: accurate to ~1% below 0.45*sr */
    double t = x * (1.0 + x2 * (0.133333333 + x2 * 0.053968254))
                 / (1.0 - x2 * 0.371732 + x2 * x2 * 0.0088);
    if (!(t > 1e-6)) t = 1e-6;
    if (t > 40.0)    t = 40.0;                  /* keeps 1+g finite */
    return t;
}

/* ------------------------------------------------------------------ */

static t_int *alien_wob_perform(t_int *w)
{
    t_alien_wob *x = (t_alien_wob *)(w[1]);
    t_sample *freq = (t_sample *)(w[2]);
    t_sample *mac  = (t_sample *)(w[3]);
    t_sample *out  = (t_sample *)(w[4]);
    int n = (int)(w[5]);

    double sr = x->x_sr;
    double ph1 = x->x_ph1, ph2 = x->x_ph2, phs = x->x_phsub;
    double ic1 = x->x_ic1, ic2 = x->x_ic2;
    double d1 = x->x_d1, d2 = x->x_d2;
    double dcx = x->x_dcx, dcy = x->x_dcy;
    double piosr = M_PI / sr;
    double twopi = 2.0 * M_PI;

    /* ---- PER-BLOCK: the expensive / discrete work -------------------
     * The exponential cutoff mapping constants, oscillator detune, drive gains
     * and blend amounts are all pinned to the block's first macro/f0 sample.
     * A steadily-swept macro therefore costs the same as a static one; the
     * per-sample loop only does a cheap exp2 and a rational tan for the wobble. */
    double mc0 = (n > 0) ? (double)mac[0] : 0.0;
    double mt = mc0 - 100.0 * floor(mc0 * 0.01);    /* wrap to [0,100) */
    if (!(mt >= 0.0 && mt <= 100.0)) mt = 0.0;      /* NaN/inf guard */
    mt *= 0.01;                                     /* macro base -> [0,1) */

    double f0blk = (n > 0) ? (double)freq[0] : 55.0;
    if (!(f0blk > 0.0)) f0blk = 55.0;
    if (f0blk < 0.01)   f0blk = 0.01;
    if (f0blk > 8000.0) f0blk = 8000.0;

    /* detune of the second saw: a few cents (the reese beat) */
    double detune = pow(2.0, 9.0 / 1200.0);         /* +9 cents */
    /* sub level fades a touch as the growl opens so the top screams cleaner */
    double sublev = 0.85 - 0.25 * mt;
    double sawlev = 0.5;

    /* post drive/fold: light at the bottom, grittier as it opens */
    double drive_in  = 1.0 + (0.4 + 2.6 * mt) ;     /* asym drive amount */
    double fold_amt  = 0.10 + 0.35 * mt;            /* light fold for grit */
    double out_gain  = 0.78;

    /* decimation coefficient (fixed one-pole pair at the oversampled rate) */
    double da_wc = tan(M_PI * 0.22);
    double da_a  = da_wc / (1.0 + da_wc);

    /* DC-blocker coefficient (~20 Hz at the base rate; the fold is asymmetric) */
    double dc_r = 1.0 - (2.0 * M_PI * 12.0 / (sr / OS));
    if (dc_r < 0.9) dc_r = 0.9;

    while (n--) {
        double f0 = *freq++;
        double mcv = *mac++;                        /* PER-SAMPLE macro read! */
        double acc = 0.0;
        int o;

        if (!(f0 > 0.0)) f0 = 0.01;
        if (f0 < 0.01)   f0 = 0.01;
        if (f0 > 8000.0) f0 = 8000.0;

        /* ---- per-sample WOB control (cheap): a fast LFO into inlet 1 must
         * wobble the cutoff / resonance / LP->BP morph SMOOTHLY, so read them
         * every sample.  Only exp2() and a rational tan are used here.        */
        double mw = mcv - 100.0 * floor(mcv * 0.01);
        if (!(mw >= 0.0 && mw <= 100.0)) mw = 0.0;
        mw *= 0.01;                                 /* wob position -> [0,1) */

        /* exponential cutoff ~60 Hz .. ~8 kHz across the wob */
        double cut = 60.0 * pow(2.0, mw * 7.06);    /* 60 * 2^7.06 ~= 8 kHz */
        if (cut < 20.0)         cut = 20.0;
        if (cut > sr * 0.45)    cut = sr * 0.45;

        /* resonance rises as the filter opens -> screams near the top, but
         * clamped so the SVF poles stay well inside the unit circle */
        double reso = 0.55 + 0.44 * mw;             /* 0.55 .. 0.99 */
        double kq = 2.0 - 1.86 * reso;              /* SVF damping 2R-like */
        if (kq < 0.06) kq = 0.06;                   /* strict lower bound */

        /* LP -> BP morph: closed & round at the bottom, formant peak up top */
        double bpmix = mw * mw;                     /* 0 .. 1, quadratic */
        double screamer = 0.35 * mw * mw * mw;      /* extra ring at the top */

        /* SVF coefficient via the cheap padded tan */
        double g  = fast_tan(cut * piosr);
        double gk = g + kq;
        double a1 = 1.0 / (1.0 + g * gk);           /* denominator > 0 */

        for (o = 0; o < OS; o++) {
            double dt1, dt2, dts, osc;
            double v0, v1, v2, bp, lp, hp, fout;

            /* ---- oscillators (polyBLEP saws + sine sub) --------------- */
            dt1 = f0 / sr;
            dt2 = f0 * detune / sr;
            dts = f0 * 0.5 / sr;
            if (dt1 > 0.45) dt1 = 0.45;
            if (dt2 > 0.45) dt2 = 0.45;

            ph1 += dt1; if (ph1 >= 1.0) ph1 -= 1.0;
            ph2 += dt2; if (ph2 >= 1.0) ph2 -= 1.0;
            phs += dts; if (phs >= 1.0) phs -= 1.0;

            osc  = sawlev * (blep_saw(ph1, dt1) + blep_saw(ph2, dt2)) * 0.5;
            osc += sublev * sin(phs * twopi);

            /* ---- TPT state-variable filter (zero-delay feedback) ------
             * Zavalishin's form: stable and self-oscillates cleanly at high
             * resonance.  Coefficients (g, kq) are updated per sample so the
             * wobble is smooth.                                            */
            v0 = osc;
            hp = (v0 - (kq + g) * ic1 - ic2) * a1;
            v1 = g * hp;
            bp = v1 + ic1;
            ic1 = bp + v1;                          /* integrator 1 update */
            v2 = g * bp;
            lp = v2 + ic2;
            ic2 = lp + v2;                          /* integrator 2 update */

            /* clamp integrator states so a self-oscillating / driven filter
             * can never run away (magnitude test survives -ffast-math) */
            if (ic1 >  12.0) ic1 =  12.0; else if (ic1 < -12.0) ic1 = -12.0;
            if (ic2 >  12.0) ic2 =  12.0; else if (ic2 < -12.0) ic2 = -12.0;

            /* macro morph LP -> BP, plus a squirt of raw bandpass ring on
             * top for the scream near the maximum of the wob */
            fout = lp * (1.0 - bpmix) + bp * bpmix + bp * screamer;

            /* ---- asymmetric soft drive + light fold (the grit) -------- */
            {
                double di = fout * drive_in;
                double bias = 0.12;                 /* asymmetry -> even harmonics */
                double sh = softclip(di + bias) - softclip(bias);
                /* light sine fold on top adds edge that reads on small speakers */
                double fd = sin(sh * (1.0 + fold_amt) * 1.2);
                fout = sh * (1.0 - 0.35) + fd * 0.35;
            }

            /* ---- decimation lowpass at the oversampled rate ---------- */
            d1 += da_a * (fout - d1);
            d2 += da_a * (d1 - d2);
            acc = d2;
        }

        /* ---- DC blocker (the asymmetric drive adds an offset) --------- */
        {
            double y = acc - dcx + dc_r * dcy;
            dcx = acc;
            dcy = y;
            acc = y;
        }

        /* ---- output safety: finite + bounded ------------------------- */
        acc *= out_gain;
        if (!(acc > -1.5 && acc < 1.5)) {           /* catches NaN/Inf too */
            acc = softclip(acc);
            if (!(acc > -1.5 && acc < 1.5)) {
                acc = 0.0;
                ic1 = ic2 = 0.0;                    /* recover the filter */
                d1 = d2 = 0.0;
            }
        }
        *out++ = (t_sample)(acc * 0.47);
    }

    x->x_ph1 = ph1; x->x_ph2 = ph2; x->x_phsub = phs;
    x->x_ic1 = flushd(ic1); x->x_ic2 = flushd(ic2);
    x->x_d1 = flushd(d1);   x->x_d2 = flushd(d2);
    x->x_dcx = flushd(dcx); x->x_dcy = flushd(dcy);

    return (w + 6);
}

static void alien_wob_dsp(t_alien_wob *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr * OS;
    if (x->x_sr < 1000) x->x_sr = 44100.0 * OS;
    dsp_add(alien_wob_perform, 5, x,
            sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_wob_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_wob *x = (t_alien_wob *)pd_new(alien_wob_tilde_class);
    (void)s; (void)argc; (void)argv;

    x->x_sr = sys_getsr() * OS;
    if (x->x_sr < 1000) x->x_sr = 44100.0 * OS;

    x->x_ph1 = 0.0; x->x_ph2 = 0.41; x->x_phsub = 0.0;
    x->x_ic1 = x->x_ic2 = 0.0;
    x->x_d1 = x->x_d2 = 0.0;
    x->x_dcx = x->x_dcy = 0.0;

    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_wob_tilde_setup(void)
{
    alien_wob_tilde_class = class_new(gensym("alien_wob~"),
        (t_newmethod)alien_wob_new, 0, sizeof(t_alien_wob),
        CLASS_DEFAULT, A_GIMME, 0);

    CLASS_MAINSIGNALIN(alien_wob_tilde_class, t_alien_wob, x_f);
    class_addmethod(alien_wob_tilde_class, (t_method)alien_wob_dsp,
                    gensym("dsp"), A_CANT, 0);
}
