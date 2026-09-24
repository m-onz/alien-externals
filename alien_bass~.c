/* alien_bass~.c  --  a morphing bass voice, sub through reese/growl
 *
 * Signal chain, all of it running at 2x oversampling:
 *
 *   2 detuned polyBLEP saws  ]
 *   1 polyBLEP square         ]--> sine wavefolder --> ZDF ladder --> drive
 *   1 sine sub                ]
 *
 * Three things here that most Pd bass patches lack:
 *
 * 1. polyBLEP oscillators.  A naive saw aliases badly, and on a club
 *    PA that reads as fizz in the top end rather than as brightness.
 * 2. A zero-delay-feedback ladder filter (Zavalishin's TPT form).  The
 *    usual one-sample-delay Moog approximations detune and go unstable
 *    as resonance rises; this one self-oscillates cleanly at res 1.0
 *    and tracks cutoff accurately all the way up.
 * 3. 2x oversampling around the folder and filter, because both are
 *    nonlinear and both generate harmonics above Nyquist.
 *
 *   inlet 0   signal/float   fundamental in Hz
 *   inlet 1   signal/float   MACRO 0..100 (wrapped mod 100) -- one knob that
 *                            morphs the whole voice: a clean deep sine sub at
 *                            0, adding detuned saws + square, opening the
 *                            filter with rising resonance, then folding and
 *                            overdriving into a bright detuned reese/growl at
 *                            100. It sweeps osc mix / detune / fold / cutoff /
 *                            res / keytrack / drive together along a
 *                            hand-tuned trajectory.
 *
 *   outlet 0  signal
 *
 * build:
 *   gcc -shared -fPIC -O2 -o alien_bass~.pd_linux alien_bass~.c -lm
 */

#include "m_pd.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define OS 2                    /* oversampling factor */

static t_class *alien_bass_tilde_class;

typedef struct _alien_bass {
    t_object x_obj;
    t_float  x_f;               /* scalar fallback for inlet 0 */

    double   x_sr;              /* oversampled rate */
    double   x_ph1, x_ph2, x_ph3, x_phsub;

    /* ladder state */
    double   x_s1, x_s2, x_s3, x_s4;
    /* decimation filter state (2-pole, at the oversampled rate) */
    double   x_d1, x_d2;
} t_alien_bass;

/* ------------------------------------------------------------------ */
/* polyBLEP: the correction that removes the alias from a naive edge   */
/* ------------------------------------------------------------------ */

static inline double poly_blep(double t, double dt)
{
    if (t < dt) { t /= dt; return t + t - t*t - 1.0; }
    if (t > 1.0 - dt) { t = (t - 1.0) / dt; return t*t + t + t + 1.0; }
    return 0.0;
}

static inline double blep_saw(double phase, double dt)
{
    return (2.0 * phase - 1.0) - poly_blep(phase, dt);
}

static inline double blep_square(double phase, double dt, double pw)
{
    double v = (phase < pw) ? 1.0 : -1.0;
    double p2 = phase - pw;
    if (p2 < 0.0) p2 += 1.0;
    v += poly_blep(phase, dt);
    v -= poly_blep(p2, dt);
    return v;
}

/* ------------------------------------------------------------------ */
/* sine wavefolder: folding past the threshold generates harmonics     */
/* that track fold depth, so an envelope on fold opens the timbre up   */
/* without a filter sweep.  This is the Buchla-ish west coast move.    */
/* ------------------------------------------------------------------ */

static inline double wavefold(double x, double amt)
{
    if (amt <= 0.0) return x;
    return sin(x * (1.0 + amt * 6.0) * 1.5707963267948966);
}

static inline double softclip(double x)
{
    if (x >  3.0) return 1.0;
    if (x < -3.0) return -1.0;
    return x * (27.0 + x*x) / (27.0 + 9.0*x*x);   /* cheap tanh approx */
}

/* ------------------------------------------------------------------ */

/* map the 0..1 macro onto the whole parameter set (curated morph):
 * 0   -> a clean, filtered sine sub, no motion
 * mid -> detuned saws + square swell in, filter opens, resonance rises
 * 1   -> a bright, folded, overdriven reese/growl with high resonance    */
