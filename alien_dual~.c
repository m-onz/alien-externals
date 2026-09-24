/* alien_dual~.c  --  complex oscillator, west coast
 *
 * Two oscillators that modulate each other, each through a wavefolder.
 * No filter anywhere.  Harmonics are generated rather than removed,
 * which is the whole difference between this and everything else in
 * your instrument set.
 *
 *   osc A <--> osc B     through-zero FM in both directions
 *     |         |
 *   fold A    fold B     sine wavefolders with symmetry offset
 *     \       /
 *      mix / ring
 *
 * Through-zero matters: at high index the modulated frequency goes
 * negative and the oscillator runs backwards rather than folding at
 * zero.  That is what gives clean bell tones instead of the muddy
 * clangour a non-TZ implementation produces.
 *
 * Cross-modulation makes it a feedback system, so past a certain index
 * it stops being predictable.  That region is the point.
 *
 *   inlet 0   signal/float   frequency of A in Hz
 *   inlet 1   signal/float   MACRO 0..100 (wrapped mod 100) -- one knob that
 *                            morphs the whole voice: a clean sine at 0, up
 *                            through reedy folded tones and ringing bells,
 *                            into a hard-synced cross-modulated chaos at 100.
 *                            It sweeps ratio, both FM indices, fold, bias,
 *                            ring, mix, drive and waveform together along a
 *                            hand-tuned trajectory.
 *
 *   outlet 0  signal         the mix
 *   outlet 1  signal         oscillator B alone, for use as a modulator
 *
 * build:
 *   gcc -shared -fPIC -O2 -o alien_dual~.pd_linux alien_dual~.c -lm
 */

#include "m_pd.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define OS 4

static t_class *alien_dual_tilde_class;

typedef struct _alien_dual {
    t_object x_obj;
    t_float  x_f;

    double   x_sr, x_pa, x_pb;
    double   x_lasta, x_lastb;      /* previous outputs, for cross FM */
    double   x_d1, x_d2, x_e1, x_e2;

    /* discrete macro state, derived once per block; -1 forces first update */
    int      x_wavea, x_waveb, x_sync;
} t_alien_dual;

static inline double osc_wave(double p, int wv)
{
    p -= floor(p);
    if (wv <= 0) return sin(p * 2.0 * M_PI);
    if (wv == 1) return 4.0 * fabs(p - 0.5) - 1.0;      /* triangle */
    return 2.0 * p - 1.0;                                /* saw      */
}

/* sine wavefolder with a DC offset before folding.  The offset is what
 * breaks the symmetry and lets even harmonics through. */
static inline double fold(double v, double amt, double bias)
{
    if (amt <= 0.0) return v;
    v = (v + bias) * (1.0 + amt * 5.0);
    return sin(v * 1.5707963267948966);
}

static inline double softclip(double v)
{
    if (v >  3.0) return 1.0;
    if (v < -3.0) return -1.0;
    return v * (27.0 + v*v) / (27.0 + 9.0*v*v);
}

/* ------------------------------------------------------------------ */

/* map the 0..1 macro onto the whole parameter set (curated morph).
 *
 * The trajectory walks the old presets in order of increasing energy:
 *   0.00 clean sine -> reedy folded tone -> ringing bell/metal ->
 *   1.00 hard-synced cross-modulated chaos.  Continuous parameters sweep
 *   monotonically brighter/harder; the two waveforms and the sync switch
 *   step through discrete sub-ranges of t. */
static inline void dual_macro(double t, double *ratio, double *ab, double *ba,
                              double *folda, double *foldb,
                              double *biasa, double *biasb,
                              double *ring, double *mix, double *drive)
{
    /* ratio climbs from unison into wide inharmonic spacing */
    *ratio = 1.0 + 6.0 * t * t;                    /* 1 -> 7           */

    /* FM indices open up; ba (feedback) lags ab and only bites up top   */
    *ab    = 4.0 * t;                              /* 0 -> 4           */
    *ba    = (t < 0.45) ? 0.0 : 3.5 * (t - 0.45) / 0.55;  /* feedback late */

    /* wavefolding fades in through the middle, bias adds even harmonics */
    *folda = (t < 0.15) ? 0.0 : 0.7 * (t - 0.15) / 0.85;
    *foldb = (t < 0.55) ? 0.0 : 0.6 * (t - 0.55) / 0.45;
    *biasa = 0.4 * t;                              /* asymmetry -> reedy */
    *biasb = 0.25 * t;

    /* ring modulation blooms in the metallic upper-mid                  */
    *ring  = (t < 0.55) ? 0.0 : 0.6 * (t - 0.55) / 0.45;

    /* mix leans toward B as B gets more interesting                     */
    *mix   = 0.35 * t;

    /* drive rides gently until the top, then grits hard                 */
    *drive = 0.1 + ((t < 0.7) ? 0.2 * t : (0.14 + 0.6 * (t - 0.7) / 0.3));
}

