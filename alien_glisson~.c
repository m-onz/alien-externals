/* alien_glisson~ -- GLISSON / GRAINLET synthesis DRONE voice
 *
 * Granular synthesis (Curtis Roads, "Microsound") in which every grain GLIDES
 * in pitch across its own short lifetime -- a glissando grain, a tiny swoop. A
 * self-scheduled cloud of ~24 overlapping Hann-windowed grains, each born at a
 * pitch near the fundamental (+/- a spread) and gliding to a different pitch by
 * the end of its life, gives a shimmering, swooping, comet-tail texture: a
 * PITCHED drone, not noise.
 *
 * The macro sweeps the whole cloud along a hand-tuned trajectory: at 0 a gentle
 * shimmering hum (sparse grains, tiny glides, narrow spread); at 100 a dense
 * swooping comet-tail cloud (many grains, big exponential glides, wide spread).
 * Grain density, size, pitch spread and glide amount all scale with the macro.
 *
 * The grain sum is normalised by expected overlap so it can't rail, then run
 * through a gentle resonant filter and a slow al_wander for autonomous motion,
 * a DC block and a softclip.
 *
 *   inlet 0   signal/float   fundamental in Hz   (CLASS_MAINSIGNALIN) -- the
 *                            centre pitch grains glide around
 *   inlet 1   signal/float   MACRO 0..100 -- density / glide / shimmer
 *   outlet 0  signal
 *
 * SAFETY (build uses -ffast-math -> magnitude clamps, not isfinite): grain
 * count clamped to capacity; grain pitches clamped [8, 0.45*sr]; amplitude
 * normalised by expected overlap; filter stable; f0 clamped [8, 0.45*sr];
 * an object-level guard `if(!(s>-1e6 && s<1e6)) s=0;` then bounds to ~[-1.5,1.5].
 *
 * build:
 *   (macOS: -bundle -undefined dynamic_lookup -o alien_glisson~.pd_darwin)
 */

#include "m_pd.h"
#include "alien.h"

#define GL_MAX   24            /* grain bank capacity */

static t_class *alien_glisson_class;

typedef struct _glgrain {
    int    act;
    double ph;                 /* oscillator phase 0..1 */
    double f0;                 /* start pitch (Hz) */
    double f1;                 /* end pitch (Hz) */
    double wp;                 /* window phase 0..1 */
    double winc;               /* window increment per sample */
} t_glgrain;

typedef struct _alien_glisson {
    t_object   x_obj;
    t_float    x_f;              /* scalar fallback for inlet 0 */
    double     x_sr;

    t_glgrain  gr[GL_MAX];
    double     gclk;             /* grain scheduler clock */
    double     msm;              /* smoothed macro (anti-zipper) */

    al_svf     lp;               /* gentle resonant filter */
    al_dc      dc;
    al_wander  wnd;              /* slow motion brain */
    al_rng     rng;
} t_alien_glisson;

