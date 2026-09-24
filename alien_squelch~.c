/* alien_squelch~ -- modern dubstep SQUELCH bass (deep, self-evolving, talking)
 *
 * A split-band monster bass built to be WOBBLED. Underneath sits a rock-solid
 * sine SUB one octave below the note; above it a detuned supersaw cluster plus a
 * touch of 2-op feedback FM for metallic richness feeds a chain of
 *
 *     wavefold -> morphing VOWEL formants -> resonant ladder -> drive ->
 *     chorus -> DC block -> softclip
 *
 * with a crossover highpass so the sub owns the lows. It is richer and gnarlier
 * than alien_wob~/alien_talk~: two independent sources of motion run at once.
 *
 * THE WOB (inlet 1, macro 0..100, read PER SAMPLE): the user modulates this with
 * an LFO. It is mapped SIMULTANEOUSLY onto the ladder CUTOFF, the VOWEL position
 * and the FOLD amount, so a fast LFO into inlet 1 gives a big squelchy TALKING
 * wob with no zipper. The macro is smoothed with a one-pole so a stepped signal
 * still glides.
 *
 * THE COMPLEXITY (al_wander brain, 8 controls): a SLOW autonomous novelty brain
 * (independent of the fast wob) continuously drifts detune, fold, vowel, ladder
 * Q/res, sub level, drive and chorus, so on a held note the bass never repeats
 * and keeps finding new vowels and growls. Its DEPTH and RATE scale with the
 * macro -- macro 0 is calm, macro 100 is wild -- so the brain rides ON TOP of
 * the wob and makes the timbre evolve by itself.
 *
 * SAFETY (build uses -ffast-math -> rely on magnitude clamps, not isfinite): f0
 * clamped to [0.01, 0.45*sr]; the alien.h blocks are state-guarded; an
 * object-level final-sample guard `if(!(s>-1e6 && s<1e6)) s=0;` then bounds to
 * ~[-1.5,1.5] and scales near unity (~0.8). Sub-bass is welcome; HISS/RAIL are
 * fine for an aggressive bass as long as the peak stays bounded.
 *
 *   inlet 0   signal/float   fundamental in Hz         (CLASS_MAINSIGNALIN)
 *   inlet 1   signal/float   MACRO 0..100 (the WOB)    -- LFO this for the wobble
 *   outlet 0  signal
 */

#include "m_pd.h"
#include "alien.h"

static t_class *alien_squelch_class;

typedef struct _alien_squelch {
    t_object   x_obj;
    t_float    x_f;              /* scalar fallback for inlet 0 */
    double     x_sr;
    al_phasor  sub;             /* clean sine sub-octave */
    al_cluster clu;             /* detuned supersaw body */
    al_fm2     fm;              /* metallic richness */
    al_formants fmt;            /* a-e-i-o-u vowel morph */
    al_ladder  lad;            /* resonant ladder */
    al_chorus  ch;             /* width */
    al_dc      dc;             /* DC block */
    al_wander  wnd;            /* the novelty brain */
    al_rng     rng;
    double     hpx, hpy;        /* crossover highpass state */
    double     msm;             /* smoothed macro (anti-zipper) */
} t_alien_squelch;

