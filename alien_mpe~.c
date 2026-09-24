/* alien_mpe~.c  --  "MPE-in-a-box": per-voice expression on a held chord
 *
 * Bang plays a chord where EACH note is an independent voice with its OWN
 * modulation -- a private vibrato LFO, a private slow pitch/timbre drift, a
 * private filter-cutoff LFO, and a private amplitude tremolo -- all running at
 * different (irrationally-spaced) rates so the voices never phase-lock. A held
 * chord therefore constantly BREATHES: each note moves independently and
 * organically, the expressive-polyphony / MPE quality with no controller.
 *
 * The private pitch drift stays within a few cents so the chord stays in tune;
 * the macro scales BOTH the depth AND the rate of all this motion, so at 0 you
 * get a still, clean chord and rising toward 100 the chord becomes a living,
 * shimmering, organic organism.
 *
 * Shared note-list interface (see alien_chord~):
 *   inlet 0   bang    TRIGGER -- fire the per-voice AR envelopes (slow attack,
 *                     long sustain/release so the chord holds and breathes)
 *   inlet 1   float   MACRO 0..100 -- EXPRESSION DEPTH + rate
 *   inlet 2   list    NOTES, MIDI note numbers e.g.  50 55 58 62 65
 *   outlet 0  signal
 */

#include "m_pd.h"
#include "alien.h"

#define MAXV 10          /* max simultaneous voices (MPE capacity)          */

static t_class *alien_mpe_tilde_class;

typedef struct _alien_mpe {
    t_object x_obj;

    double   x_sr;
    int      x_nv;                  /* active voices                        */
    double   x_freq[MAXV];          /* base voice frequencies (Hz)          */

    /* per-voice oscillator (two detuned saws) */
    double   x_ph[MAXV], x_ph2[MAXV];

    /* per-voice private modulators (all desynchronised) */
    double   x_vibp[MAXV];          /* vibrato LFO phase                    */
    double   x_vibr[MAXV];          /* vibrato rate (Hz)                    */
    double   x_cutp[MAXV];          /* filter-cutoff LFO phase              */
    double   x_cutr[MAXV];          /* filter-cutoff LFO rate (Hz)          */
    double   x_trep[MAXV];          /* tremolo LFO phase                    */
    double   x_trer[MAXV];          /* tremolo LFO rate (Hz)                */
    al_wander x_wnd[MAXV];          /* private slow pitch/timbre random walk*/
    double   x_wrate[MAXV];         /* private wander rate                  */

    /* per-voice filter + envelope */
    al_svf   x_flt[MAXV];
    int      x_seg[MAXV];           /* 0 idle, 1 attack, 2 sustain(3=rel)   */
    double   x_env[MAXV];

    t_float  x_macro;               /* inlet 1 float = EXPRESSION DEPTH     */
    int      x_trig;                /* set by bang, consumed in perform     */

    al_dc    dc;
    al_rng   rng;
} t_alien_mpe;

/* ------------------------------------------------------------------ */

static void mpe_bang(t_alien_mpe *x) { x->x_trig = 1; }

static void mpe_macro(t_alien_mpe *x, t_floatarg f) { x->x_macro = f; }

/* seed the private modulator rates for a voice, spaced by an irrational ratio
 * (golden-ratio walk) so no two voices ever share a rate -> never lock. */
static void mpe_seed_rates(t_alien_mpe *x, int v)
{
    /* irrational spread across voices + a little randomness per voice */
    double phi = 1.6180339887498949;
    double kv  = (double)v * phi;
    double j1 = al_rand(&x->rng), j2 = al_rand(&x->rng), j3 = al_rand(&x->rng);

    x->x_vibr[v]  = 3.7  + 2.3 * (kv - floor(kv)) + 0.8 * j1;   /* ~3.7..6.8 Hz */
    x->x_cutr[v]  = 0.23 + 0.55 * (kv*phi - floor(kv*phi)) + 0.15 * j2; /* slow */
    x->x_trer[v]  = 0.9  + 1.7 * (kv*phi*phi - floor(kv*phi*phi)) + 0.4 * j3;
    x->x_wrate[v] = 0.11 + 0.09 * (kv*0.5 - floor(kv*0.5));     /* very slow    */

    /* desynchronise starting phases too */
    x->x_vibp[v] = al_rand(&x->rng);
    x->x_cutp[v] = al_rand(&x->rng);
    x->x_trep[v] = al_rand(&x->rng);
}

