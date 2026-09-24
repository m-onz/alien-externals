/* alien_lead~.c  --  monophonic lead: eski, hoover, screech, vowel, stab
 *
 * The idea: three kinds of phase mangling share ONE accumulator, so
 * they combine rather than sitting in separate modes.
 *
 *   sync   hard sync to a master oscillator -- the screaming lead
 *   fm     phase modulation from the same master -- bells and screech
 *   pd     phase distortion (Casio CZ) -- resonant sweeps, no filter
 *
 * All three share one master oscillator, so the interactions are where
 * the unusual sounds live.  Around them: up to 7 unison voices, ring
 * modulation, a morphing state-variable filter, a talking formant bank,
 * and drive.  4x oversampled, since sync and FM run well past Nyquist.
 *
 *   inlet 0   signal/float   frequency in Hz
 *   inlet 1   signal/float   MACRO 0..100 (wrapped mod 100) -- one knob that
 *                            morphs the whole voice along a curated path:
 *                            at 0 a mellow single-voice sine behind a low
 *                            resonant lowpass; sweeping up it detunes into
 *                            a bright supersaw, then folds in sync, FM,
 *                            phase distortion, ring mod, a vowel formant
 *                            and drive, opening the filter toward a
 *                            screaming, inharmonic, overdriven lead at 100.
 *
 *   outlet 0  signal
 *
 * build:
 *   gcc -shared -fPIC -O2 -o alien_lead~.pd_linux alien_lead~.c -lm
 */

#include "m_pd.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define OS      4
#define MAXUNI  7
#define NFORM   3

static t_class *alien_lead_tilde_class;

typedef struct _svf { double ic1, ic2; } t_svf;

typedef struct _alien_lead {
    t_object x_obj;
    t_float  x_f;

    double   x_sr;
    double   x_ph[MAXUNI];      /* slave phases   */
    double   x_mph;             /* master phase   */
    double   x_mlast;           /* modulator output, for feedback */
    double   x_glidef;          /* smoothed frequency */

    t_svf    x_main;
    t_svf    x_form[NFORM];
    double   x_dec1, x_dec2;    /* decimation filter */
} t_alien_lead;

/* ------------------------------------------------------------------ */

static inline double pblep(double t, double dt)
{
    if (t < dt)       { t /= dt;         return t + t - t*t - 1.0; }
    if (t > 1.0 - dt) { t = (t-1.0)/dt;  return t*t + t + t + 1.0; }
    return 0.0;
}

static inline double softclip(double x)
{
    if (x >  3.0) return 1.0;
    if (x < -3.0) return -1.0;
    return x * (27.0 + x*x) / (27.0 + 9.0*x*x);
}

/* Casio CZ phase distortion: squeeze the first part of the cycle into a
 * shorter span and stretch the rest.  On a sine this gives the resonant
 * sweep the CZ synths are known for, with no filter involved at all. */
static inline double pd_warp(double p, double amt)
{
    double k;
    if (amt <= 0.0) return p;
    k = 0.5 - 0.49 * amt;
    return (p < k) ? (0.5 * p / k) : (0.5 + 0.5 * (p - k) / (1.0 - k));
}

/* sine / saw / square crossfade, band-limited where it matters */
static inline double waveform(double p, double dt, double w, double pw)
{
    double sine, saw, sq, v;
    if (w <= 1.0) {
        sine = sin(p * 2.0 * M_PI);
        saw  = (2.0*p - 1.0) - pblep(p, dt);
        v = sine + (saw - sine) * w;
    } else {
        double p2 = p - pw;
        if (p2 < 0.0) p2 += 1.0;
        saw = (2.0*p - 1.0) - pblep(p, dt);
        sq  = ((p < pw) ? 1.0 : -1.0) + pblep(p, dt) - pblep(p2, dt);
        v = saw + (sq - saw) * (w - 1.0);
    }
    return v;
}