static t_int *alien_glisson_perform(t_int *w)
{
    t_alien_glisson *x = (t_alien_glisson *)(w[1]);
    t_sample *fin = (t_sample *)(w[2]);
    t_sample *min = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    int n = (int)(w[5]);
    double sr = x->x_sr, nyq = sr * 0.45, fmin = 8.0;
    double smc = 1.0 - exp(-AL_2PI * 30.0 / sr);   /* macro smoothing coef */
    double msm = x->msm;
    int i;

    al_denorm_off();

    while (n--) {
        double f0 = *fin++, pr = *min++;
        double pn = al_param100(pr);               /* target macro 0..1 */
        double wdens, wspr, wglide, wcut, wq;
        double density, durms, spread, glide, cut, res;
        double s = 0.0, norm;

        if (!(f0 > 0.0)) f0 = 8.0;
        if (f0 < fmin) f0 = fmin;
        if (f0 > nyq)  f0 = nyq;

        /* smooth the macro so a fast LFO changes density/glide with no zipper */
        msm += smc * (pn - msm);
        if (!(msm >= 0.0 && msm <= 1.0)) msm = al_clamp(msm, 0.0, 1.0);

        /* slow autonomous motion brain (healthy floor so a held note drifts) */
        al_wander_tick(&x->wnd, &x->rng, 0.5 + msm * 1.2, sr);
        wdens  = al_wander_val(&x->wnd, 0);
        wspr   = al_wander_val(&x->wnd, 1);
        wglide = al_wander_val(&x->wnd, 2);
        wcut   = al_wander_val(&x->wnd, 3);
        wq     = al_wander_val(&x->wnd, 4);

        /* macro-driven cloud parameters (0 = gentle hum, 1 = comet cloud) */
        density = 12.0 + (30.0 + 220.0 * msm) * (0.4 + 0.6 * wdens); /* grains/s */
        durms   = 90.0 - 55.0 * msm;                 /* long soft -> shorter swoops */
        durms  *= (0.8 + 0.5 * wglide);
        if (durms < 12.0) durms = 12.0;
        spread  = 0.005 + (0.02 + 0.16 * msm) * (0.4 + 0.6 * wspr);  /* +/- ratio */
        glide   = (0.02 + 1.2 * msm) * (0.3 + 0.7 * wglide);         /* octaves */

        /* ---- schedule new grains ---- */
        x->gclk += density / sr;
        if (x->gclk >= 1.0) {
            x->gclk -= floor(x->gclk);
            for (i = 0; i < GL_MAX; i++) if (!x->gr[i].act) {
                double dm  = durms * (0.6 + 0.8 * al_rand(&x->rng));
                double sc  = 1.0 + spread * al_randb(&x->rng);        /* start ratio */
                double dir = al_randb(&x->rng);                      /* glide dir/amt */
                double gr  = pow(2.0, glide * dir);                  /* end/start ratio */
                double a, b;
                if (sc < 0.25) sc = 0.25; if (sc > 4.0) sc = 4.0;
                a = f0 * sc;
                b = a * gr;
                if (a < fmin) a = fmin; if (a > nyq) a = nyq;
                if (b < fmin) b = fmin; if (b > nyq) b = nyq;
                x->gr[i].act  = 1;
                x->gr[i].ph   = 0.0;
                x->gr[i].f0   = a;
                x->gr[i].f1   = b;
                x->gr[i].wp   = 0.0;
                x->gr[i].winc = 1000.0 / (dm * sr);
                break;
            }
        }

        /* ---- render active grains: each glides f0 -> f1 over its window ---- */
        for (i = 0; i < GL_MAX; i++) if (x->gr[i].act) {
            double wp = x->gr[i].wp;
            double win = 0.5 - 0.5 * cos(AL_2PI * wp);   /* Hann */
            /* exponential glide in pitch across the grain lifetime */
            double f = x->gr[i].f0 * pow(x->gr[i].f1 / x->gr[i].f0, wp);
            if (f < fmin) f = fmin; if (f > nyq) f = nyq;
            s += win * sin(AL_2PI * x->gr[i].ph);
            x->gr[i].ph += f / sr;
            if (x->gr[i].ph >= 1.0) x->gr[i].ph -= floor(x->gr[i].ph);
            x->gr[i].wp = wp + x->gr[i].winc;
            if (x->gr[i].wp >= 1.0) x->gr[i].act = 0;
        }

        /* normalise by expected simultaneous overlap so it can't rail.
         * overlap ~= density * (mean grain length in s); Hann sum ~ overlap/2. */
        norm = density * (durms * 0.001);
        if (norm < 1.0) norm = 1.0;
        s *= 1.6 / norm;

        /* gentle resonant filter, brain- and macro-driven, for shimmer/motion */
        cut = f0 * (2.0 + 6.0 * msm) * pow(2.0, (wcut - 0.5) * 2.0);
        cut = al_clamp(cut, 60.0, nyq);
        res = al_clamp(0.25 + 0.35 * msm + 0.25 * wq, 0.0, 0.9);
        s = al_svf_lp(&x->lp, s, cut, res, sr);

        s = al_dc_tick(&x->dc, s);

        if (!(s > -1e6 && s < 1e6)) {              /* object-level guard */
            s = 0.0;
            al_svf_init(&x->lp);
            al_dc_init(&x->dc);
            for (i = 0; i < GL_MAX; i++) x->gr[i].act = 0;
        }
        s = al_softclip(s * 1.4);                  /* makeup + bound */
        if (s >  1.5) s =  1.5; else if (s < -1.5) s = -1.5;
        *out++ = (t_sample)(s * 0.72);
    }

    x->msm = msm;
    return (w + 6);
}

static void alien_glisson_dsp(t_alien_glisson *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    if (x->x_sr < 1000.0) x->x_sr = 44100.0;
    dsp_add(alien_glisson_perform, 5, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec,
            (t_int)sp[0]->s_n);
}

static void *alien_glisson_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_glisson *x = (t_alien_glisson *)pd_new(alien_glisson_class);
    int i;
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100.0;

    al_rng_seed(&x->rng, 0x6c1553a7u);
    for (i = 0; i < GL_MAX; i++) x->gr[i].act = 0;
    x->gclk = 0.0;
    x->msm  = 0.0;

    al_svf_init(&x->lp);
    al_dc_init(&x->dc);
    al_wander_init(&x->wnd, &x->rng, 5);

    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_glisson_tilde_setup(void)
{
    alien_glisson_class = class_new(gensym("alien_glisson~"),
        (t_newmethod)alien_glisson_new, 0, sizeof(t_alien_glisson),
        CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_glisson_class, t_alien_glisson, x_f);
    class_addmethod(alien_glisson_class, (t_method)alien_glisson_dsp,
                    gensym("dsp"), A_CANT, 0);
}
