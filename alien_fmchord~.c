/* alien_fmchord~.c  --  a paraphonic FM CHORD voice (glassy, bell-like, metallic)
 *
 * Give it a list of MIDI notes and bang it: every note becomes its own 2-op FM
 * voice (a carrier at the note pitch, a modulator at an integer-ish RATIO with
 * feedback), so the whole chord rings out glassy and bell-like -- classic DX
 * territory. A per-voice AD envelope also drives the FM INDEX, so notes strike
 * bright and mellow as they decay. The macro sweeps index + ratio + feedback +
 * length: mellow near-sine bells at 0 -> bright, clangorous, metallic chords at
 * 100. A slow wander drifts index/ratio so the chord keeps evolving.
 *
 * Shares the note-list voice interface (see alien_chord~):
 *
 *   inlet 0   bang            TRIGGER -- (re)voice the note list, fire envelope
 *   inlet 1   float           MACRO 0..100 -- bell(0) .. metal(100)
 *   inlet 2   list            NOTES, MIDI note numbers e.g.  50 55 58 62 65
 *
 *   outlet 0  signal
 *
 * build:
 *   (macOS: -bundle -undefined dynamic_lookup -o alien_fmchord~.pd_darwin ... )
 */

#include "m_pd.h"
#include "alien.h"

#define MAXV 16          /* max simultaneous notes */

static t_class *alien_fmchord_tilde_class;

typedef struct _alien_fmchord {
    t_object x_obj;

    double   x_sr;
    int      x_nv;                 /* active voices                       */
    double   x_freq[MAXV];         /* voice frequencies (Hz)              */
    double   x_ratk[MAXV];         /* per-voice ratio kick (from interval)*/

    al_fm2   x_fm[MAXV];           /* one 2-op FM voice per note          */

    t_float  x_macro;              /* inlet 1 float                       */

    /* AD envelope (shared, drives amp + FM index) */
    int      x_seg;                /* 0 idle, 1 attack, 2 decay           */
    double   x_env;
    int      x_trig;               /* set by bang, consumed in perform    */

    al_dc     dc;
    al_wander wnd;
    al_rng    rng;
} t_alien_fmchord;

/* ------------------------------------------------------------------ */

static void fmchord_bang(t_alien_fmchord *x) { x->x_trig = 1; }

static void fmchord_macro(t_alien_fmchord *x, t_floatarg f) { x->x_macro = f; }

/* a list of MIDI notes -> voice frequencies (+ per-voice interval flavour) */
static void fmchord_notes(t_alien_fmchord *x, t_symbol *s, int argc, t_atom *argv)
{
    int i, n = 0;
    double root = -1.0;
    (void)s;
    for (i = 0; i < argc && n < MAXV; i++) {
        double m = atom_getfloat(argv + i);
        if (!(m > 0.0)) continue;                 /* skip 0 / junk        */
        if (root < 0.0) root = m;
        x->x_freq[n] = al_mtof(m);
        /* let the interval above the root nudge each voice's ratio a touch */
        {
            double semi = m - root;               /* 0 for root note      */
            double kk = semi - 12.0 * floor(semi / 12.0);   /* 0..12       */
            x->x_ratk[n] = 0.02 * (kk - 6.0);     /* ~ -0.12 .. +0.12     */
        }
        n++;
    }
    if (n > 0) x->x_nv = n;
}

/* ------------------------------------------------------------------ */

