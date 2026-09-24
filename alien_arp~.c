/* alien_arp~.c  --  an ARPEGGIATOR / generative melodic voice
 *
 * A single rich mono synth voice that WALKS a note list for melodic interest.
 * Give it a list of MIDI notes (the pool / scale) and clock it with bangs --
 * usually a [metro] -- and each bang advances a step index and plays the NEXT
 * note, retriggering a per-note pluck envelope. It's a mono lead, poly-by-time.
 *
 * The PATTERN through the pool is chosen by the macro's zone:
 *   0-20   up        walk the list low->high, wrap
 *   20-40  down      walk high->low, wrap
 *   40-60  up-down   bounce (ping-pong) through the list
 *   60-80  random    pick any note in the pool
 *   80-100 wander    a Markov-ish walk: mostly stepwise, occasional leaps,
 *                    with octave jumps up top -- a generative melody
 *
 * The macro ALSO shapes timbre (filter cutoff/res, detune, decay/gate) and a
 * slow wander drifts cutoff + detune for life. Voice: a small detuned supersaw
 * (al_cluster) -> resonant al_ladder -> per-note AD env -> DC-block -> softclip.
 *
 * Interface (copied from alien_chord~):
 *   inlet 0   bang    STEP / CLOCK -- advance one step, play the next note,
 *                     retrigger the envelope (a single bang plays one note)
 *   inlet 1   float   MACRO 0..100 -- pattern zone + timbre
 *   inlet 2   list    NOTES, MIDI note numbers, e.g.  50 55 58 62 65 (the pool)
 *   outlet 0  signal
 */

#include "m_pd.h"
#include "alien.h"

#define MAXV 32          /* max notes in the pool */

static t_class *alien_arp_tilde_class;

typedef struct _alien_arp {
    t_object x_obj;

    double   x_sr;
    int      x_np;                 /* notes in the pool                   */
    double   x_pool[MAXV];         /* pool frequencies (Hz)               */

    t_float  x_macro;              /* inlet 1 float                       */

    /* sequencer state */
    int      x_step;               /* current index into the pool         */
    int      x_dir;                /* +1 / -1 for up-down bounce          */
    int      x_trig;               /* set by bang, consumed in perform    */

    /* the currently sounding note */
    double   x_freq;               /* target freq (Hz)                    */
    double   x_gfreq;              /* glided freq for portamento          */

    /* voice */
    al_cluster osc;
    al_ladder  lad;
    al_dc      dc;
    al_glide   gld;
    al_wander  wnd;
    al_rng     rng;

    /* AD envelope */
    int      x_seg;                /* 0 idle, 1 attack, 2 decay           */
    double   x_env;
    double   x_dectime;            /* seconds, set on each step           */
} t_alien_arp;

/* ------------------------------------------------------------------ */

/* advance the step index per the pattern zone, return the new index */
static int arp_advance(t_alien_arp *x, int zone)
{
    int np = x->x_np; if (np < 1) np = 1;
    int s = x->x_step;

    if (np == 1) { x->x_step = 0; return 0; }

    switch (zone) {
        case 0:                                  /* up */
            s = s + 1; if (s >= np) s = 0;
            break;
        case 1:                                  /* down */
            s = s - 1; if (s < 0) s = np - 1;
            break;
        case 2:                                  /* up-down bounce */
            s = s + x->x_dir;
            if (s >= np - 1) { s = np - 1; x->x_dir = -1; }
            else if (s <= 0) { s = 0; x->x_dir = +1; }
            break;
        case 3:                                  /* random */
            s = (int)(al_rand(&x->rng) * np);
            if (s >= np) s = np - 1; if (s < 0) s = 0;
            break;
        default: {                               /* 4: wander / Markov walk */
            double u = al_rand(&x->rng);
            int move;
            if (u < 0.60)      move = (al_rand(&x->rng) < 0.5) ? -1 : +1;   /* stepwise */
            else if (u < 0.80) move = (al_rand(&x->rng) < 0.5) ? -2 : +2;   /* small leap */
            else if (u < 0.92) move = 0;                                    /* repeat */
            else               move = (int)((al_rand(&x->rng) - 0.5) * 2.0 * (np - 1)); /* big leap */
            s = s + move;
            /* reflect at the edges to stay in-bounds and musical */
            while (s < 0)      s = -s;
            while (s >= np)    s = 2 * (np - 1) - s;
            if (s < 0) s = 0; if (s >= np) s = np - 1;
            break;
        }
    }
    if (s < 0) s = 0; if (s >= np) s = np - 1;
    x->x_step = s;
    return s;
}

/* ------------------------------------------------------------------ */

static void arp_bang(t_alien_arp *x) { x->x_trig = 1; }

static void arp_macro(t_alien_arp *x, t_floatarg f) { x->x_macro = f; }

/* a list of MIDI notes -> the frequency pool */
static void arp_notes(t_alien_arp *x, t_symbol *s, int argc, t_atom *argv)
{
    int i, n = 0;
    (void)s;
    for (i = 0; i < argc && n < MAXV; i++) {
        double m = atom_getfloat(argv + i);
        if (!(m > 0.0)) continue;                 /* skip 0 / junk        */
        x->x_pool[n] = al_mtof(m);
        n++;
    }
    if (n > 0) {
        x->x_np = n;
        if (x->x_step >= n) x->x_step = 0;
    }
}

/* ------------------------------------------------------------------ */