/* a list of MIDI notes -> voice frequencies */
static void mpe_notes(t_alien_mpe *x, t_symbol *s, int argc, t_atom *argv)
{
    int i, n = 0;
    (void)s;
    for (i = 0; i < argc && n < MAXV; i++) {
        double m = atom_getfloat(argv + i);
        double f;
        if (!(m > 0.0)) continue;                 /* skip 0 / junk         */
        f = al_mtof(m);
        if (!(f > 8.0)) f = 8.0;
        if (f > 0.45 * x->x_sr) f = 0.45 * x->x_sr;
        x->x_freq[n] = f;
        n++;
    }
    if (n > 0) x->x_nv = n;
}

/* ------------------------------------------------------------------ */

static t_int *alien_mpe_perform(t_int *w)
{
    t_alien_mpe *x = (t_alien_mpe *)(w[1]);
    t_sample *out = (t_sample *)(w[2]);
    int nsamp = (int)(w[3]);

    double sr = x->x_sr;
    int nv = x->x_nv, v;
    double isr = 1.0 / sr;

    al_denorm_off();

    /* macro -> expression depth (0..1) */
    double t = x->x_macro; t = t - 100.0 * floor(t * 0.01);
    if (!(t >= 0.0 && t <= 100.0)) t = 0.0;
    t *= 0.01;

    if (nv < 1) nv = 1;
    if (nv > MAXV) nv = MAXV;

    /* per-block motion amounts (all scale with the macro) */
    double vib_cents = 6.0 * t;                 /* vibrato depth, cents     */
    double drift_cents = 9.0 * t;               /* slow pitch drift, cents  */
    double cut_depth = t;                       /* filter LFO depth 0..1    */
    double trem_depth = 0.35 * t;               /* tremolo depth            */
    double ratemul = 0.25 + 0.75 * t;           /* modulators slow at low macro */
    double detune = 0.0015 + 0.01 * t;          /* voice-internal detune    */
    double res = 0.18 + 0.45 * t;
    double norm = 0.72 / sqrt((double)nv);      /* incoherent-sum scale     */

    /* AR envelope coefficients: slow attack, long release, held sustain */
    double atkc = 1.0 - exp(-1.0 / (sr * (0.12 + 0.25 * t)));   /* ~120-370ms */
    double susc = exp(-1.0 / (sr * (6.0 + 10.0 * t)));          /* very long fade */

    /* consume a trigger: reset phases + (re)seed rates + start attacks */
    if (x->x_trig) {
        x->x_trig = 0;
        for (v = 0; v < nv; v++) {
            x->x_ph[v] = al_rand(&x->rng);
            x->x_ph2[v] = al_rand(&x->rng);
            mpe_seed_rates(x, v);
            x->x_seg[v] = 1;
            x->x_env[v] = 0.0;
        }
    }

    while (nsamp--) {
        double mix = 0.0, y;

        for (v = 0; v < nv; v++) {
            double env = x->x_env[v];
            double f, semis, cents, fmod, d1, d2, s1, cutoff, trem, vs;

            /* --- per-voice AR envelope --- */
            if (x->x_seg[v] == 1) {
                env += atkc * (1.03 - env);
                if (env >= 1.0) { env = 1.0; x->x_seg[v] = 2; }
            } else if (x->x_seg[v] == 2) {
                env *= susc;                       /* long sustain/release   */
                if (env < 1e-6) { env = 0.0; x->x_seg[v] = 0; }
            }
            x->x_env[v] = env;
            if (env <= 0.0) continue;

            /* --- private slow pitch/timbre random walk --- */
            al_wander_tick(&x->x_wnd[v], &x->rng, x->x_wrate[v] * ratemul, sr);
            double wpitch = al_wander_bip(&x->x_wnd[v], 0);   /* -1..1        */
            double wtimbre = al_wander_val(&x->x_wnd[v], 1);  /*  0..1        */

            /* --- private vibrato LFO --- */
            x->x_vibp[v] += x->x_vibr[v] * ratemul * isr;
            if (x->x_vibp[v] >= 1.0) x->x_vibp[v] -= floor(x->x_vibp[v]);
            double vib = al_sin01(x->x_vibp[v]);

            /* total detune in cents (bounded so the chord stays in tune) */
            cents = vib_cents * vib + drift_cents * wpitch;
            if (cents > 20.0) cents = 20.0;         /* hard cent clamp        */
            if (cents < -20.0) cents = -20.0;
            semis = cents / 100.0;
            fmod = 1.0 + 0.0577623 * semis;         /* ~2^(semis/12) linearised */
            if (fmod < 0.95) fmod = 0.95;
            if (fmod > 1.05) fmod = 1.05;

            f = x->x_freq[v] * fmod;
            if (!(f > 8.0)) f = 8.0;
            if (f > 0.45 * sr) f = 0.45 * sr;

            /* --- two lightly detuned saws (the voice body) --- */
            d1 = f * isr;
            d2 = f * (1.0 + detune) * isr;
            x->x_ph[v] = al_wrap01(x->x_ph[v] + d1);
            x->x_ph2[v] = al_wrap01(x->x_ph2[v] + d2);
            s1 = 0.5 * (al_saw_bl(x->x_ph[v], d1) + al_saw_bl(x->x_ph2[v], d2));

            /* --- private filter-cutoff LFO (+ timbre drift) --- */
            x->x_cutp[v] += x->x_cutr[v] * ratemul * isr;
            if (x->x_cutp[v] >= 1.0) x->x_cutp[v] -= floor(x->x_cutp[v]);
            double clfo = 0.5 + 0.5 * al_sin01(x->x_cutp[v]);   /* 0..1        */
            /* base cutoff tracks the note; LFO + timbre drift open/close it  */
            double octaves = 1.6 + 2.4 * cut_depth * clfo + 1.2 * cut_depth * wtimbre;
            cutoff = f * pow(2.0, octaves);
            if (cutoff < 40.0) cutoff = 40.0;
            if (cutoff > sr * 0.45) cutoff = sr * 0.45;
            s1 = al_svf_lp(&x->x_flt[v], s1, cutoff, res, sr);

            /* --- private amplitude tremolo --- */
            x->x_trep[v] += x->x_trer[v] * ratemul * isr;
            if (x->x_trep[v] >= 1.0) x->x_trep[v] -= floor(x->x_trep[v]);
            trem = 1.0 - trem_depth * (0.5 + 0.5 * al_sin01(x->x_trep[v]));

            vs = s1 * env * trem;
            if (!(vs > -1e6 && vs < 1e6)) vs = 0.0;
            mix += vs;
        }

        mix *= norm;

        y = al_dc_tick(&x->dc, mix);
        if (!(y > -1e6 && y < 1e6)) y = 0.0;
        y = al_softclip(y);
        if (y > 1.5) y = 1.5; else if (y < -1.5) y = -1.5;
        *out++ = (t_sample)y;
    }
    return (w + 4);
}