static inline void bass_macro(double t, double *saw, double *sq, double *sub,
                              double *detune, double *pw, double *fold,
                              double *drive, double *cutoff, double *res,
                              double *keytrack)
{
    /* oscillator mix: sub dominates low, saws + square swell in on top   */
    *sub    = 1.0 - 0.6 * t;                    /* always present, fades   */
    *saw    = (t < 0.2) ? 0.0 : (t - 0.2) / 0.8;/* saws fade in past 0.2   */
    *sq     = (t < 0.5) ? 0.0 : (t - 0.5) * 0.9;/* square joins up top     */
    *detune = 40.0 * t * t;                     /* the reese spread grows  */
    *pw     = 0.5 - 0.2 * t;                    /* pulse narrows a little  */

    /* shaping: fold and drive are the aggression, only up top            */
    *fold   = (t < 0.5) ? 0.0 : (t - 0.5) / 0.5 * 0.6;
    *drive  = 0.1 + 0.7 * t * t;

    /* filter: opens and gets more resonant as it brightens               */
    *cutoff   = 120.0 * pow(2.0, 5.5 * t);      /* ~120 Hz -> ~5.4 kHz     */
    *res      = 0.05 + 0.95 * t;                /* up to self-oscillation  */
    *keytrack = 0.1 + 0.3 * t;
}

