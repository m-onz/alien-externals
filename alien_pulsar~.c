/* alien_pulsar~.c  --  pulsar synthesis (after Curtis Roads)
 *
 * A "pulsaret" -- a short windowed waveform -- is emitted once per PULSAR
 * PERIOD; the rest of the period is SILENCE.  The pulsaret carries its own
 * internal FORMANT frequency, so two clocks run at once: the emission rate
 * (perceived pitch) and the formant (perceived timbre / vowel).  Sweeping the
 * duty cycle walks a single continuum -- a train of clicks -> resonant formant
 * tones -> granular textures -> smooth tones -- the whole point of the method.
 *
 * A raised-cosine (Hann) window across the pulsaret keeps every emission
 * click-free, and stochastic MASKING randomly drops whole pulsars, turning a
 * steady pulse into a broken, breathing rhythm.
 *
 *   inlet 0   signal/float   fundamental (pulsar emission rate) in Hz = pitch
 *   inlet 1   signal/float   MACRO 0..100 (wrapped mod 100) -- one knob that
 *                            morphs the whole voice: at 0 a mellow, wide,
 *                            fully-open sine pulsar (nearly a smooth tone, low
 *                            formant, no grit); climbing through resonant vowel
 *                            formants and narrowing granular pulsarets into
 *                            broken masked bursts; at 100 a bright, high-formant
 *                            odd-harmonic click spray, tightly windowed, heavily
 *                            masked and driven.  It sweeps wave/formant/duty/
 *                            mask/drive together along a hand-tuned trajectory.
 *
 *   outlet 0  signal
 *
 * build:
 *   gcc -shared -fPIC -O2 -o alien_pulsar~.pd_linux alien_pulsar~.c -lm
 *   (macOS: -bundle -undefined dynamic_lookup -o alien_pulsar~.pd_darwin)
 */

#include "m_pd.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TWOPI (2.0 * M_PI)
#define NWAVE 5

static t_class *alien_pulsar_tilde_class;

typedef struct _alien_pulsar {
    t_object x_obj;
    t_float  x_f;

    double   x_sr;
    double   x_phase;      /* pulsar period phase 0..1 (emission clock) */
    double   x_fphase;     /* pulsaret formant phase 0..1 */
    double   x_freq;       /* fundamental */
    int      x_active;     /* is this pulsar audible (not masked)? */

    /* discrete/block-rate state: derived from the macro's first sample,
     * recomputed only when the macro band changes (avoids per-sample work) */
    double   x_lastmac;    /* last macro value we derived a band from */
    int      x_haveband;   /* has a band been derived yet? */
    int      x_wave;       /* current pulsaret waveform (discrete) */
    double   x_fratio;     /* formant ratio                        */
    double   x_duty;       /* window/duty width (clamped positive)  */
    double   x_mask;       /* masking (drop) probability            */
    double   x_drive;      /* drive amount                          */

    unsigned x_rng;
} t_alien_pulsar;

/* minimum window/duty width: guarantees gp = phase/duty stays finite and
 * bounded, and keeps the pulsaret audible (no zero-width silence) */
#define DUTY_MIN 0.02

static unsigned rn(t_alien_pulsar *x)
{
    x->x_rng ^= x->x_rng << 13; x->x_rng ^= x->x_rng >> 17;
    x->x_rng ^= x->x_rng << 5;  return x->x_rng;
}
static double frnd(t_alien_pulsar *x){ return (double)(rn(x)&0xffffff)/(double)0x1000000; }

static inline double softclip(double v)
{
    if (v >  3.0) return 1.0;
    if (v < -3.0) return -1.0;
    return v * (27.0 + v*v) / (27.0 + 9.0*v*v);
}

/* one cycle of the pulsaret waveform at phase p (0..1), in ~[-1,1] */
static double pulsaret(int wave, double p)
{
    switch (wave) {
        case 1:  return 2.0 * p - 1.0;                       /* saw */
        case 2:  return 4.0 * fabs(p - 0.5) - 1.0;           /* triangle */
        case 3:  return (p < 0.5) ? 1.0 : -1.0;              /* pulse */
        case 4: {                                            /* odd-harmonic */
            double s = sin(TWOPI * p);
            s += sin(3.0 * TWOPI * p) / 3.0;
            s += sin(5.0 * TWOPI * p) / 5.0;
            return s * 0.72;
        }
        default: return sin(TWOPI * p);                      /* sine */
    }
}

/* ------------------------------------------------------------------ */

/* map the 0..1 macro onto the whole parameter set (curated morph).
 *
 * wave is DISCRETE: split t into sub-ranges that pick softer -> harsher
 * pulsarets (sine -> triangle -> odd-harmonic -> saw -> pulse).  The
 * continuous controls sweep monotonically toward the aggressive end:
 * the formant ratio rises (dark -> bright vowel), the duty narrows
 * (smooth tone -> granular clicks), masking and drive grow (steady ->
 * broken and driven). */
