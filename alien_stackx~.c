/* alien_stackx~ -- ANGRY, WOBBLE, SQUELCHY -- but CLEAR -- bass
 *
 * A bass built from the alien_stack~ ADDITIVE PARTIAL ENGINE (a perfectly
 * harmonic, band-limited partial stack -- harmonic / fifths / bell ratio sets).
 * That clean, rich, tonal source is then made aggressive without going noisy:
 *
 *     additive stack -> wavefold (al_fold) + drive (al_drive) growl ->
 *     WOB resonant ladder (cutoff = MACRO, per-sample, smoothed) ->
 *     optional resonant SVF bandpass for vocal squelch -> DC block -> softclip
 *
 * with a rock-solid CLEAN SINE SUB one octave below so the low end stays huge.
 * Folding a harmonic source keeps the spectrum harmonic (tonal), so even at
 * full aggression the flatness stays LOW -- this is the CLEANEST/most musical
 * of the aggressive basses, a squelchy harmonic bass, not noise.
 *
 * THE WOB (inlet 1, macro 0..100, read PER SAMPLE, one-pole smoothed): drives
 * the ladder CUTOFF, the fold amount and the drive/aggression simultaneously,
 * so a fast LFO into inlet 1 gives a big zipper-free squelchy wobble.
 *
 * THE BRAIN (al_wander): a slow autonomous drift over partial count / stretch /
 * tilt / fold / resonance so a held note evolves and finds new tones -- staying
 * harmonic and tonal. Rate & depth scale with the macro.
 *
 * SAFETY (build uses -ffast-math -> rely on magnitude clamps): the additive
 * engine keeps its Nyquist band-limiting + unity normalisation; f0 clamped to
 * [0.01, 0.45*sr]; ladder/SVF stable; state denormal-flushed and finite-guarded;
 * object-level guard `if(!(s>-1e6 && s<1e6)) s=0;` then bound ~[-1.5,1.5].
 *
 *   inlet 0   signal/float   fundamental in Hz         (CLASS_MAINSIGNALIN)
 *   inlet 1   signal/float   MACRO 0..100 (the WOB)    -- LFO this for the wobble
 *   outlet 0  signal
 */

#include "m_pd.h"
#include "alien.h"

#define MAXP 48
#define MAXN 32

static t_class *alien_stackx_class;

/* the ratio sets the engine morphs between, increasing inharmonicity:
 * harmonic organ, stacked fifths, then a bell (from alien_stack~). */
#define NMODE 3
static const double MODE_RATIO[NMODE][MAXP] = {
    {0},  /* 0 harmonic: filled 1,2,3,4... at load time */
    {1,1.5,2.25,3.375,5.0625,7.594,11.39,17.09,25.63,38.44,57.67,86.5},   /* fifths */
    {1,2.00,3.01,4.15,5.43,6.79,8.21,9.33,10.9,12.4,14.1,15.9}            /* bell */
};

typedef struct _alien_stackx {
    t_object   x_obj;
    t_float    x_f;             /* scalar fallback for inlet 0 */
    double     x_sr;

    /* ---- additive engine state (copied from alien_stack~) ---- */
    double     x_ph[MAXP];      /* partial phases */
    double     x_shph[MAXP];    /* shimmer LFO phases */
    double     x_ratio[MAXP];   /* current ratio set */
    int        x_lastmode;

    int        x_n;             /* active partial count */
    double     x_amp[MAXP];     /* per-partial amplitude */
    double     x_finc[MAXP];    /* per-partial phase increment */
    double     x_active[MAXP];  /* 1 if below Nyquist else 0 */
    double     x_shinc[MAXP];   /* shimmer LFO increment */
    double     x_shdepth;
    double     x_norm;          /* peak normalisation */
    double     x_lastmac;
    double     x_lastf0;
    int        x_haveblock;

    /* ---- bass processing chain ---- */
    al_phasor  sub;             /* clean sine sub-octave */
    al_ladder  lad;             /* WOB resonant ladder */
    al_svf     bpf;             /* vocal squelch bandpass */
    al_dc      dc;
    al_wander  wnd;             /* the evolution brain */
    al_rng     rng;
    double     msm;             /* smoothed macro (anti-zipper) */
} t_alien_stackx;

/* load ratio set for a discrete mode */
static void load_mode(t_alien_stackx *x, int mode)
{
    int i;
    if (mode == 0) for (i = 0; i < MAXP; i++) x->x_ratio[i] = i + 1;
    else           for (i = 0; i < MAXP; i++) x->x_ratio[i] = MODE_RATIO[mode][i];
    x->x_lastmode = mode;
}

/* map 0..1 macro onto the partial-engine parameter set (curated morph).
 * brain params (wn/ws/wt/wf) drift partial count / stretch / tilt / fold. */