/* TPT state variable filter, one sample, returns lp/bp/hp by morph */
static inline double svf(t_svf *s, double in, double g, double k, double morph)
{
    double a1 = 1.0 / (1.0 + g*(g + k));
    double a2 = g * a1;
    double a3 = g * a2;
    double v3 = in - s->ic2;
    double v1 = a1*s->ic1 + a2*v3;
    double v2 = s->ic2 + a2*s->ic1 + a3*v3;
    double lp, bp, hp;
    s->ic1 = 2.0*v1 - s->ic1;
    s->ic2 = 2.0*v2 - s->ic2;
    lp = v2; bp = v1; hp = in - k*v1 - v2;
    if (morph <= 0.5) return lp + (bp - lp) * (morph * 2.0);
    return bp + (hp - bp) * ((morph - 0.5) * 2.0);
}

static inline double svf_bp(t_svf *s, double in, double g, double k)
{
    double a1 = 1.0 / (1.0 + g*(g + k));
    double a2 = g * a1;
    double a3 = g * a2;
    double v3 = in - s->ic2;
    double v1 = a1*s->ic1 + a2*v3;
    double v2 = s->ic2 + a2*s->ic1 + a3*v3;
    s->ic1 = 2.0*v1 - s->ic1;
    s->ic2 = 2.0*v2 - s->ic2;
    return v1;
}

/* five vowels, three formants each: F1 F2 F3 and their relative levels */
static const double VOW_F[5][NFORM] = {
    {730, 1090, 2440},   /* a */
    {530, 1840, 2480},   /* e */
    {270, 2290, 3010},   /* i */
    {570,  840, 2410},   /* o */
    {300,  870, 2240}    /* u */
};
static const double VOW_A[5][NFORM] = {
    {1.0, 0.50, 0.22}, {1.0, 0.45, 0.20}, {1.0, 0.40, 0.25},
    {1.0, 0.35, 0.10}, {1.0, 0.20, 0.08}
};

static inline void vowel_at(double v, double *f, double *a)
{
    int i0, i1, k;
    double t;
    if (v < 0.0) v = 0.0;
    if (v > 1.0) v = 1.0;
    t  = v * 4.0;
    i0 = (int)t; if (i0 > 3) i0 = 3;
    i1 = i0 + 1;
    t -= i0;
    for (k = 0; k < NFORM; k++) {
        f[k] = VOW_F[i0][k] + (VOW_F[i1][k] - VOW_F[i0][k]) * t;
        a[k] = VOW_A[i0][k] + (VOW_A[i1][k] - VOW_A[i0][k]) * t;
    }
}

/* map the 0..1 macro onto the whole parameter set (curated morph).
 * 0   : one sine voice, low resonant lowpass -- mellow.
 * ~.3 : detuned saw supersaw opening up -- bright.
 * ~.6 : sync + FM + phase distortion + inharmonic ratio bite in.
 * 1   : screaming ring-modded, vowel-tinted, overdriven lead. */
static inline void lead_macro(double t, double *wave, double *pw,
    double *ratio, double *sync, double *fm, double *fb, double *pd,
    double *ring, int *uni, double *spread, double *cutoff, double *res,
    double *morph, double *vowel, double *vmix, double *drive)
{
    /* oscillator moves sine -> saw -> square-ish */
    *wave   = 2.0 * t;                        if (*wave > 2.0) *wave = 2.0;
    *pw     = 0.5 - 0.2 * t;                  /* thinner pulse up top */

    /* inharmonic ratio only bites in the upper half */
    *ratio  = (t < 0.55) ? 1.0
            : 1.0 + 2.7 * (t - 0.55) / 0.45;  /* 1 -> ~3.7 metallic */

    /* sync / FM / phase distortion fade in as the sound gets aggressive */
    *sync   = (t < 0.5) ? 0.0 : (t - 0.5) / 0.5;
    *fm     = (t < 0.45) ? 0.0 : 3.5 * (t - 0.45) / 0.55;
    *fb     = (t < 0.7) ? 0.0 : 0.35 * (t - 0.7) / 0.3;
    *pd     = (t < 0.4) ? 0.0 : 0.7 * (t - 0.4) / 0.6;
    *ring   = (t < 0.75) ? 0.0 : 0.5 * (t - 0.75) / 0.25;

    /* unison detune blooms in the lower-middle, holds wide up top */
    {
        double u = 1.0 + 6.0 * (t < 0.5 ? t / 0.5 : 1.0);
        *uni = (int)(u + 0.5);
        if (*uni < 1) *uni = 1;
        if (*uni > MAXUNI) *uni = MAXUNI;
    }
    *spread = 40.0 * (t < 0.5 ? t / 0.5 : 1.0);

    /* filter opens and gets more resonant / brighter morph as t rises */
    *cutoff = 500.0 * pow(2.0, 5.2 * t);      /* ~500Hz -> ~18kHz */
    *res    = 0.45 + 0.2 * t;                 /* a touch resonant, more up top */
    *morph  = 0.5 * t;                        /* LP -> toward BP for screech */

    /* vowel formant tint arrives near the top for the talking scream */
    *vowel  = 0.2 + 0.6 * t;
    *vmix   = (t < 0.6) ? 0.0 : 0.6 * (t - 0.6) / 0.4;

    *drive  = 0.15 + 0.6 * t * t;             /* grit grows toward the top */
}

