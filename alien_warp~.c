/* alien_warp~ -- OSCILLATOR-WARP bass (Serum-2-style warp modes)
 *
 * The squelch/movement here comes entirely from modulating the oscillator's
 * WARP parameters -- the WAVESHAPE ITSELF warps. There is NO modulated lowpass
 * anywhere; nothing filters the wobble in. A source ramp/phasor is bent, folded,
 * pulse-width-modulated and hard-synced, and the amount of each warp is driven
 * by the macro. Feed an LFO / S&H into inlet 1 and the timbre squelches by
 * warping the wave, not by sweeping a filter.
 *
 * THE WARP MODES (all applied to phase/shape of a band-limited-ish source):
 *   - PWM        : pulse width of a square derived from the phase
 *   - ASYMMETRY  : skew/bend the phase ramp so it is lopsided (fast-then-slow)
 *   - MIRROR/FLIP: fold the phase around a movable point (triangle-ish)
 *   - HARD SYNC  : reset the source phase to a higher sync-frequency phasor
 *   - WAVEFOLD   : al_fold on the resulting wave (sine folding)
 *
 * THE MACRO (inlet 1, 0..100, read PER SAMPLE, smoothed): drives the overall
 * warp AMOUNT and ALSO tours which warp dominates -- a sweep goes roughly
 * PWM -> skew -> sync -> fold. So an LFO into inlet 1 = the movement.
 *
 * BASS: a clean sine SUB one octave below owns the lows.
 *
 * BRAIN: a slow al_wander drifts the sync ratio, skew bias and fold bias so a
 * held note keeps evolving. Light al_drive adds grit.
 *
 * SAFETY (-ffast-math -> rely on magnitude clamps): f0 clamped [0.01,0.45*sr];
 * sync ratio / skew / fold all bounded; the nonlinear warp + sync stage is
 * 2x oversampled and averaged to tame the harshest aliasing so it is aggressive
 * but not pure hash; a finite guard `if(!(s>-1e6 && s<1e6)) s=0;` then bounds to
 * ~[-1.5,1.5]. Integrated RMS aims near -12 dBFS, peak <= ~0.95.
 *
 *   inlet 0   signal/float   fundamental in Hz         (CLASS_MAINSIGNALIN)
 *   inlet 1   signal/float   MACRO 0..100 (the WARP)   -- LFO this for movement
 *   outlet 0  signal
 */

#include "m_pd.h"
#include "alien.h"

static t_class *alien_warp_class;

typedef struct _alien_warp {
    t_object   x_obj;
    t_float    x_f;              /* scalar fallback for inlet 0 */
    double     x_sr;
    double     mainp;           /* source oscillator phase 0..1 */
    double     syncp;           /* sync master phase 0..1 */
    al_phasor  sub;             /* clean sine sub-octave */
    al_dc      dc;              /* DC block */
    al_wander  wnd;             /* slow evolution brain */
    al_rng     rng;
    double     msm;             /* smoothed macro (anti-zipper) */
} t_alien_warp;

/* Warp a source phasor into a waveform, given the warp amount `amt` (0..1) and
 * which mode dominates via `sel` (0..1 tours PWM->skew->sync->fold). Runs at the
 * (possibly oversampled) rate; `dt` is hz/sr for that rate. `pw`,`skew`,`foldb`,
 * `syncr` are the (already bounded) warp controls. Reads+advances *mp,*sp. */
static inline double warp_source(double *mp, double *sp, double dt,
                                 double amt, double sel,
                                 double pw, double skew, double foldb,
                                 double syncr)
{
    double p, ph, w, s_pwm, s_skew, s_mir, s_fold, out;
    double gpwm, gskew, gsync, gfold;

    /* advance sync master; when it wraps, HARD-SYNC the source phase to it */
    *sp += syncr * dt;
    if (*sp >= 1.0) {
        *sp -= floor(*sp);
        *mp = *sp;                          /* hard reset: the sync edge */
    }
    /* advance source */
    *mp += dt;
    if (*mp >= 1.0 || *mp < 0.0) *mp -= floor(*mp);
    p = *mp;

    /* --- ASYMMETRY / skew: bend the phase ramp so it is lopsided --- */
    /* skew in ~(0.05..0.95) is the split point; below split time compresses */
    {
        double sp2 = al_clamp(skew, 0.05, 0.95);
        if (p < sp2) ph = 0.5 * p / sp2;
        else         ph = 0.5 + 0.5 * (p - sp2) / (1.0 - sp2);
        ph = al_lerp(p, ph, amt);           /* blend clean->skewed by amount */
    }

    /* --- source waves derived from the (skewed) phase --- */
    s_skew = al_sin01(ph);                                  /* skewed sine   */
    s_pwm  = al_squ01(p, al_clamp(pw, 0.02, 0.98)) * 0.9;   /* PWM square    */
    /* MIRROR/FLIP: fold phase around a point -> triangle-ish, movable apex */
    {
        double apex = al_clamp(0.5 + (foldb - 0.5) * 0.6, 0.1, 0.9);
        double m = (p < apex) ? p / apex : (1.0 - p) / (1.0 - apex);
        s_mir = 2.0 * m - 1.0;              /* -1..1 triangle, skewed apex */
    }
    /* HARD-SYNC voice: a plain saw on the synced source phase */
    s_fold = al_saw01(p);                  /* the raw synced ramp -> to fold */

    /* --- crossfade which warp dominates across sel (PWM->skew->sync->fold) - */
    {
        double x = al_clamp(sel, 0.0, 1.0) * 3.0;   /* 0..3 over 4 zones */
        double z0 = 1.0 - al_clamp(fabs(x - 0.0), 0.0, 1.0); /* PWM   */
        double z1 = 1.0 - al_clamp(fabs(x - 1.0), 0.0, 1.0); /* skew  */
        double z2 = 1.0 - al_clamp(fabs(x - 2.0), 0.0, 1.0); /* sync  */
        double z3 = 1.0 - al_clamp(fabs(x - 3.0), 0.0, 1.0); /* fold  */
        double g = z0 + z1 + z2 + z3; if (g < 1e-6) g = 1e-6;
        gpwm = z0/g; gskew = z1/g; gsync = z2/g; gfold = z3/g;
    }

    w = gpwm*s_pwm + gskew*s_skew + gsync*s_mir + gfold*s_fold;

    /* WAVEFOLD stage: fold amount grows with warp amount and fold-zone weight */
    {
        double fa = amt * (0.25 + 1.15 * gfold + 0.35 * gsync);
        out = al_fold(w, al_clamp(fa, 0.0, 1.2));
    }
    /* PWM-zone width modulation baked into the amount already via pw */
    (void)dt;
    return out;
}

