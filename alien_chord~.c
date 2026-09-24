/* alien_chord~.c  --  a monophonic object that plays POLYPHONY (paraphonic)
 *
 * One signal path, many pitches: give it a list of MIDI notes and bang it, and
 * it voices the whole chord through a shared filter + envelope -- the classic
 * "mono synth, many notes" string-machine / trance-chord architecture, in a
 * single object. Each note is two band-limited saws detuned against each other;
 * a shared resonant ladder and an AD envelope shape the whole stack; a slow
 * wander brain keeps it alive.
 *
 * This is the reference for the note-list voice interface used by the
 * alien_*chord~ / alien_pluck~ / alien_arp~ family:
 *
 *   inlet 0   bang            TRIGGER -- (re)voice the current note list and
 *                             fire the envelope (a struck/plucked chord that
 *                             swells and decays; no note-off needed)
 *   inlet 1   float           MACRO 0..100 -- timbre: mellow/short/dark at 0,
 *                             bright/long/detuned/lush at 100
 *   inlet 2   list            NOTES, MIDI note numbers e.g.  50 55 58 62 65
 *                             (stored; played on the next bang; mtof internally)
 *
 *   outlet 0  signal
 *
 * build:
 *   (macOS: -bundle -undefined dynamic_lookup -o alien_chord~.pd_darwin ... )
 */

#include "m_pd.h"
#include "alien.h"

#define MAXV 16          /* max simultaneous notes */

static t_class *alien_chord_tilde_class;

typedef struct _alien_chord {
    t_object x_obj;

    double   x_sr;
    int      x_nv;                 /* active voices                       */
    double   x_freq[MAXV];         /* voice frequencies (Hz)              */
    double   x_ph[MAXV], x_ph2[MAXV];

    t_float  x_macro;              /* inlet 1 float                       */

    /* AD envelope */
    int      x_seg;                /* 0 idle, 1 attack, 2 decay           */
    double   x_env;
    int      x_trig;               /* set by bang, consumed in perform    */

    al_ladder lad;
    al_dc     dc;
    al_wander wnd;
    al_rng    rng;
} t_alien_chord;

/* ------------------------------------------------------------------ */

static void chord_bang(t_alien_chord *x) { x->x_trig = 1; }

static void chord_macro(t_alien_chord *x, t_floatarg f) { x->x_macro = f; }

/* a list of MIDI notes -> voice frequencies */
static void chord_notes(t_alien_chord *x, t_symbol *s, int argc, t_atom *argv)
{
    int i, n = 0;
    (void)s;
    for (i = 0; i < argc && n < MAXV; i++) {
        double m = atom_getfloat(argv + i);
        if (!(m > 0.0)) continue;                 /* skip 0 / junk        */
        x->x_freq[n] = al_mtof(m);
        n++;
    }
    if (n > 0) x->x_nv = n;
}

/* ------------------------------------------------------------------ */

static t_int *alien_chord_perform(t_int *w)
{
    t_alien_chord *x = (t_alien_chord *)(w[1]);
    t_sample *out = (t_sample *)(w[2]);
    int nsamp = (int)(w[3]);

    double sr = x->x_sr;
    int nv = x->x_nv, v;

    /* macro -> character (per block) */
    double t = x->x_macro; t = t - 100.0 * floor(t * 0.01);
    if (!(t >= 0.0 && t <= 100.0)) t = 0.0;
    t *= 0.01;

    /* slow wander for life */
    al_wander_tick(&x->wnd, &x->rng, 0.15 + 0.4 * t, sr);
    double wc = al_wander_bip(&x->wnd, 0);      /* cutoff drift  */
    double wd = al_wander_val(&x->wnd, 1);      /* detune drift  */

    double detune = 0.001 + 0.02 * t + 0.01 * wd;
    double cutoff = 220.0 * pow(2.0, 1.4 + 4.3 * t + 0.7 * wc);
    double res    = 0.12 + 0.55 * t;
    double dectime = 0.2 + 3.2 * t;
    double drive  = 0.10 + 0.45 * t;
    double atkc   = 1.0 - exp(-1.0 / (sr * 0.006));       /* ~6ms attack   */
    double decc   = exp(-1.0 / (sr * dectime));
    double norm   = 0.5 / sqrt((double)(nv > 0 ? nv : 1));  /* incoherent-sum scale */

    if (cutoff < 40.0) cutoff = 40.0;
    if (cutoff > sr * 0.45) cutoff = sr * 0.45;

    /* consume a trigger: reset phases + start the attack */
    if (x->x_trig) {
        x->x_trig = 0;
        x->x_seg = 1;
        for (v = 0; v < nv; v++) { x->x_ph[v] = 0.0; x->x_ph2[v] = 0.37; }
    }

    while (nsamp--) {
        double s = 0.0, env = x->x_env, y;

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
            for (v = 0; v < nv; v++) {
                double f = x->x_freq[v];
                double d1 = f / sr, d2 = f * (1.0 + detune) / sr;
                x->x_ph[v]  = al_wrap01(x->x_ph[v]  + d1);
                x->x_ph2[v] = al_wrap01(x->x_ph2[v] + d2);
                s += al_saw_bl(x->x_ph[v], d1) + al_saw_bl(x->x_ph2[v], d2);
            }
            s *= norm;
            s = al_ladder_tick(&x->lad, s, cutoff, res, sr);
            s *= env;
            s = al_drive(s, drive);
        }

        y = al_dc_tick(&x->dc, s);
        if (!(y > -1e6 && y < 1e6)) y = 0.0;
        *out++ = (t_sample)(al_softclip(y) * 0.85);
    }
    return (w + 4);
}

static void alien_chord_dsp(t_alien_chord *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr; if (x->x_sr < 1000) x->x_sr = 44100;
    dsp_add(alien_chord_perform, 3, x, sp[0]->s_vec, (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_chord_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_chord *x = (t_alien_chord *)pd_new(alien_chord_tilde_class);
    int i;
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    x->x_nv = 1; x->x_freq[0] = al_mtof(50);
    for (i = 0; i < MAXV; i++) { x->x_ph[i] = 0.0; x->x_ph2[i] = 0.37; }
    x->x_macro = 50.0;
    x->x_seg = 0; x->x_env = 0.0; x->x_trig = 0;
    al_ladder_init(&x->lad); al_dc_init(&x->dc);
    al_rng_seed(&x->rng, 0x51ee7u); al_wander_init(&x->wnd, &x->rng, 4);

    /* inlet 1: float macro; inlet 2: list of notes */
    floatinlet_new(&x->x_obj, &x->x_macro);
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_list, gensym("notes"));
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_chord_tilde_setup(void)
{
    alien_chord_tilde_class = class_new(gensym("alien_chord~"),
        (t_newmethod)alien_chord_new, 0, sizeof(t_alien_chord),
        CLASS_DEFAULT, A_GIMME, 0);
    class_addbang(alien_chord_tilde_class, chord_bang);
    class_addmethod(alien_chord_tilde_class, (t_method)chord_macro,
                    gensym("macro"), A_FLOAT, 0);
    class_addmethod(alien_chord_tilde_class, (t_method)chord_notes,
                    gensym("notes"), A_GIMME, 0);
    class_addmethod(alien_chord_tilde_class, (t_method)alien_chord_dsp,
                    gensym("dsp"), A_CANT, 0);
}