static t_int *alien_arp_perform(t_int *w)
{
    t_alien_arp *x = (t_alien_arp *)(w[1]);
    t_sample *out = (t_sample *)(w[2]);
    int nsamp = (int)(w[3]);

    double sr = x->x_sr;

    /* macro -> character (per block) */
    double t = x->x_macro; t = t - 100.0 * floor(t * 0.01);
    if (!(t >= 0.0 && t <= 100.0)) t = 0.0;
    int zone = (int)(t / 20.0); if (zone > 4) zone = 4; if (zone < 0) zone = 0;
    t *= 0.01;

    /* slow wander for life */
    al_wander_tick(&x->wnd, &x->rng, 0.12 + 0.4 * t, sr);
    double wc = al_wander_bip(&x->wnd, 0);      /* cutoff drift  */
    double wd = al_wander_val(&x->wnd, 1);      /* detune drift  */

    double detune  = 0.004 + 0.03 * t + 0.012 * wd;
    double res     = 0.20 + 0.55 * t;
    double drive   = 0.15 + 0.40 * t;
    double glidems = 2.0 + 12.0 * (1.0 - t);    /* short slew, tighter up top */

    /* consume a trigger: advance step, set the note, fire the attack */
    if (x->x_trig) {
        x->x_trig = 0;
        int s = arp_advance(x, zone);
        double f = x->x_pool[s];

        /* octave jumps up top for melodic reach */
        if (zone == 4 && al_rand(&x->rng) < (0.10 + 0.25 * t)) {
            f *= (al_rand(&x->rng) < 0.5) ? 2.0 : 0.5;
        }
        if (!(f >= 8.0)) f = 8.0;
        if (f > sr * 0.45) f = sr * 0.45;
        x->x_freq = f;

        x->x_seg = 1;                                   /* attack */
        /* per-note pluck: short & bright at 0, longer & more sustained up top */
        x->x_dectime = 0.10 + 0.9 * t;
    }

    double atkc = 1.0 - exp(-1.0 / (sr * 0.004));       /* ~4ms attack   */
    double decc = exp(-1.0 / (sr * (x->x_dectime > 0.02 ? x->x_dectime : 0.02)));

    /* per-sample cutoff base; drifts with wander + follows env for a pluck */
    double cutbase = 220.0 * pow(2.0, 1.6 + 4.2 * t + 0.7 * wc);
    if (cutbase < 40.0) cutbase = 40.0;
    if (cutbase > sr * 0.45) cutbase = sr * 0.45;

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

        /* glide the pitch toward the target */
        x->x_gfreq = al_glide_tick(&x->gld, x->x_freq, glidems, sr);

        if (env > 0.0) {
            double f = x->x_gfreq;
            if (!(f >= 8.0)) f = 8.0;
            if (f > sr * 0.45) f = sr * 0.45;

            s = al_cluster_tick(&x->osc, f, detune, sr);

            /* filter opens with the envelope for a plucky voice */
            double cutoff = cutbase * (0.35 + 0.65 * env);
            if (cutoff < 40.0) cutoff = 40.0;
            if (cutoff > sr * 0.45) cutoff = sr * 0.45;

            s = al_ladder_tick(&x->lad, s, cutoff, res, sr);
            s *= env;
            s = al_drive(s, drive);
        }

        double y = al_dc_tick(&x->dc, s);
        if (!(y > -1e6 && y < 1e6)) y = 0.0;
        y = al_softclip(y * 2.2) * 0.9;
        if (y >  1.5) y =  1.5;
        if (y < -1.5) y = -1.5;
        *out++ = (t_sample)y;
    }
    return (w + 4);
}

static void alien_arp_dsp(t_alien_arp *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr; if (x->x_sr < 1000) x->x_sr = 44100;
    dsp_add(alien_arp_perform, 3, x, sp[0]->s_vec, (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_arp_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_arp *x = (t_alien_arp *)pd_new(alien_arp_tilde_class);
    int i;
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;

    /* default pool: a small pentatonic-ish scale so it never plays silence */
    static const double DEF[] = { 48, 50, 52, 55, 57, 60 };
    x->x_np = (int)(sizeof(DEF) / sizeof(DEF[0]));
    for (i = 0; i < x->x_np; i++) x->x_pool[i] = al_mtof(DEF[i]);

    x->x_macro  = 50.0;
    x->x_step   = 0;
    x->x_dir    = +1;
    x->x_trig   = 0;
    x->x_freq   = x->x_pool[0];
    x->x_gfreq  = x->x_pool[0];
    x->x_seg    = 0;
    x->x_env    = 0.0;
    x->x_dectime = 0.3;

    al_rng_seed(&x->rng, 0xA79ee1u);
    al_cluster_init(&x->osc, 5);
    al_ladder_init(&x->lad);
    al_dc_init(&x->dc);
    al_glide_init(&x->gld, x->x_pool[0]);
    al_wander_init(&x->wnd, &x->rng, 4);

    /* inlet 1: float macro; inlet 2: list of notes */
    floatinlet_new(&x->x_obj, &x->x_macro);
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_list, gensym("notes"));
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_arp_tilde_setup(void)
{
    alien_arp_tilde_class = class_new(gensym("alien_arp~"),
        (t_newmethod)alien_arp_new, 0, sizeof(t_alien_arp),
        CLASS_DEFAULT, A_GIMME, 0);
    class_addbang(alien_arp_tilde_class, arp_bang);
    class_addmethod(alien_arp_tilde_class, (t_method)arp_macro,
                    gensym("macro"), A_FLOAT, 0);
    class_addmethod(alien_arp_tilde_class, (t_method)arp_notes,
                    gensym("notes"), A_GIMME, 0);
    class_addmethod(alien_arp_tilde_class, (t_method)alien_arp_dsp,
                    gensym("dsp"), A_CANT, 0);
}
