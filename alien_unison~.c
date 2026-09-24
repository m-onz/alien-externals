/* alien_unison~.c  --  a SUPERSAW / UNISON SWARM chord voice (paraphonic)
 *
 * Bang plays a chord where every note is a fat detuned supersaw cluster: many
 * band-limited saws spread by a beat-rate detune, given a phase spread and a
 * slow independent per-voice pitch drift so the wall shimmers and never sits
 * still.  A shared resonant ladder + AR envelope shape the whole stack -- huge,
 * lush, trance / hands-up detuned pads.
 *
 * note-list voice interface (copied from alien_chord~):
 *
 *   inlet 0   bang            TRIGGER -- (re)voice the current note list and
 *                             fire a slow-attack / long-release AR envelope so
 *                             the pad swells in and holds, then sustains until
 *                             the next bang
 *   inlet 1   float           MACRO 0..100 -- 0 = tight focused chord,
 *                             100 = massive wide shimmering detuned wall
 *                             (detune amount + voice spread + cutoff/res + drive)
 *   inlet 2   list            NOTES, MIDI note numbers e.g.  40 47 52 55 59 64
 *                             (stored; played on the next bang; mtof internally)
 *
 *   outlet 0  signal
 *
 * build:
 *   (macOS: -bundle -undefined dynamic_lookup -o alien_unison~.pd_darwin ... )
 */

#include "m_pd.h"
#include "alien.h"

#define MAXV 8           /* max simultaneous chord notes */

static t_class *alien_unison_tilde_class;

typedef struct _alien_unison {
    t_object x_obj;

    double    x_sr;
    int       x_nv;                 /* active voices                        */
    double    x_freq[MAXV];         /* voice frequencies (Hz)               */
    al_cluster x_cl[MAXV];          /* one supersaw cluster per chord note   */
    double    x_drift[MAXV];        /* slow per-voice pitch drift (cents-ish)*/
    double    x_dtg[MAXV];          /* drift target                          */

    t_float   x_macro;              /* inlet 1 float                        */

    /* AR envelope (slow attack, sustain, long release on re-trigger tail) */
    int       x_seg;                /* 0 idle, 1 attack, 2 sustain           */
    double    x_env;
    int       x_trig;               /* set by bang, consumed in perform      */

    al_ladder lad;
    al_dc     dc;
    al_wander wnd;
    al_rng    rng;
} t_alien_unison;

/* ------------------------------------------------------------------ */

static void unison_bang(t_alien_unison *x) { x->x_trig = 1; }

static void unison_macro(t_alien_unison *x, t_floatarg f) { x->x_macro = f; }

/* a list of MIDI notes -> voice frequencies */
static void unison_notes(t_alien_unison *x, t_symbol *s, int argc, t_atom *argv)
{
    int i, n = 0;
    (void)s;
    for (i = 0; i < argc && n < MAXV; i++) {
        double m = atom_getfloat(argv + i);
        if (!(m > 0.0)) continue;                 /* skip 0 / junk         */
        x->x_freq[n] = al_mtof(m);
        n++;
    }
    if (n > 0) x->x_nv = n;
}

/* ------------------------------------------------------------------ */