static void alien_mpe_dsp(t_alien_mpe *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr; if (x->x_sr < 1000) x->x_sr = 44100;
    dsp_add(alien_mpe_perform, 3, x, sp[0]->s_vec, (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_mpe_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_mpe *x = (t_alien_mpe *)pd_new(alien_mpe_tilde_class);
    int i;
    (void)s; (void)argc; (void)argv;

    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    al_rng_seed(&x->rng, 0xA11E7u);

    x->x_nv = 1;
    x->x_freq[0] = al_mtof(50);
    for (i = 0; i < MAXV; i++) {
        x->x_ph[i] = 0.0; x->x_ph2[i] = 0.37;
        x->x_seg[i] = 0; x->x_env[i] = 0.0;
        al_svf_init(&x->x_flt[i]);
        al_wander_init(&x->x_wnd[i], &x->rng, 4);
        mpe_seed_rates(x, i);
    }
    x->x_macro = 50.0;
    x->x_trig = 0;
    al_dc_init(&x->dc);

    /* inlet 1: float macro (expression depth); inlet 2: list of notes */
    floatinlet_new(&x->x_obj, &x->x_macro);
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_list, gensym("notes"));
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_mpe_tilde_setup(void)
{
    alien_mpe_tilde_class = class_new(gensym("alien_mpe~"),
        (t_newmethod)alien_mpe_new, 0, sizeof(t_alien_mpe),
        CLASS_DEFAULT, A_GIMME, 0);
    class_addbang(alien_mpe_tilde_class, mpe_bang);
    class_addmethod(alien_mpe_tilde_class, (t_method)mpe_macro,
                    gensym("macro"), A_FLOAT, 0);
    class_addmethod(alien_mpe_tilde_class, (t_method)mpe_notes,
                    gensym("notes"), A_GIMME, 0);
    class_addmethod(alien_mpe_tilde_class, (t_method)alien_mpe_dsp,
                    gensym("dsp"), A_CANT, 0);
}