static t_int *alien_squelch_perform(t_int *w)
{
    t_alien_squelch *x = (t_alien_squelch *)(w[1]);
    t_sample *fin = (t_sample *)(w[2]);
    t_sample *pin = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    int n = (int)(w[5]);
    double sr = x->x_sr;
    double hpa = exp(-AL_2PI * 90.0 / sr);          /* crossover HP ~90 Hz */
    double smc = 1.0 - exp(-AL_2PI * 40.0 / sr);    /* macro smoothing coef */
    double msm = x->msm;

    al_denorm_off();

    while (n--) {
        double f0 = *fin++, pr = *pin++;
        /* macro 0..1: clamp the useful range so exactly-100 reads full (not
         * wrapped to 0), still tolerant of out-of-range LFO drive */
        double pn = (pr >= 0.0 && pr <= 100.0) ? pr*0.01 : al_param100(pr);
        double subv, up, vowel, cut, fold, det, s, hp, fmv;
        double wv, wdet, wfold, wcut, wq, wsub, wdr, wch;
        double depth;

        if (!(f0 > 0.0)) f0 = 0.01;
        if (f0 < 0.01)   f0 = 0.01;
        if (f0 > sr*0.45) f0 = sr*0.45;

        /* smooth the macro so a stepped/fast LFO wobbles with no zipper */
        msm += smc * (pn - msm);
        if (!(msm >= 0.0 && msm <= 1.0)) msm = al_clamp(msm, 0.0, 1.0);

        /* THE BRAIN -- slow autonomous drift, rate & depth grow with the macro */
        al_wander_tick(&x->wnd, &x->rng, 0.5 + msm*4.5, sr);
        depth = 0.15 + 0.85*msm;                    /* macro scales brain depth */
        wv   = al_wander_val(&x->wnd,0); wdet = al_wander_val(&x->wnd,1);
        wfold= al_wander_val(&x->wnd,2); wcut = al_wander_val(&x->wnd,3);
        wq   = al_wander_val(&x->wnd,4); wsub = al_wander_val(&x->wnd,5);
        wdr  = al_wander_val(&x->wnd,6); wch  = al_wander_val(&x->wnd,7);

        /* ---- SUB: clean sine an octave below the note (owns the lows) ---- */
        subv = al_sin01(al_phasor_tick(&x->sub, f0*0.5, sr));
        subv *= 0.9 + 0.1*depth*wsub;

        /* ---- UPPER SOURCE: supersaw + a touch of feedback FM ------------- */
        det = 0.006 + msm*0.02 + depth*wdet*0.018;
        up  = al_cluster_tick(&x->clu, f0, det, sr) * 0.85;
        fmv = al_fm2_tick(&x->fm, f0, 2.0, 0.9 + 1.6*msm, 0.35, sr);
        up += (0.12 + 0.22*msm) * fmv;

        /* ---- WOB-driven FOLD (wob + brain together) --------------------- */
        fold = 0.10 + msm*0.6 + depth*(wfold - 0.3)*0.8;
        up = al_fold(up, fold);

        /* ---- WOB-driven morphing VOWEL formants ------------------------- */
        vowel = al_clamp(0.15 + msm*0.45 + depth*(wv - 0.4)*0.9, 0.0, 1.0);
        {
            double form = al_formants_tick(&x->fmt, up, vowel, 0.3 + depth*wq, sr);
            up = al_lerp(up, form, 0.35 + 0.55*msm);
        }

        /* ---- crossover highpass so the sub owns the low end ------------- */
        hp = hpa*(x->hpy + up - x->hpx); x->hpx = up; x->hpy = hp; up = hp;

        /* ---- WOB-driven resonant ladder (cutoff is the main squelch) ---- */
        cut = 70.0 * pow(2.0, 0.5 + msm*5.8 + depth*(wcut - 0.4)*3.2);
        cut = al_clamp(cut, 40.0, 12000.0);
        up = al_ladder_tick(&x->lad, up, cut, 0.25 + 0.55*msm + 0.15*depth*wq, sr);

        /* ---- drive + chorus (upper only; sub stays clean/mono) ---------- */
        up = al_drive(up, 0.15 + msm*0.45 + depth*wdr*0.25);
        up = al_chorus_tick(&x->ch, up, 4.0*(0.3 + depth*wch), 0.25, sr);

        /* ---- mix, DC block, final guard -------------------------------- */
        s = (0.75 - 0.35*msm)*subv + (0.9 + 0.5*msm)*up;
        s = al_dc_tick(&x->dc, s);

        if (!(s > -1e6 && s < 1e6)) {               /* object-level guard */
            s = 0.0;
            al_ladder_init(&x->lad);
            x->hpx = x->hpy = 0.0;
        }
        s = al_softclip(s);
        if (s >  1.5) s =  1.5; else if (s < -1.5) s = -1.5;
        *out++ = (t_sample)(s * 0.68);
    }

    x->msm = msm;
    return (w + 6);
}

static void alien_squelch_dsp(t_alien_squelch *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    if (x->x_sr < 1000) x->x_sr = 44100.0;
    dsp_add(alien_squelch_perform, 5, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec,
            (t_int)sp[0]->s_n);
}

static void *alien_squelch_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_squelch *x = (t_alien_squelch *)pd_new(alien_squelch_class);
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100.0;
    al_phasor_init(&x->sub); al_cluster_init(&x->clu, 5);
    al_fm2_init(&x->fm); al_formants_init(&x->fmt); al_ladder_init(&x->lad);
    al_chorus_init(&x->ch); al_dc_init(&x->dc);
    al_rng_seed(&x->rng, 0x5091ec7u); al_wander_init(&x->wnd, &x->rng, 8);
    x->hpx = x->hpy = 0.0; x->msm = 0.0;
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_squelch_tilde_setup(void)
{
    alien_squelch_class = class_new(gensym("alien_squelch~"),
        (t_newmethod)alien_squelch_new, 0, sizeof(t_alien_squelch),
        CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_squelch_class, t_alien_squelch, x_f);
    class_addmethod(alien_squelch_class, (t_method)alien_squelch_dsp,
                    gensym("dsp"), A_CANT, 0);
}