/* Discrete macro quantities (waveform select + sync switch).  Derived ONCE
 * PER BLOCK from the macro's first sample so mode changes never thrash the
 * per-sample loop. */
static inline void dual_macro_discrete(double t, int *wavea, int *waveb,
                                       int *sync)
{
    /* discrete waveforms: sine low, triangle mid, saw-ish high          */
    *wavea = (t < 0.4) ? 0 : (t < 0.8 ? 1 : 2);
    *waveb = (t < 0.6) ? 0 : 1;

    /* hard sync only in the chaotic top quarter                         */
    *sync  = (t >= 0.75);
}

static t_int *alien_dual_perform(t_int *w)
{
    t_alien_dual *x = (t_alien_dual *)(w[1]);
    t_sample *fin  = (t_sample *)(w[2]);
    t_sample *min  = (t_sample *)(w[3]);
    t_sample *outa = (t_sample *)(w[4]);
    t_sample *outb = (t_sample *)(w[5]);
    int nsamp = (int)(w[6]);

    double sr = x->x_sr;
    if (!(sr > 0.0)) sr = 44100.0 * OS;           /* sane fallback */

    /* ---- once-per-block: discrete macro from the FIRST macro sample.
     * Waveform select + sync switch are stepped here, not per-sample, so
     * they only change on an actual block boundary and never thrash the
     * inner loop.  Sync's phase reset fires only on an integer change. */
    {
        double mac0 = min[0];
        double t0 = mac0 - 100.0 * floor(mac0 * 0.01);
        int wavea, waveb, sync;
        if (!(t0 >= 0.0 && t0 <= 100.0)) t0 = 0.0;  /* NaN/inf guard */
        t0 *= 0.01;
        dual_macro_discrete(t0, &wavea, &waveb, &sync);
        x->x_wavea = wavea;
        x->x_waveb = waveb;
        x->x_sync  = sync;
    }

    /* filter coefficient is a constant -- hoist it out of every loop */
    {
    const double wc = tan(M_PI * 0.11);
    const double a  = wc / (1.0 + wc);
    const int    wavea = x->x_wavea, waveb = x->x_waveb, sync = x->x_sync;

    while (nsamp--) {
        double f0 = *fin++, mac = *min++;
        double t, ratio, ab, ba, folda, foldb, biasa, biasb;
        double ring, mix, drive;
        double acc = 0.0, accb = 0.0;
        double fa, fb;
        int o;

        /* wrap the macro into [0,100) then normalise to [0,1) */
        t = mac - 100.0 * floor(mac * 0.01);
        if (!(t >= 0.0 && t <= 100.0)) t = 0.0;   /* NaN/inf guard */
        t *= 0.01;
        dual_macro(t, &ratio, &ab, &ba, &folda, &foldb, &biasa, &biasb,
                   &ring, &mix, &drive);

        fa = folda; fb = foldb;

        /* clamp everything that feeds phase increments or folds so nothing
         * can blow up: f0 finite and non-zero, ratio bounded, FM indices
         * bounded, fold amounts in [0,1]. */
        if (!(f0 == f0)) f0 = 0.01;               /* NaN -> floor */
        if (f0 < 0.01) f0 = 0.01;
        if (f0 > 10000.0) f0 = 10000.0;
        if (ratio < 0.01) ratio = 0.01; else if (ratio > 16.0) ratio = 16.0;
        if (ab < 0.0) ab = 0.0; else if (ab > 8.0) ab = 8.0;
        if (ba < 0.0) ba = 0.0; else if (ba > 8.0) ba = 8.0;
        if (fa < 0.0) fa = 0.0; else if (fa > 1.0) fa = 1.0;
        if (fb < 0.0) fb = 0.0; else if (fb > 1.0) fb = 1.0;
        if (ring < 0.0) ring = 0.0; else if (ring > 1.0) ring = 1.0;
        if (mix  < 0.0) mix  = 0.0; else if (mix  > 1.0) mix  = 1.0;
        if (drive < 0.0) drive = 0.0; else if (drive > 2.0) drive = 2.0;

        for (o = 0; o < OS; o++) {
            double dta, dtb, va, vb, y;
            int wrapped;

            /* through-zero: the increment may be negative, so the
             * oscillator simply runs backwards.  x_lasta/x_lastb are the
             * bounded (folded) previous outputs, so the FM term is bounded;
             * the hard clamp below is the final safety net. */
            dta = (f0 + ba * f0 * x->x_lastb) / sr;
            dtb = (f0 * ratio + ab * f0 * ratio * x->x_lasta) / sr;
            if (!(dta == dta)) dta = 0.0;         /* NaN guard */
            if (!(dtb == dtb)) dtb = 0.0;
            if (dta >  0.49) dta =  0.49;
            else if (dta < -0.49) dta = -0.49;
            if (dtb >  0.49) dtb =  0.49;
            else if (dtb < -0.49) dtb = -0.49;

            x->x_pa += dta;
            wrapped = (x->x_pa >= 1.0 || x->x_pa < 0.0);
            x->x_pa -= floor(x->x_pa);

            x->x_pb += dtb;
            x->x_pb -= floor(x->x_pb);
            if (sync && wrapped) x->x_pb = 0.0;

            va = osc_wave(x->x_pa, wavea);
            vb = osc_wave(x->x_pb, waveb);

            va = fold(va, fa, biasa);
            vb = fold(vb, fb, biasb);

            /* keep the feedback state bounded and finite */
            if (!(va == va)) va = 0.0;
            else if (va >  2.0) va =  2.0; else if (va < -2.0) va = -2.0;
            if (!(vb == vb)) vb = 0.0;
            else if (vb >  2.0) vb =  2.0; else if (vb < -2.0) vb = -2.0;
            x->x_lasta = va;
            x->x_lastb = vb;

            y = va * (1.0 - mix) + vb * mix;
            if (ring > 0.0) y = y * (1.0 - ring) + va * vb * ring;
            if (drive > 0.0)
                y = softclip(y * (1.0 + drive * 8.0)) / (1.0 + drive * 2.0);

            x->x_d1 += a * (y  - x->x_d1);
            x->x_d2 += a * (x->x_d1 - x->x_d2);
            x->x_e1 += a * (vb - x->x_e1);
            x->x_e2 += a * (x->x_e1 - x->x_e2);
            acc  = x->x_d2;
            accb = x->x_e2;
        }

        /* final finite + bounded output.  isfinite catches any NaN/Inf that
         * slipped through the filter state; soft-limit to ~[-1.5,1.5]. */
        if (!isfinite(acc)) { acc = 0.0; x->x_d1 = x->x_d2 = 0.0; }
        if (!isfinite(accb)){ accb = 0.0; x->x_e1 = x->x_e2 = 0.0; }
        acc  *= 0.8;
        accb *= 0.8;
        if (acc  >  1.5) acc  =  1.5; else if (acc  < -1.5) acc  = -1.5;
        if (accb >  1.5) accb =  1.5; else if (accb < -1.5) accb = -1.5;

        *outa++ = (t_sample)(acc * 0.57);
        *outb++ = (t_sample)(accb * 0.57);
    }
    }
    return (w + 7);
}

static void alien_dual_dsp(t_alien_dual *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr * OS;
    dsp_add(alien_dual_perform, 6, x,
            sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, sp[3]->s_vec,
            (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_dual_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_dual *x = (t_alien_dual *)pd_new(alien_dual_tilde_class);
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr() * OS;
    if (x->x_sr < 1000) x->x_sr = 44100.0 * OS;
    x->x_pa = 0.0; x->x_pb = 0.25;
    x->x_lasta = x->x_lastb = 0.0;
    x->x_d1 = x->x_d2 = x->x_e1 = x->x_e2 = 0.0;
    x->x_wavea = x->x_waveb = x->x_sync = 0;

    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_dual_tilde_setup(void)
{
    alien_dual_tilde_class = class_new(gensym("alien_dual~"),
        (t_newmethod)alien_dual_new, 0, sizeof(t_alien_dual),
        CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_dual_tilde_class, t_alien_dual, x_f);
    class_addmethod(alien_dual_tilde_class, (t_method)alien_dual_dsp,
                    gensym("dsp"), A_CANT, 0);
}
