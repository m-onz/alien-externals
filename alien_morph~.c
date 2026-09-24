/* alien_morph~ -- WAVETABLE-POSITION MORPH bass (the modern wobble)
 *
 * The movement here is NOT a modulated lowpass cutoff. The "growl/squelch" comes
 * entirely from SCANNING the wavetable READ POSITION across a stack of single-
 * cycle frames that run from clean to nasty, plus FM sidebands whose index grows
 * with the scan. Feed an LFO / S&H / stepped shape into inlet 1 and it drags the
 * read position through the frame stack = a wavetable-scan growl, no filter sweep
 * involved.
 *
 * THE STACK (built at init, band-limited-ish -- bounded harmonic sums):
 *   0 sine  1 triangle  2 saw  3 square/pulse  4 harmonically-rich growl
 *   5 formant-bumped  6 folded/edgy  7 extra-folded/edgy
 * A morph path from a pure tone to a metallic, formant-y, folded snarl.
 *
 * THE MORPH (inlet 1, macro 0..100, read PER SAMPLE, one-pole smoothed): the
 * macro IS the read position along the stack. It crossfades between the two
 * adjacent frames (linear) AND each frame is read at f0 with linear interpolation.
 * So the macro strongly changes TIMBRE -- an LFO on inlet 1 scans the table.
 *
 * FM-WARP: an al_fmop modulator phase-modulates the wavetable read position; its
 * index scales with the macro so more scan = more moving sidebands (metallic
 * growl). Integer-ish ratio keeps it musical.
 *
 * BASS: a clean sine SUB one octave below keeps the fundamental solid.
 *
 * BRAIN: a SLOW al_wander drifts the frame-path offset / FM ratio / detune for
 * non-repeating evolution -- secondary to the macro scan, never the main move.
 *
 * A gentle FIXED tone-shaping tilt (one-pole, static coef) warms the top -- it
 * does NOT move with the macro, so it is not the source of the wobble.
 *
 * SAFETY (-ffast-math -> magnitude clamps): f0 clamped [0.01, 0.45*sr]; table
 * reads are index-masked; FM index bounded; finite guard then bound ~[-1.5,1.5].
 * Loudness normalised toward RMS ~ -12 dBFS, peak <= ~0.95.
 *
 *   inlet 0   signal/float   fundamental in Hz         (CLASS_MAINSIGNALIN)
 *   inlet 1   signal/float   MACRO 0..100 (the SCAN)   -- LFO this to growl
 *   outlet 0  signal
 */

#include "m_pd.h"
#include "alien.h"

#define MORPH_FRAMES 8            /* single-cycle frames in the stack        */
#define MORPH_TLEN   2048         /* samples per frame (power of two)         */
#define MORPH_MASK   (MORPH_TLEN - 1)

static t_class *alien_morph_class;

typedef struct _alien_morph {
    t_object   x_obj;
    t_float    x_f;              /* scalar fallback for inlet 0              */
    double     x_sr;
    double    *tab;             /* MORPH_FRAMES * MORPH_TLEN single cycles   */
    al_phasor  car;            /* wavetable read phasor (the carrier)       */
    al_phasor  sub;            /* clean sine sub-octave                     */
    al_fmop    mod;            /* FM warp modulator                         */
    al_wander  wnd;            /* slow non-repeating brain                  */
    al_rng     rng;
    double     msm;            /* smoothed macro (anti-zipper)              */
    double     tilt;           /* fixed tone-shaping one-pole state         */
    al_dc      dc;             /* output DC blocker                         */
} t_alien_morph;

/* one masked, linearly-interpolated read of frame `fr` at phase p (0..1) */
static inline double morph_read(const double *tab, int fr, double p)
{
    double fp = p * MORPH_TLEN;
    int i0 = (int)fp; double frac = fp - i0;
    int a = (fr * MORPH_TLEN) + (i0 & MORPH_MASK);
    int b = (fr * MORPH_TLEN) + ((i0 + 1) & MORPH_MASK);
    return al_lerp(tab[a], tab[b], frac);
}

/* Build the frame stack: each frame is a single cycle summed from a bounded
 * number of harmonics with a per-frame recipe (band-limited-ish), then peak-
 * normalised so every frame sits near +/-1 (so morphing does not jump level). */