static t_int *alien_unison_perform(t_int *w)
{
    t_alien_unison *x = (t_alien_unison *)(w[1]);
    t_sample *out = (t_sample *)(w[2]);
    int nsamp = (int)(w[3]);

    double sr = x->x_sr;
    int nv = x->x_nv, v;
    if (nv < 0) nv = 0; if (nv > MAXV) nv = MAXV;

    /* macro -> character (per block) */
    double t = x->x_macro; t = t - 100.0 * floor(t * 0.01);
    if (!(t >= 0.0 && t <= 100.0)) t = 0.0;
    t *= 0.01;

    /* slow wander for life */
    al_wander_tick(&x->wnd, &x->rng, 0.10 + 0.35 * t, sr);
    double wc = al_wander_bip(&x->wnd, 0);      /* cutoff drift  */
    double wd = al_wander_val(&x->wnd, 1);      /* detune drift  */

    /* unison detune (fraction) grows with macro; wander animates it.
     * 0 -> ~0.0015 (tight), 100 -> ~0.05 (huge beat-rate spread)         */
    double detune = 0.0015 + 0.040 * t + 0.008 * t * wd;
    double cutoff = 180.0 * pow(2.0, 1.5 + 3.4 * t + 0.6 * wc);
    double res    = 0.10 + 0.45 * t;
    double drive  = 0.06 + 0.28 * t;

    /* per-voice slow pitch-drift depth (fraction of freq) sweeps with macro */
    double driftdepth = 0.0006 + 0.010 * t;
    double drifta = (0.4 + 1.2 * t) / sr;       /* drift smoothing rate    */

    double atkc  = 1.0 - exp(-1.0 / (sr * 0.30));         /* ~300ms attack */
    double norm  = 2.9 / sqrt((double)(nv > 0 ? nv : 1));

    if (cutoff < 40.0)        cutoff = 40.0;
    if (cutoff > sr * 0.45)   cutoff = sr * 0.45;

    /* consume a trigger: pick new drift targets + start the attack */
    if (x->x_trig) {
        x->x_trig = 0;
        x->x_seg = 1;
        for (v = 0; v < MAXV; v++)
            x->x_dtg[v] = al_randb(&x->rng);
    }

    while (nsamp--) {
        double s = 0.0, env = x->x_env, y;

        /* AR envelope: slow attack, then hold (sustain) until next bang   */
        if (x->x_seg == 1) {
            env += atkc * (1.04 - env);
            if (env >= 1.0) { env = 1.0; x->x_seg = 2; }
        }
        x->x_env = env;

        if (env > 0.0) {
            for (v = 0; v < nv; v++) {
                double f = x->x_freq[v];

                /* slow independent per-voice pitch drift (shimmer)        */
                x->x_drift[v] += drifta * (x->x_dtg[v] - x->x_drift[v]);
                double fv = f * (1.0 + driftdepth * x->x_drift[v]);
                if (fv < 8.0)        fv = 8.0;
                if (fv > sr * 0.45)  fv = sr * 0.45;

                /* the supersaw cluster: many detuned band-limited saws    */
                s += al_cluster_tick(&x->x_cl[v], fv, detune, sr);
            }
            s *= norm;
            s = al_ladder_tick(&x->lad, s, cutoff, res, sr);
            s *= env;
            s = al_drive(s, drive);
        }

        y = al_dc_tick(&x->dc, s);
        if (!(y > -1e6 && y < 1e6)) y = 0.0;
        y = al_softclip(y) * 0.82;
        if (y >  1.5) y =  1.5;
        if (y < -1.5) y = -1.5;
        *out++ = (t_sample)y;
    }
    return (w + 4);
}

static void alien_unison_dsp(t_alien_unison *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr; if (x->x_sr < 1000) x->x_sr = 44100;
    dsp_add(alien_unison_perform, 3, x, sp[0]->s_vec, (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_unison_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_unison *x = (t_alien_unison *)pd_new(alien_unison_tilde_class);
    int i;
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    x->x_nv = 1; x->x_freq[0] = al_mtof(50);
    for (i = 0; i < MAXV; i++) {
        al_cluster_init(&x->x_cl[i], AL_CLUSTER_MAX);   /* 7 saws / note   */
        x->x_drift[i] = 0.0; x->x_dtg[i] = 0.0;
    }
    x->x_macro = 50.0;
    x->x_seg = 0; x->x_env = 0.0; x->x_trig = 0;
    al_ladder_init(&x->lad); al_dc_init(&x->dc);
    al_rng_seed(&x->rng, 0x0175012u ^ 0x9e3779b9u);
    al_wander_init(&x->wnd, &x->rng, 4);

    /* inlet 1: float macro; inlet 2: list of notes */
    floatinlet_new(&x->x_obj, &x->x_macro);
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_list, gensym("notes"));
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_unison_tilde_setup(void)
{
    alien_unison_tilde_class = class_new(gensym("alien_unison~"),
        (t_newmethod)alien_unison_new, 0, sizeof(t_alien_unison),
        CLASS_DEFAULT, A_GIMME, 0);
    class_addbang(alien_unison_tilde_class, unison_bang);
    class_addmethod(alien_unison_tilde_class, (t_method)unison_macro,
                    gensym("macro"), A_FLOAT, 0);
    class_addmethod(alien_unison_tilde_class, (t_method)unison_notes,
                    gensym("notes"), A_GIMME, 0);
    class_addmethod(alien_unison_tilde_class, (t_method)alien_unison_dsp,
                    gensym("dsp"), A_CANT, 0);
}