/* ------------------------------------------------------------------ */

/* keep filter/state variables finite and bounded */
static inline void svf_sanitize(t_svf *s)
{
    /* magnitude + self-inequality tests catch Inf and NaN even when
     * -ffast-math lets the compiler assume isfinite() is always true. */
    if (!isfinite(s->ic1) || !(s->ic1 == s->ic1) ||
        s->ic1 > 1e6 || s->ic1 < -1e6) s->ic1 = 0.0;
    if (!isfinite(s->ic2) || !(s->ic2 == s->ic2) ||
        s->ic2 > 1e6 || s->ic2 < -1e6) s->ic2 = 0.0;
}

static t_int *alien_lead_perform(t_int *w)
{
    t_alien_lead *x = (t_alien_lead *)(w[1]);
    t_sample *fin  = (t_sample *)(w[2]);
    t_sample *min  = (t_sample *)(w[3]);
    t_sample *out  = (t_sample *)(w[4]);
    int n = (int)(w[5]);

    double sr = x->x_sr;
    double nyq = sr * 0.45;

    /* ---- PER-BLOCK: read macro from first sample only ----------------
     * Coefficients, discrete selections and the detune table are heavy
     * (tan, pow, sin table setup); recomputing them per sample -- times
     * OS times up to 7 voices -- is the stutter/CPU killer.  We compute
     * them once here from the block's first macro value.               */
    double t, wave, pw, ratio, sync, fmi, fb, pdamt, ring;
    double spread, cutoff, res, morph, vowel, vmix, drive;
    int uni, v;
    double mac0 = (n > 0) ? (double)min[0] : 0.0;

    double g_main, k_main;                 /* main filter coeffs   */
    double fq[NFORM], fa[NFORM], fg[NFORM];/* formant coeffs       */
    double det[MAXUNI];                    /* per-voice detune     */
    double predrive, postgain;

    /* wrap the macro into [0,100) then normalise to [0,1) */
    t = mac0 - 100.0 * floor(mac0 * 0.01);
    if (!(t >= 0.0 && t <= 100.0)) t = 0.0;   /* NaN/inf guard */
    t *= 0.01;
    lead_macro(t, &wave, &pw, &ratio, &sync, &fmi, &fb, &pdamt, &ring,
               &uni, &spread, &cutoff, &res, &morph, &vowel, &vmix, &drive);

    /* hard safety clamps on morph outputs (guard against future edits) */
    if (uni < 1) uni = 1; if (uni > MAXUNI) uni = MAXUNI;   /* array bound */
    if (fmi   < 0.0) fmi   = 0.0; if (fmi   > 4.0) fmi   = 4.0;
    if (fb    < 0.0) fb    = 0.0; if (fb    > 0.5) fb    = 0.5;
    if (pdamt < 0.0) pdamt = 0.0; if (pdamt > 0.95) pdamt = 0.95;
    if (sync  < 0.0) sync  = 0.0; if (sync  > 1.0) sync  = 1.0;
    if (ring  < 0.0) ring  = 0.0; if (ring  > 1.0) ring  = 1.0;
    if (vmix  < 0.0) vmix  = 0.0; if (vmix  > 1.0) vmix  = 1.0;
    if (ratio < 0.1) ratio = 0.1;                 /* master dt > 0 */
    if (morph < 0.0) morph = 0.0; if (morph > 1.0) morph = 1.0;
    if (drive < 0.0) drive = 0.0; if (drive > 2.0) drive = 2.0;
    if (!isfinite(wave)) wave = 0.0;
    if (!isfinite(pw) || pw < 0.05) pw = 0.05; if (pw > 0.95) pw = 0.95;
    if (!isfinite(spread)) spread = 0.0;

    /* main filter coeffs (once per block) */
    if (!isfinite(cutoff) || cutoff < 20.0) cutoff = 20.0;
    if (cutoff > nyq) cutoff = nyq;
    g_main = tan(M_PI * cutoff / sr);
    if (res < 0.0) res = 0.0; if (res > 0.9) res = 0.9;
    k_main = 2.0 - 1.98 * res;            /* stays >= ~0.22, no self-blowup */

    /* formant coeffs (once per block, only if used) */
    if (vmix > 0.0) {
        int fi;
        vowel_at(vowel, fq, fa);
        for (fi = 0; fi < NFORM; fi++) {
            double fc = fq[fi];
            if (!isfinite(fc) || fc < 20.0) fc = 20.0;
            if (fc > nyq) fc = nyq;
            fg[fi] = tan(M_PI * fc / sr);
        }
    }

    /* per-voice detune table (once per block) -- kills the per-sample pow() */
    for (v = 0; v < uni; v++) {
        det[v] = (uni > 1)
            ? pow(2.0, ((v - (uni-1)*0.5) / (double)(uni-1))
                       * spread * 2.0 / 1200.0)
            : 1.0;
        if (!isfinite(det[v]) || det[v] <= 0.0) det[v] = 1.0;
    }

    /* drive gain staging (once per block), keeps output near unity */
    predrive = 1.0 + drive * 10.0;
    postgain = 1.0 / (1.0 + drive * 2.5);

    while (n--) {
        double f0 = *fin++;
        double acc = 0.0;
        int o;

        (void)*min++;   /* macro is block-rate; advance the pointer */

        if (!isfinite(f0)) f0 = 0.0;
        if (f0 < 0.01) f0 = 0.01;             /* forbid divide-by-zero */
        if (f0 > 12000.0) f0 = 12000.0;
        x->x_glidef = f0;                     /* glide reserved; track directly */

        for (o = 0; o < OS; o++) {
            double fpitch, mdt, mout, sum = 0.0, sig;
            double y, fsum = 0.0;
            int mwrapped = 0;
            double mfrac = 0.0;

            fpitch = x->x_glidef;

            /* ---- master oscillator (sync source and modulator) --- */
            mdt = fpitch * ratio / sr;
            if (mdt < 1e-9) mdt = 1e-9;           /* mfrac divide guard */
            if (mdt > 0.45) mdt = 0.45;
            x->x_mph += mdt;
            if (x->x_mph >= 1.0) {
                x->x_mph -= 1.0;
                mwrapped = 1;
                mfrac = x->x_mph / mdt;     /* how far past the reset */
            }
            {
                double mfb = fb * x->x_mlast * 0.5;
                if (mfb >  4.0) mfb =  4.0;
                if (mfb < -4.0) mfb = -4.0;
                mout = sin((x->x_mph + mfb) * 2.0 * M_PI);
            }
            if (!isfinite(mout)) mout = 0.0;
            x->x_mlast = mout;

            /* ---- slave voices ------------------------------------ */
            for (v = 0; v < uni; v++) {
                double dt, p, pre = 0.0, val;

                dt = fpitch * det[v] / sr;
                if (dt < 0.0) dt = 0.0;
                if (dt > 0.45) dt = 0.45;

                if (mwrapped && sync > 0.0) {
                    /* value just before the reset, for the BLEP step */
                    pre = waveform(x->x_ph[v], dt, wave, pw);
                    x->x_ph[v] = (1.0 - sync) * x->x_ph[v] + sync * (mfrac * dt);
                }

                x->x_ph[v] += dt;
                if (x->x_ph[v] >= 1.0) x->x_ph[v] -= 1.0;
                if (!isfinite(x->x_ph[v])) x->x_ph[v] = 0.0;

                /* phase modulation and phase distortion on one phase */
                p = x->x_ph[v] + fmi * mout * 0.25;
                p -= floor(p);
                p = pd_warp(p, pdamt);

                val = waveform(p, dt, wave, pw);

                /* band-limit the sync discontinuity */
                if (mwrapped && sync > 0.0) {
                    double step = val - pre;
                    val -= step * pblep(mfrac < 1.0 ? mfrac : 0.999, 1.0) * 0.5 * sync;
                }
                sum += val;
            }
            sig = sum / (double)uni;
            if (!isfinite(sig)) sig = 0.0;

            if (ring > 0.0) sig = sig * (1.0 - ring) + sig * mout * ring;

            /* ---- main filter (coeffs precomputed per block) ------ */
            y = svf(&x->x_main, sig, g_main, k_main, morph);
            svf_sanitize(&x->x_main);          /* stop runaway resonance */

            /* ---- formant bank (coeffs precomputed per block) ----- */
            if (vmix > 0.0) {
                int fi;
                for (fi = 0; fi < NFORM; fi++) {
                    fsum += svf_bp(&x->x_form[fi], sig, fg[fi], 0.25) * fa[fi];
                    svf_sanitize(&x->x_form[fi]);
                }
                y = y * (1.0 - vmix) + fsum * 1.6 * vmix;
            }
            if (!isfinite(y)) y = 0.0;

            /* ---- drive and decimation ---------------------------- */
            y = softclip(y * predrive) * postgain;
            {
                double wc = tan(M_PI * 0.11);
                double a  = wc / (1.0 + wc);
                x->x_dec1 += a * (y - x->x_dec1);
                x->x_dec2 += a * (x->x_dec1 - x->x_dec2);
            }
            acc = x->x_dec2;
        }

        /* final finite/bounded guarantee + soft limit ~[-1.5,1.5].
         * The (!(acc==acc)) test catches NaN even if -ffast-math elides
         * isfinite(); the magnitude test catches Inf/huge values. */
        if (!isfinite(acc) || !(acc == acc) || acc > 1e6 || acc < -1e6)
            acc = 0.0;
        acc = 1.5 * softclip(acc);
        if (acc >  1.5) acc =  1.5;
        if (acc < -1.5) acc = -1.5;
        *out++ = (t_sample)(acc * 0.58);
    }

    /* keep persistent filter state finite & denormal-free between blocks */
    svf_sanitize(&x->x_main);
    { int fi; for (fi = 0; fi < NFORM; fi++) svf_sanitize(&x->x_form[fi]); }
    if (!isfinite(x->x_dec1) || fabs(x->x_dec1) > 1e6) x->x_dec1 = 0.0;
    if (!isfinite(x->x_dec2) || fabs(x->x_dec2) > 1e6) x->x_dec2 = 0.0;
    if (!isfinite(x->x_mlast)) x->x_mlast = 0.0;
    if (!isfinite(x->x_mph))   x->x_mph = 0.0;
    if (fabs(x->x_main.ic1) < 1e-15) x->x_main.ic1 = 0;
    if (fabs(x->x_main.ic2) < 1e-15) x->x_main.ic2 = 0;
    return (w + 6);
}