static void morph_build(double *tab)
{
    int f, i, k;
    for (f = 0; f < MORPH_FRAMES; f++) {
        double *fr = tab + f * MORPH_TLEN;
        double peak = 1e-9;
        for (i = 0; i < MORPH_TLEN; i++) {
            double ph = (double)i / MORPH_TLEN;   /* 0..1                    */
            double th = AL_2PI * ph;
            double s = 0.0;
            switch (f) {
                case 0:  /* sine */
                    s = sin(th);
                    break;
                case 1:  /* triangle: odd harmonics, 1/k^2, alternating sign */
                    for (k = 1; k <= 15; k += 2) {
                        double sg = (((k - 1) / 2) & 1) ? -1.0 : 1.0;
                        s += sg * sin(k * th) / (double)(k * k);
                    }
                    break;
                case 2:  /* saw: all harmonics, 1/k */
                    for (k = 1; k <= 24; k++) s += sin(k * th) / (double)k;
                    break;
                case 3:  /* square/pulse: odd harmonics, 1/k */
                    for (k = 1; k <= 23; k += 2) s += sin(k * th) / (double)k;
                    break;
                case 4:  /* growl: dense mid/high stack, gentle rolloff */
                    for (k = 1; k <= 22; k++) {
                        double a = pow(0.86, k - 1);
                        s += a * sin(k * th + 0.3 * k);
                    }
                    break;
                case 5:  /* formant-bumped: saw-ish with a boosted band ~h5-8 */
                    for (k = 1; k <= 20; k++) {
                        double a = 1.0 / (double)k;
                        double bump = exp(-((k - 6.0) * (k - 6.0)) / 6.0);
                        s += (a + 1.6 * bump) * sin(k * th);
                    }
                    break;
                case 6:  /* folded/edgy: fold a saw sum */
                    for (k = 1; k <= 12; k++) s += sin(k * th) / (double)k;
                    s = al_fold(0.8 * s, 0.55);
                    break;
                default: /* extra-folded/edgy: more folds, brighter seed */
                    for (k = 1; k <= 16; k++) s += sin(k * th) / (double)k;
                    s = al_fold(0.9 * s, 0.9);
                    break;
            }
            fr[i] = s;
        }
        for (i = 0; i < MORPH_TLEN; i++) {
            double a = fabs(fr[i]); if (a > peak) peak = a;
        }
        for (i = 0; i < MORPH_TLEN; i++) fr[i] /= peak;   /* peak-normalise  */
    }
}