static inline void pulsar_macro(double t, int *wave, double *fratio,
                                double *duty, double *mask, double *drive)
{
    double d;

    /* sanitise: t is expected in [0,1); guard NaN/inf/out-of-range */
    if (!(t >= 0.0 && t < 1.0)) t = 0.0;

    /* discrete waveform, harshening in five bands */
    if      (t < 0.20) *wave = 0;   /* sine        */
    else if (t < 0.40) *wave = 2;   /* triangle    */
    else if (t < 0.60) *wave = 4;   /* odd-harmonic (bandlimited-ish) */
    else if (t < 0.80) *wave = 1;   /* saw         */
    else               *wave = 3;   /* pulse       */

    *fratio = 1.0 + 20.0 * t * t;                 /* low -> high formant  */

    /* open tone -> tight clicks, but never zero-width (would divide by
     * zero in the perform loop and blow up to Inf) */
    d = 1.0 - 0.94 * t;
    if (d < DUTY_MIN) d = DUTY_MIN;
    if (d > 1.0)      d = 1.0;
    *duty = d;

    *mask   = (t < 0.5) ? 0.0 : (t - 0.5) / 0.5 * 0.6;  /* steady -> broken */
    *drive  = (t < 0.4) ? 0.0 : (t - 0.4) / 0.6;   /* clean -> driven      */
}

static t_int *alien_pulsar_perform(t_int *w)
{
    t_alien_pulsar *x = (t_alien_pulsar *)(w[1]);
    t_sample *fin = (t_sample *)(w[2]);
    t_sample *min = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    int nsamp = (int)(w[5]);

    double sr = x->x_sr;
    double nyq = sr * 0.49;
    int wave;
    double fratio, duty, mask, drive;

    /* --- BLOCK RATE: derive discrete quantities ONCE from the macro's
     * first sample, and only when it actually changes.  This keeps the
     * heavy branchy band-select / parameter mapping out of the per-sample
     * loop (fixes the stutter/CPU spikes). */
    {
        double mac0 = min[0];
        double t;
        if (!isfinite(mac0)) mac0 = 0.0;
        if (!x->x_haveband || mac0 != x->x_lastmac) {
            /* wrap the macro into [0,100) then normalise to [0,1) */
            t = mac0 - 100.0 * floor(mac0 * 0.01);
            if (!(t >= 0.0 && t < 100.0)) t = 0.0;   /* NaN/inf guard */
            t *= 0.01;
            pulsar_macro(t, &x->x_wave, &x->x_fratio,
                         &x->x_duty, &x->x_mask, &x->x_drive);
            x->x_lastmac  = mac0;
            x->x_haveband = 1;
        }
    }
    wave   = x->x_wave;
    fratio = x->x_fratio;
    duty   = x->x_duty;               /* already clamped >= DUTY_MIN */
    mask   = x->x_mask;
    drive  = x->x_drive;

    while (nsamp--) {
        double ftarget = *fin++;
        double o = 0.0;
        double y;

        if (!isfinite(ftarget)) ftarget = 0.01;
        if (ftarget < 0.01)     ftarget = 0.01;    /* f0 >= ~0.01, no /0 */
        if (ftarget > 8000.0)   ftarget = 8000.0;
        x->x_freq = ftarget;

        /* advance the pulsar (emission) clock; on wrap decide masking */
        x->x_phase += x->x_freq / sr;
        if (x->x_phase >= 1.0) {
            x->x_phase -= 1.0;
            if (x->x_phase >= 1.0) x->x_phase = 0.0;
            x->x_fphase = 0.0;
            x->x_active = (frnd(x) >= mask);        /* mask = drop probability */
        }

        if (x->x_active && x->x_phase < duty) {
            /* position within the pulsaret, 0..1 (duty >= DUTY_MIN > 0) */
            double gp = x->x_phase / duty;
            /* raised-cosine (Hann) window -> click-free emission */
            double win = 0.5 - 0.5 * cos(TWOPI * gp);
            /* formant frequency in Hz, tracking pitch, clamped < Nyquist */
            double ff = x->x_freq * fratio;
            if (ff < 0.01)  ff = 0.01;
            if (ff > nyq)   ff = nyq;
            x->x_fphase += ff / sr;
            if (x->x_fphase >= 1.0) x->x_fphase -= floor(x->x_fphase);
            o = pulsaret(wave, x->x_fphase) * win;
        } else {
            x->x_fphase = 0.0;
        }

        if (drive > 0.0)
            o = softclip(o * (1.0 + drive * 6.0)) / (1.0 + drive * 1.5);
        else
            o = softclip(o);

        y = o * 0.952;                              /* nominal ~unity level */
        if (!isfinite(y)) y = 0.0;                  /* NaN/Inf guard */
        if (y >  1.5) y =  1.5;                      /* hard safety bound */
        if (y < -1.5) y = -1.5;
        *out++ = (t_sample)y;
    }
    return (w + 6);
}

static void alien_pulsar_dsp(t_alien_pulsar *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    dsp_add(alien_pulsar_perform, 5, x,
            sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_pulsar_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_pulsar *x = (t_alien_pulsar *)pd_new(alien_pulsar_tilde_class);
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    x->x_phase = 0.0; x->x_fphase = 0.0; x->x_freq = 110.0;
    x->x_active = 1;
    x->x_lastmac = 0.0; x->x_haveband = 0;
    x->x_wave = 0; x->x_fratio = 1.0; x->x_duty = 1.0;
    x->x_mask = 0.0; x->x_drive = 0.0;
    x->x_rng = 22221u;

    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);   /* macro */
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_pulsar_tilde_setup(void)
{
    alien_pulsar_tilde_class = class_new(gensym("alien_pulsar~"),
        (t_newmethod)alien_pulsar_new, 0, sizeof(t_alien_pulsar),
        CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_pulsar_tilde_class, t_alien_pulsar, x_f);
    class_addmethod(alien_pulsar_tilde_class, (t_method)alien_pulsar_dsp, gensym("dsp"), A_CANT, 0);
}