static t_int *alien_bass_perform(t_int *w)
{
    t_alien_bass *x = (t_alien_bass *)(w[1]);
    t_sample *freq = (t_sample *)(w[2]);
    t_sample *mac  = (t_sample *)(w[3]);
    t_sample *out  = (t_sample *)(w[4]);
    int n = (int)(w[5]);

    double sr = x->x_sr;
    double ph1 = x->x_ph1, ph2 = x->x_ph2, ph3 = x->x_ph3, phs = x->x_phsub;
    double s1 = x->x_s1, s2 = x->x_s2, s3 = x->x_s3, s4 = x->x_s4;
    double d1 = x->x_d1, d2 = x->x_d2;

    /* ---- PER-BLOCK: read the macro once from the first sample -------
     * All the expensive macro-derived quantities (pow/log/tan for the
     * filter coefficients, detune ratio, mix) are computed here, once
     * per block, instead of per sample.  A steadily-swept macro therefore
     * costs the same as a static one.                                   */
    double mc0 = (n > 0) ? (double)mac[0] : 0.0;
    double t, saw, sq, sub, detune, pw, fold, drive, cutoff, res, keytrack;
    double det;

    t = mc0 - 100.0 * floor(mc0 * 0.01);
    if (!(t >= 0.0 && t <= 100.0)) t = 0.0;        /* NaN/inf guard */
    t *= 0.01;
    bass_macro(t, &saw, &sq, &sub, &detune, &pw, &fold,
               &drive, &cutoff, &res, &keytrack);
    det = pow(2.0, detune / 1200.0);

    /* filter cutoff tracks the block's first f0 (musically fine, and
     * keeps pow/log/tan out of the per-sample loop).                    */
    double f0blk = (n > 0) ? (double)freq[0] : 55.0;
    if (!(f0blk > 0.0)) f0blk = 55.0;              /* NaN/inf/<=0 guard */
    if (f0blk < 0.01) f0blk = 0.01;
    if (f0blk > 8000.0) f0blk = 8000.0;

    double cut = cutoff
        * pow(2.0, keytrack * log(f0blk / 55.0) / log(2.0));
    if (cut < 20.0) cut = 20.0;
    if (cut > sr * 0.45) cut = sr * 0.45;

    double g  = tan(M_PI * cut / sr);
    if (!(g > 0.0)) g = 1e-6;                       /* guard */
    double ig = 1.0 / (1.0 + g);
    double G  = g * ig;
    double G4 = G * G * G * G;
    double G3 = G * G * G;
    double G2 = G * G;
    double k  = res * 4.0;
    double kG4inv = 1.0 / (1.0 + k * G4);           /* denominator > 0   */

    /* decimation coefficient (fixed) */
    double da_wc = tan(M_PI * 0.22);
    double da_a  = da_wc / (1.0 + da_wc);

    /* output drive gains (per-block) */
    double drive_in  = 1.0 + drive * 8.0;
    double drive_out = 1.0 / (1.0 + drive * 2.0);

    double twopi = 2.0 * M_PI;

    while (n--) {
        double f0 = *freq++;
        mac++;                                      /* macro read per block */
        double acc = 0.0;
        int o;

        if (!(f0 > 0.0)) f0 = 0.01;                 /* NaN/inf/<=0 guard */
        if (f0 < 0.01) f0 = 0.01;
        if (f0 > 8000.0) f0 = 8000.0;

        for (o = 0; o < OS; o++) {
            double dt1, dt2, dt3, dts, osc;
            double S, u, v, y1, y2, y3, y4;

            /* ---- oscillators -------------------------------------- */
            dt1 = f0 / sr;
            dt2 = f0 * det / sr;
            dt3 = f0 / sr;
            dts = f0 * 0.5 / sr;
            if (dt1 > 0.45) dt1 = 0.45;
            if (dt2 > 0.45) dt2 = 0.45;
            if (dt3 > 0.45) dt3 = 0.45;

            ph1 += dt1; if (ph1 >= 1.0) ph1 -= 1.0;
            ph2 += dt2; if (ph2 >= 1.0) ph2 -= 1.0;
            ph3 += dt3; if (ph3 >= 1.0) ph3 -= 1.0;
            phs += dts; if (phs >= 1.0) phs -= 1.0;

            osc  = saw * 0.5 * (blep_saw(ph1, dt1) + blep_saw(ph2, dt2));
            osc += sq  * blep_square(ph3, dt3, pw);
            osc += sub * sin(phs * twopi);

            /* ---- wavefolder --------------------------------------- */
            osc = wavefold(osc, fold);

            /* ---- zero-delay-feedback ladder ------------------------
             * Four TPT one-poles with a solved feedback path, so the
             * resonance does not detune the cutoff and the filter stays
             * stable when it self-oscillates.  Coefficients are computed
             * once per block above.                                     */
            S = (G3 * s1 + G2 * s2 + G * s3 + s4) * ig;

            u = (osc - k * softclip(S)) * kG4inv;
            u = softclip(u);            /* input saturation: the growl */

            v = (u  - s1) * G; y1 = v + s1; s1 = y1 + v;
            v = (y1 - s2) * G; y2 = v + s2; s2 = y2 + v;
            v = (y2 - s3) * G; y3 = v + s3; s3 = y3 + v;
            v = (y3 - s4) * G; y4 = v + s4; s4 = y4 + v;

            /* clamp ladder state to a finite safe range each step so a
             * self-oscillating / overdriven filter can never blow up    */
            if (s1 >  8.0) s1 =  8.0; else if (s1 < -8.0) s1 = -8.0;
            if (s2 >  8.0) s2 =  8.0; else if (s2 < -8.0) s2 = -8.0;
            if (s3 >  8.0) s3 =  8.0; else if (s3 < -8.0) s3 = -8.0;
            if (s4 >  8.0) s4 =  8.0; else if (s4 < -8.0) s4 = -8.0;

            /* ---- output drive ------------------------------------- */
            y4 = softclip(y4 * drive_in) * drive_out;

            /* ---- decimation lowpass at the oversampled rate -------- */
            d1 += da_a * (y4 - d1);
            d2 += da_a * (d1 - d2);
            acc = d2;
        }

        /* ---- final output guard: finite + soft-limited ------------ */
        if (!isfinite(acc)) {
            acc = 0.0;
            /* a non-finite accumulator means the filter state is junk;
             * reset it so the voice recovers instead of staying dead    */
            s1 = s2 = s3 = s4 = 0.0;
            d1 = d2 = 0.0;
        }
        acc = softclip(acc);                        /* keep within ~[-1.5,1.5] */
        *out++ = (t_sample)(acc * 0.69);
    }

    x->x_ph1 = ph1; x->x_ph2 = ph2; x->x_ph3 = ph3; x->x_phsub = phs;
    x->x_s1 = s1; x->x_s2 = s2; x->x_s3 = s3; x->x_s4 = s4;
    x->x_d1 = d1; x->x_d2 = d2;

    /* flush denormals on every retained state variable */
    if (fabs(x->x_s1) < 1e-15) x->x_s1 = 0;
    if (fabs(x->x_s2) < 1e-15) x->x_s2 = 0;
    if (fabs(x->x_s3) < 1e-15) x->x_s3 = 0;
    if (fabs(x->x_s4) < 1e-15) x->x_s4 = 0;
    if (fabs(x->x_d1) < 1e-15) x->x_d1 = 0;
    if (fabs(x->x_d2) < 1e-15) x->x_d2 = 0;

    return (w + 6);
}

static void alien_bass_dsp(t_alien_bass *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr * OS;
    dsp_add(alien_bass_perform, 5, x,
            sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_bass_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_bass *x = (t_alien_bass *)pd_new(alien_bass_tilde_class);
    (void)s; (void)argc; (void)argv;

    x->x_sr = sys_getsr() * OS;
    if (x->x_sr < 1000) x->x_sr = 44100.0 * OS;

    x->x_ph1 = 0.0; x->x_ph2 = 0.31; x->x_ph3 = 0.67; x->x_phsub = 0.0;
    x->x_s1 = x->x_s2 = x->x_s3 = x->x_s4 = 0.0;
    x->x_d1 = x->x_d2 = 0.0;

    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_bass_tilde_setup(void)
{
    alien_bass_tilde_class = class_new(gensym("alien_bass~"),
        (t_newmethod)alien_bass_new, 0, sizeof(t_alien_bass),
        CLASS_DEFAULT, A_GIMME, 0);

    CLASS_MAINSIGNALIN(alien_bass_tilde_class, t_alien_bass, x_f);
    class_addmethod(alien_bass_tilde_class, (t_method)alien_bass_dsp,
                    gensym("dsp"), A_CANT, 0);
}