static t_int *alien_morph_perform(t_int *w)
{
    t_alien_morph *x = (t_alien_morph *)(w[1]);
    t_sample *fin = (t_sample *)(w[2]);
    t_sample *pin = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    int n = (int)(w[5]);
    double sr = x->x_sr;
    const double *tab = x->tab;
    double smc  = 1.0 - exp(-AL_2PI * 40.0 / sr);   /* macro smoothing coef  */
    double tltc = al_cut2coef(4500.0, sr);          /* FIXED tone tilt coef  */
    double msm  = x->msm;
    double tilt = x->tilt;

    al_denorm_off();

    while (n--) {
        double f0 = *fin++, pr = *pin++;
        double pn = (pr >= 0.0 && pr <= 100.0) ? pr * 0.01 : al_param100(pr);
        double subv, carr, pos, fpos, fmv, fmidx, mratio, s;
        double wpos, wratio, wdet;
        int fa, fb;

        if (!(f0 > 0.0)) f0 = 0.01;
        if (f0 < 0.01)    f0 = 0.01;
        if (f0 > sr * 0.45) f0 = sr * 0.45;

        /* smooth the macro so a stepped/fast LFO scans with no zipper */
        msm += smc * (pn - msm);
        if (!(msm >= 0.0 && msm <= 1.0)) msm = al_clamp(msm, 0.0, 1.0);

        /* SLOW brain -- non-repeating drift, NOT the main movement */
        al_wander_tick(&x->wnd, &x->rng, 0.35 + msm * 1.2, sr);
        wpos   = al_wander_bip(&x->wnd, 0);   /* frame-path offset          */
        wratio = al_wander_val(&x->wnd, 1);   /* FM ratio drift             */
        wdet   = al_wander_bip(&x->wnd, 2);   /* sub detune drift           */

        /* ---- SUB: clean sine one octave below (owns the lows) ---------- */
        {
            double subf = f0 * 0.5 * (1.0 + 0.002 * wdet);
            subv = al_sin01(al_phasor_tick(&x->sub, subf, sr));
        }

        /* ---- READ POSITION = the macro (this IS the growl) ------------- */
        /* macro maps across the whole stack; brain nudges it a little */
        pos = al_clamp(msm * (MORPH_FRAMES - 1) + wpos * 0.5,
                       0.0, (double)(MORPH_FRAMES - 1));
        fa   = (int)pos; if (fa > MORPH_FRAMES - 2) fa = MORPH_FRAMES - 2;
        fb   = fa + 1;
        fpos = pos - fa; if (fpos < 0.0) fpos = 0.0; if (fpos > 1.0) fpos = 1.0;

        /* ---- FM WARP: modulator phase-modulates the read position ------ */
        /* integer-ish ratio, drifting 1..3; index grows with the scan */
        mratio = 1.0 + floor(wratio * 3.0);                 /* 1,2,3        */
        fmidx  = 0.02 + msm * 0.22;                         /* bounded      */
        fmv    = al_fmop_tick(&x->mod, f0 * mratio, 0.0, 0.2, sr);
        {
            double p = al_phasor_tick(&x->car, f0, sr);
            /* phase-modulate the wavetable read by the FM output */
            p = al_wrap01(p + fmidx * fmv);
            /* crossfade the two adjacent frames, each read at the warped p */
            carr = al_lerp(morph_read(tab, fa, p),
                           morph_read(tab, fb, p), fpos);
        }

        /* ---- light drive for grit (grows a touch with the scan) -------- */
        carr = al_drive(carr, 0.08 + msm * 0.30);

        /* ---- FIXED tone tilt (static coef -- NOT a moving cutoff) ------ */
        /* a gentle high-shelf-ish warmth: blend some one-pole lowpass in */
        tilt += tltc * (carr - tilt);
        carr = 0.7 * carr + 0.3 * tilt;

        /* ---- mix sub + morph carrier ---------------------------------- */
        s = 0.9 * subv + 1.0 * carr;

        if (!(s > -1e6 && s < 1e6)) {           /* object-level guard       */
            s = 0.0;
            al_phasor_init(&x->car);
            tilt = 0.0;
        }
        s = al_dc_tick(&x->dc, s);              /* block DC before clip     */
        s = al_softclip(s);
        if (s >  1.5) s =  1.5; else if (s < -1.5) s = -1.5;
        *out++ = (t_sample)(s * 0.41);          /* toward RMS ~ -12 dBFS    */
    }

    x->msm  = msm;
    x->tilt = tilt;
    return (w + 6);
}

static void alien_morph_dsp(t_alien_morph *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    if (x->x_sr < 1000) x->x_sr = 44100.0;
    dsp_add(alien_morph_perform, 5, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec,
            (t_int)sp[0]->s_n);
}

static void *alien_morph_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_morph *x = (t_alien_morph *)pd_new(alien_morph_class);
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100.0;
    x->tab = (double *)getbytes(sizeof(double) * MORPH_FRAMES * MORPH_TLEN);
    if (x->tab) morph_build(x->tab);
    al_phasor_init(&x->car); al_phasor_init(&x->sub);
    al_fmop_init(&x->mod);
    al_rng_seed(&x->rng, 0x3a19b7u); al_wander_init(&x->wnd, &x->rng, 4);
    al_dc_init(&x->dc);
    x->msm = 0.0; x->tilt = 0.0;
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

static void alien_morph_free(t_alien_morph *x)
{
    if (x->tab) freebytes(x->tab, sizeof(double) * MORPH_FRAMES * MORPH_TLEN);
}

void alien_morph_tilde_setup(void)
{
    alien_morph_class = class_new(gensym("alien_morph~"),
        (t_newmethod)alien_morph_new, (t_method)alien_morph_free,
        sizeof(t_alien_morph), CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_morph_class, t_alien_morph, x_f);
    class_addmethod(alien_morph_class, (t_method)alien_morph_dsp,
                    gensym("dsp"), A_CANT, 0);
}