static t_int *alien_warp_perform(t_int *w)
{
    t_alien_warp *x = (t_alien_warp *)(w[1]);
    t_sample *fin = (t_sample *)(w[2]);
    t_sample *pin = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    int n = (int)(w[5]);
    double sr = x->x_sr;
    double smc = 1.0 - exp(-AL_2PI * 40.0 / sr);    /* macro smoothing coef */
    double msm = x->msm;
    double osr = sr * 2.0;                          /* 2x oversample rate   */

    al_denorm_off();

    while (n--) {
        double f0 = *fin++, pr = *pin++;
        double pn = (pr >= 0.0 && pr <= 100.0) ? pr*0.01 : al_param100(pr);
        double amt, sel, pw, skew, foldb, syncr, subv, warp, s;
        double wsync, wskew, wfold, wpw;

        if (!(f0 > 0.0)) f0 = 0.01;
        if (f0 < 0.01)   f0 = 0.01;
        if (f0 > sr*0.45) f0 = sr*0.45;

        /* smooth the macro so a stepped/fast LFO warps with no zipper */
        msm += smc * (pn - msm);
        if (!(msm >= 0.0 && msm <= 1.0)) msm = al_clamp(msm, 0.0, 1.0);

        /* slow evolution brain; rate & depth grow a little with the macro */
        al_wander_tick(&x->wnd, &x->rng, 0.4 + msm*2.0, sr);
        wsync = al_wander_val(&x->wnd,0);
        wskew = al_wander_val(&x->wnd,1);
        wfold = al_wander_val(&x->wnd,2);
        wpw   = al_wander_val(&x->wnd,3);

        /* macro drives overall warp AMOUNT and tours which warp dominates */
        amt = al_clamp(0.12 + msm*0.9, 0.0, 1.0);
        sel = al_clamp(msm, 0.0, 1.0);

        /* bounded warp controls (macro + slow brain drift) */
        pw    = al_clamp(0.5 + (wpw - 0.5)*0.7 + msm*0.15, 0.05, 0.95);
        skew  = al_clamp(0.5 - msm*0.35 + (wskew - 0.5)*0.35, 0.08, 0.92);
        foldb = al_clamp(0.5 + (wfold - 0.5)*0.6, 0.1, 0.9);
        /* sync ratio 1..~4.5: rises with macro, drifts with the brain */
        syncr = al_clamp(1.0 + msm*3.0 + (wsync - 0.5)*1.2, 1.0, 4.6);

        /* ---- WARP SOURCE, 2x oversampled + averaged (tame the aliasing) --- */
        {
            double dt = f0 / osr;
            double a = warp_source(&x->mainp, &x->syncp, dt, amt, sel,
                                   pw, skew, foldb, syncr);
            double b = warp_source(&x->mainp, &x->syncp, dt, amt, sel,
                                   pw, skew, foldb, syncr);
            warp = 0.5 * (a + b);
        }

        /* light grit */
        warp = al_drive(warp, 0.1 + msm*0.25);

        /* ---- SUB: clean sine an octave below (owns the lows) ---- */
        subv = al_sin01(al_phasor_tick(&x->sub, f0*0.5, sr));

        /* ---- mix, DC block, final guard ---- */
        s = 0.62*subv + 0.85*warp;
        s = al_dc_tick(&x->dc, s);

        if (!(s > -1e6 && s < 1e6)) {               /* object-level guard */
            s = 0.0;
            x->mainp = x->syncp = 0.0;
            al_dc_init(&x->dc);
        }
        s = al_softclip(s * 1.05);
        if (s >  1.5) s =  1.5; else if (s < -1.5) s = -1.5;
        *out++ = (t_sample)(s * 0.46);
    }

    x->msm = msm;
    return (w + 6);
}

static void alien_warp_dsp(t_alien_warp *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    if (x->x_sr < 1000) x->x_sr = 44100.0;
    dsp_add(alien_warp_perform, 5, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec,
            (t_int)sp[0]->s_n);
}

static void *alien_warp_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_warp *x = (t_alien_warp *)pd_new(alien_warp_class);
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100.0;
    x->mainp = 0.0; x->syncp = 0.0; x->msm = 0.0;
    al_phasor_init(&x->sub); al_dc_init(&x->dc);
    al_rng_seed(&x->rng, 0x0a11e77u); al_wander_init(&x->wnd, &x->rng, 4);
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_warp_tilde_setup(void)
{
    alien_warp_class = class_new(gensym("alien_warp~"),
        (t_newmethod)alien_warp_new, 0, sizeof(t_alien_warp),
        CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_warp_class, t_alien_warp, x_f);
    class_addmethod(alien_warp_class, (t_method)alien_warp_dsp,
                    gensym("dsp"), A_CANT, 0);
}