static t_int *alien_fmchord_perform(t_int *w)
{
    t_alien_fmchord *x = (t_alien_fmchord *)(w[1]);
    t_sample *out = (t_sample *)(w[2]);
    int nsamp = (int)(w[3]);

    al_denorm_off();

    double sr = x->x_sr;
    int nv = x->x_nv, v;

    /* macro -> character (per block) */
    double t = x->x_macro; t = t - 100.0 * floor(t * 0.01);
    if (!(t >= 0.0 && t <= 100.0)) t = 0.0;
    t *= 0.01;

    /* slow wander for life */
    al_wander_tick(&x->wnd, &x->rng, 0.10 + 0.35 * t, sr);
    double wi = al_wander_bip(&x->wnd, 0);      /* index drift  */
    double wr = al_wander_bip(&x->wnd, 1);      /* ratio drift  */

    /* base FM parameters from the macro (BOUNDED for -ffast-math safety) */
    double ratio_base = 1.0 + floor(0.5 + t * 3.0) + 0.4 * t;  /* ~1..4.4 */
    ratio_base += 0.15 * wr;
    if (ratio_base < 0.5) ratio_base = 0.5;
    if (ratio_base > 7.0) ratio_base = 7.0;

    double index_max = 0.4 + 7.5 * t + 0.6 * wi;   /* attack index peak */
    if (index_max < 0.0)  index_max = 0.0;
    if (index_max > 9.0)  index_max = 9.0;         /* HARD cap */
    double index_sus = 0.15 + 1.2 * t;             /* sustained index  */
    if (index_sus > index_max) index_sus = index_max;

    double fb = 0.05 + 0.55 * t;                   /* modulator feedback */
    if (fb < 0.0)  fb = 0.0;
    if (fb > 0.85) fb = 0.85;                       /* < 1 : can't diverge */

    double dectime = 0.4 + 3.6 * t;                /* longer + brighter at 100 */
    double atkc = 1.0 - exp(-1.0 / (sr * 0.004));  /* ~4ms attack */
    double decc = exp(-1.0 / (sr * dectime));

    double norm = 0.7 / sqrt((double)(nv > 0 ? nv : 1));  /* incoherent-sum scale */

    /* consume a trigger: reset the envelope (phases keep running -> no clicks) */
    if (x->x_trig) {
        x->x_trig = 0;
        x->x_seg = 1;
    }

    while (nsamp--) {
        double s = 0.0, env = x->x_env;

        /* envelope */
        if (x->x_seg == 1) {
            env += atkc * (1.04 - env);
            if (env >= 1.0) { env = 1.0; x->x_seg = 2; }
        } else if (x->x_seg == 2) {
            env *= decc;
            if (env < 1e-6) { env = 0.0; x->x_seg = 0; }
        }
        x->x_env = env;

        if (env > 0.0) {
            /* index tracks the envelope: bright strike, mellow tail (FM) */
            double index = index_sus + (index_max - index_sus) * env;
            if (index > 9.0) index = 9.0;          /* re-clamp for safety */

            for (v = 0; v < nv; v++) {
                double f = x->x_freq[v];
                if (f < 8.0) f = 8.0;
                if (f > 0.45 * sr) f = 0.45 * sr;
                double ratio = ratio_base + x->x_ratk[v];
                if (ratio < 0.25) ratio = 0.25;
                if (ratio > 8.0)  ratio = 8.0;
                s += al_fm2_tick(&x->x_fm[v], f, ratio, index, fb, sr);
            }
            s *= norm * env;
        }

        s = al_dc_tick(&x->dc, s);
        if (!(s > -1e6 && s < 1e6)) s = 0.0;
        s = al_softclip(s);
        if (s >  1.5) s =  1.5;
        if (s < -1.5) s = -1.5;
        *out++ = (t_sample)(s * 0.9);
    }
    return (w + 4);
}

static void alien_fmchord_dsp(t_alien_fmchord *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr; if (x->x_sr < 1000) x->x_sr = 44100;
    dsp_add(alien_fmchord_perform, 3, x, sp[0]->s_vec, (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_fmchord_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_fmchord *x = (t_alien_fmchord *)pd_new(alien_fmchord_tilde_class);
    int i;
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    x->x_nv = 1; x->x_freq[0] = al_mtof(50); x->x_ratk[0] = 0.0;
    for (i = 0; i < MAXV; i++) {
        al_fm2_init(&x->x_fm[i]);
        if (i > 0) { x->x_freq[i] = al_mtof(50 + i); x->x_ratk[i] = 0.0; }
    }
    x->x_macro = 50.0;
    x->x_seg = 0; x->x_env = 0.0; x->x_trig = 0;
    al_dc_init(&x->dc);
    al_rng_seed(&x->rng, 0xf3c00du); al_wander_init(&x->wnd, &x->rng, 4);

    /* inlet 1: float macro; inlet 2: list of notes */
    floatinlet_new(&x->x_obj, &x->x_macro);
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_list, gensym("notes"));
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_fmchord_tilde_setup(void)
{
    alien_fmchord_tilde_class = class_new(gensym("alien_fmchord~"),
        (t_newmethod)alien_fmchord_new, 0, sizeof(t_alien_fmchord),
        CLASS_DEFAULT, A_GIMME, 0);
    class_addbang(alien_fmchord_tilde_class, fmchord_bang);
    class_addmethod(alien_fmchord_tilde_class, (t_method)fmchord_macro,
                    gensym("macro"), A_FLOAT, 0);
    class_addmethod(alien_fmchord_tilde_class, (t_method)fmchord_notes,
                    gensym("notes"), A_GIMME, 0);
    class_addmethod(alien_fmchord_tilde_class, (t_method)alien_fmchord_dsp,
                    gensym("dsp"), A_CANT, 0);
}