static inline void stack_macro(double t, double wn, double ws, double wt, double wf,
                               int *n, double *stretch, double *tilt,
                               double *shimmer, double *shimrate, double *odd,
                               int *mode)
{
    /* keep it tonal: harmonic for the lower half, fifths mid, bell top */
    *mode = (t < 0.5) ? 0 : (t < 0.8 ? 1 : 2);

    *n        = (int)(6.0 + 22.0 * t + 6.0 * wn);   /* count drifts with brain   */
    if (*n < 1)    *n = 1;
    if (*n > MAXN) *n = MAXN;
    *stretch  = 1.0 + 0.5 * t * t + 0.12 * (ws - 0.5); /* mild spread, tonal     */
    *tilt     = -0.4 + 1.9 * t + 0.4 * (wt - 0.5);  /* dark -> brighter          */
    *shimmer  = (t < 0.5) ? 0.0 : (t - 0.5) / 0.5 * 0.5; /* gentle life          */
    *shimrate = 0.25 + 1.2 * t;
    *odd      = 0.0;                                 /* keep full harmonic series */
    (void)wf;
}

/* rebuild per-block partial tables from a single macro (the smoothed one) + f0 */
static void build_block(t_alien_stackx *x, double mac, double f0,
                        double wn, double ws, double wt, double wf)
{
    double sr = x->x_sr, nyq = sr * 0.48;
    double t, stretch, tilt, shimmer, shimrate, odd;
    double sum = 0.0;
    int n, mode, i;

    if (!(mac == mac)) mac = 0.0;
    t = mac - 100.0 * floor(mac * 0.01);
    if (!(t >= 0.0 && t <= 100.0)) t = 0.0;
    t *= 0.01;

    stack_macro(t, wn, ws, wt, wf, &n, &stretch, &tilt, &shimmer, &shimrate,
                &odd, &mode);

    if (mode != x->x_lastmode) load_mode(x, mode);
    if (n < 1)    n = 1;
    if (n > MAXN) n = MAXN;

    if (!(f0 == f0)) f0 = 0.01;
    if (f0 < 0.01)   f0 = 0.01;
    if (f0 > sr*0.45) f0 = sr*0.45;

    x->x_n       = n;
    x->x_shdepth = (shimmer > 0.0) ? shimmer * 0.010 : 0.0;

    for (i = 0; i < n; i++) {
        double r = x->x_ratio[i];
        double fr, a;

        if (r <= 0.0) {
            x->x_active[i]=0.0; x->x_amp[i]=0.0; x->x_finc[i]=0.0; x->x_shinc[i]=0.0;
            continue;
        }
        fr = f0 * pow(r, stretch);
        if (fr > nyq || fr < 1.0) {
            x->x_active[i]=0.0; x->x_amp[i]=0.0; x->x_finc[i]=0.0; x->x_shinc[i]=0.0;
            continue;
        }
        a = 1.0 / (double)(i + 1);
        if (tilt != 0.0) a *= pow(r, -tilt);
        if (odd > 0.0 && (i & 1)) a *= 1.0 - odd;
        if (!(a == a) || a < 0.0) a = 0.0;

        x->x_active[i] = 1.0;
        x->x_amp[i]    = a;
        x->x_finc[i]   = fr / sr;
        x->x_shinc[i]  = (x->x_shdepth > 0.0)
                       ? shimrate * (0.6 + 0.4 * (double)i / n) / sr : 0.0;
        sum += a;
    }

    x->x_norm = (sum > 1e-6) ? (1.0 / sum) : 1.0;
    x->x_lastmac = mac;
    x->x_lastf0  = f0;
    x->x_haveblock = 1;
}

/* run the additive engine for one sample, return the summed partials (~unity) */
static inline double stack_engine(t_alien_stackx *x)
{
    double y = 0.0, shdepth = x->x_shdepth;
    int i, n = x->x_n;
    for (i = 0; i < n; i++) {
        double det = 1.0;
        if (x->x_active[i] == 0.0) continue;
        if (shdepth > 0.0) {
            x->x_shph[i] += x->x_shinc[i];
            if (x->x_shph[i] >= 1.0) x->x_shph[i] -= 1.0;
            det = 1.0 + shdepth * sin(x->x_shph[i] * AL_2PI);
        }
        x->x_ph[i] += x->x_finc[i] * det;
        if (x->x_ph[i] >= 1.0) x->x_ph[i] -= 1.0;
        else if (x->x_ph[i] < 0.0) x->x_ph[i] += 1.0;
        y += x->x_amp[i] * sin(x->x_ph[i] * AL_2PI);
    }
    return y * x->x_norm;
}