static void alien_lead_dsp(t_alien_lead *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr * OS;
    dsp_add(alien_lead_perform, 5, x,
            sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec,
            (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_lead_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_lead *x = (t_alien_lead *)pd_new(alien_lead_tilde_class);
    int i;
    (void)s; (void)argc; (void)argv;

    x->x_sr = sys_getsr() * OS;
    if (x->x_sr < 1000) x->x_sr = 44100.0 * OS;

    for (i = 0; i < MAXUNI; i++) x->x_ph[i] = i * 0.137;
    x->x_mph = 0.0; x->x_mlast = 0.0; x->x_glidef = 110.0;
    x->x_main.ic1 = x->x_main.ic2 = 0.0;
    for (i = 0; i < NFORM; i++) x->x_form[i].ic1 = x->x_form[i].ic2 = 0.0;
    x->x_dec1 = x->x_dec2 = 0.0;

    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_lead_tilde_setup(void)
{
    alien_lead_tilde_class = class_new(gensym("alien_lead~"),
        (t_newmethod)alien_lead_new, 0, sizeof(t_alien_lead),
        CLASS_DEFAULT, A_GIMME, 0);

    CLASS_MAINSIGNALIN(alien_lead_tilde_class, t_alien_lead, x_f);
    class_addmethod(alien_lead_tilde_class, (t_method)alien_lead_dsp,
                    gensym("dsp"), A_CANT, 0);
}