static t_int *alien_stackx_perform(t_int *w)
{
    t_alien_stackx *x = (t_alien_stackx *)(w[1]);
    t_sample *fin = (t_sample *)(w[2]);
    t_sample *pin = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    int n = (int)(w[5]);
    double sr = x->x_sr;
    double smc = 1.0 - exp(-AL_2PI * 30.0 / sr);   /* macro smoothing coef */
    double msm = x->msm;

    al_denorm_off();

    while (n--) {
        double f0 = *fin++, pr = *pin++;
        double pn = (pr >= 0.0 && pr <= 100.0) ? pr*0.01 : al_param100(pr);
        double subv, eng, s, cut, fold, drv, bp;
        double wn, ws, wt, wf, wcut, wq, depth;

        if (!(f0 > 0.0)) f0 = 0.01;
        if (f0 < 0.01)   f0 = 0.01;
        if (f0 > sr*0.45) f0 = sr*0.45;

        /* smooth the macro so a stepped/fast LFO wobbles with no zipper */
        msm += smc * (pn - msm);
        if (!(msm >= 0.0 && msm <= 1.0)) msm = al_clamp(msm, 0.0, 1.0);

        /* THE BRAIN -- slow autonomous drift, rate & depth grow with macro */
        al_wander_tick(&x->wnd, &x->rng, 0.3 + msm*2.5, sr);
        depth = 0.15 + 0.85*msm;
        wn  = al_wander_val(&x->wnd,0); ws  = al_wander_val(&x->wnd,1);
        wt  = al_wander_val(&x->wnd,2); wf  = al_wander_val(&x->wnd,3);
        wcut= al_wander_val(&x->wnd,4); wq  = al_wander_val(&x->wnd,5);

        /* rebuild the partial tables from the SMOOTHED macro (per-block granularity
         * on the pow() heavy work) whenever macro/f0 drift enough or brain moves */
        {
            double mac = msm * 100.0;
            if (!x->x_haveblock || fabs(mac - x->x_lastmac) > 0.02
                                || fabs(f0 - x->x_lastf0) > 0.001)
                build_block(x, mac, f0, wn, ws, wt, wf);
        }

        /* ---- clean sine SUB one octave below (owns the low end) ---- */
        subv = al_sin01(al_phasor_tick(&x->sub, f0*0.5, sr));

        /* ---- the clean harmonic ADDITIVE STACK source ---- */
        eng = stack_engine(x);

        /* ---- ANGRY: wavefold a harmonic source (stays tonal) + drive ---- */
        fold = 0.06 + msm*0.42 + depth*(wf - 0.4)*0.35;
        eng = al_fold(eng, al_clamp(fold, 0.0, 0.95));
        drv = 0.10 + msm*0.5 + depth*(wt - 0.4)*0.2;
        eng = al_drive(eng, al_clamp(drv, 0.0, 0.95));

        /* ---- SQUELCH WOB: resonant ladder, cutoff = macro (per sample) ---- */
        cut = 60.0 * pow(2.0, 0.4 + msm*5.6 + depth*(wcut - 0.4)*2.6);
        cut = al_clamp(cut, 40.0, 11000.0);
        eng = al_ladder_tick(&x->lad, eng, cut,
                             0.55 + 0.35*msm + 0.10*depth*wq, sr);

        /* ---- extra vocal squelch: resonant bandpass tracking the cutoff ---- */
        bp = al_svf_bp(&x->bpf, eng, al_clamp(cut*1.3, 60.0, 11000.0),
                       0.6 + 0.3*msm, sr);
        eng = al_lerp(eng, bp, 0.25 + 0.35*msm);

        /* ---- mix: solid sine sub + the squelchy harmonic body ---- */
        s = (0.85 - 0.25*msm)*subv + (0.9 + 0.35*msm)*eng;
        s = al_dc_tick(&x->dc, s);

        if (!(s > -1e6 && s < 1e6)) {              /* object-level guard */
            s = 0.0;
            al_ladder_init(&x->lad); al_svf_init(&x->bpf);
        }
        s = al_softclip(s * 1.15);
        if (s >  1.5) s =  1.5; else if (s < -1.5) s = -1.5;
        *out++ = (t_sample)(s * 0.50);
    }

    x->msm = msm;
    return (w + 6);
}

static void alien_stackx_dsp(t_alien_stackx *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    if (x->x_sr < 1000) x->x_sr = 44100.0;
    dsp_add(alien_stackx_perform, 5, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec,
            (t_int)sp[0]->s_n);
}

static void *alien_stackx_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_stackx *x = (t_alien_stackx *)pd_new(alien_stackx_class);
    int i;
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100.0;

    for (i = 0; i < MAXP; i++) {
        x->x_ph[i]   = (double)i * 0.0137;
        x->x_shph[i] = (double)i / (double)MAXP;
        x->x_amp[i]=0.0; x->x_finc[i]=0.0; x->x_active[i]=0.0; x->x_shinc[i]=0.0;
    }
    x->x_lastmode = -1;
    load_mode(x, 0);
    x->x_haveblock = 0;
    x->x_lastmac = -1e30;
    x->x_lastf0  = -1e30;
    x->x_n = 0;
    x->x_norm = 1.0;
    x->x_shdepth = 0.0;

    al_phasor_init(&x->sub);
    al_ladder_init(&x->lad);
    al_svf_init(&x->bpf);
    al_dc_init(&x->dc);
    al_rng_seed(&x->rng, 0x57ac6bu);
    al_wander_init(&x->wnd, &x->rng, 6);
    x->msm = 0.0;

    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_stackx_tilde_setup(void)
{
    alien_stackx_class = class_new(gensym("alien_stackx~"),
        (t_newmethod)alien_stackx_new, 0, sizeof(t_alien_stackx),
        CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_stackx_class, t_alien_stackx, x_f);
    class_addmethod(alien_stackx_class, (t_method)alien_stackx_dsp,
                    gensym("dsp"), A_CANT, 0);
}
