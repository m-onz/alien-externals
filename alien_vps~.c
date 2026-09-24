/* alien_vps~ -- a VECTOR PHASE SHAPING chord (paraphonic, note-list voice)
 *
 * A glassy, vocal, resonant FORMANT-SWEEPING chord machine. Each chord note is
 * a Vector Phase Shaping (VPS) oscillator (Kleimola / Lazzarini / Valimaki /
 * Timoney, "Phaseshaping Oscillator Algorithms for Musical Sound Synthesis",
 * 2011 -- the oscillator behind FAW's Circle^2 synth). Give it a list of MIDI
 * notes and bang it, and the whole chord swells through a shared envelope,
 * ladder and drive: a struck/plucked resonant chord with a moving formant.
 *
 * VECTOR PHASE SHAPING -- per note a phasor x runs in [0,1) at the note freq,
 * then x is pushed through a piecewise-linear phase-shaping function with ONE
 * control point (d, v):
 *
 *     if (x < d)  xs = (v / d) * x;
 *     else        xs = v + ((1 - v)/(1 - d)) * (x - d);
 *     out = cos(2*PI * xs);
 *
 * With v > 1 (roughly integer) the shaper packs several cosine cycles into part
 * of the fundamental period -- a RESONANT FORMANT peak at ~v*f0. d moves the
 * formant position across the cycle; v is its harmonic number. Sweeping d and v
 * glides the formant exactly like a resonant filter opening or CZ phase
 * distortion -- but it is pure oscillator maths, no filter. A second slightly
 * detuned VPS layer per note adds width.
 *
 * THE MACRO (inlet 1, 0..100): mellow near-sine at 0 -> bright resonant
 * vocal/formant sweep in the middle -> buzzy metallic high-v at 100. It drives
 * v (~1..8), the d sweep, the detune, a shared ladder and drive.
 *
 * THE BRAIN (al_wander): a slow autonomous drift on d and v per voice so a held
 * chord shimmers and never repeats.
 *
 *   inlet 0   bang            TRIGGER -- (re)voice the note list, fire the AR
 *                             envelope (slow attack, hold, release)
 *   inlet 1   float           MACRO 0..100 (formant character)
 *   inlet 2   list            NOTES, MIDI note numbers e.g. 40 47 52 55 59 64
 *   outlet 0  signal
 *
 * SAFETY (build uses -ffast-math -> magnitude clamps): d clamped (0.01,0.99),
 * v clamped [1,8]; freq clamped [8, 0.45*sr]; sum normalised ~1/sqrt(nv) so a
 * big chord never rails; finite guard `if(!(s>-1e6&&s<1e6)) s=0;` then bounded
 * ~[-1.5,1.5]. Handles an empty list, many notes and re-bangs.
 */

#include "m_pd.h"
#include "alien.h"

#define MAXV 10          /* max simultaneous notes */

static t_class *alien_vps_tilde_class;

typedef struct _alien_vps {
    t_object x_obj;

    double   x_sr;
    int      x_nv;                 /* active voices                       */
    double   x_freq[MAXV];         /* voice frequencies (Hz)              */
    double   x_ph[MAXV], x_ph2[MAXV];

    t_float  x_macro;              /* inlet 1 float                       */

    /* AR envelope: slow attack, hold, release */
    int      x_seg;                /* 0 idle, 1 attack, 2 hold, 3 release */
    double   x_env;
    double   x_hold;               /* samples remaining in hold           */
    int      x_trig;               /* set by bang, consumed in perform    */

    al_ladder lad;
    al_dc     dc;
    al_wander wnd;
    al_rng    rng;
} t_alien_vps;

/* ------------------------------------------------------------------ */

static void vps_bang(t_alien_vps *x) { x->x_trig = 1; }

static void vps_macro(t_alien_vps *x, t_floatarg f) { x->x_macro = f; }

/* a list of MIDI notes -> voice frequencies */
static void vps_notes(t_alien_vps *x, t_symbol *s, int argc, t_atom *argv)
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

/* ------------------------------------------------------------------ *
 * Vector Phase Shaping: one control point (d, v). Warp phase x -> xs,
 * then read a cosine. d in (0.01,0.99); v roughly integer for a clean
 * resonant formant at ~v*f0. Returns ~[-1,1].                          */
static inline double al_vps(double x, double d, double v)
{
    double xs;
    if (x < d) xs = (v / d) * x;
    else       xs = v + ((1.0 - v) / (1.0 - d)) * (x - d);
    return cos(AL_2PI * xs);
}

/* ------------------------------------------------------------------ */

static t_int *alien_vps_perform(t_int *w)
{
    t_alien_vps *x = (t_alien_vps *)(w[1]);
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
    double wd = al_wander_bip(&x->wnd, 0);      /* d drift       */
    double wv = al_wander_bip(&x->wnd, 1);      /* v drift       */
    double wc = al_wander_bip(&x->wnd, 2);      /* cutoff drift  */

    /* VPS controls swept by the macro. v: mellow ~1 -> buzzy ~7.  d: base
     * formant position, swept + slowly drifting so the formant glides. */
    double vbase = 1.0 + 6.0 * t + 0.9 * wv;
    if (vbase < 1.0) vbase = 1.0;
    if (vbase > 7.5) vbase = 7.5;
    double dbase = 0.5 + 0.30 * t + 0.20 * wd;   /* moves formant position */
    if (dbase < 0.06) dbase = 0.06;
    if (dbase > 0.94) dbase = 0.94;

    double detune = 0.002 + 0.02 * t;
    double dvoff  = 0.11;                          /* d offset for 2nd layer */
    /* ladder tames the buzz at high v (keeps flatness out of HISS territory) */
    double cutoff = 400.0 * pow(2.0, 1.4 + 3.4 * t + 0.6 * wc);
    double res    = 0.10 + 0.40 * t;
    double drive  = 0.14 + 0.40 * t;

    double atkc   = 1.0 - exp(-1.0 / (sr * 0.09));    /* ~90ms slow attack */
    /* long sustaining AR so one bang fills the render (no silent tail) */
    double relc   = exp(-1.0 / (sr * (2.2 + 2.5 * t)));/* release           */
    double holdn  = sr * (1.4 + 1.2 * t);            /* hold time          */
    double norm   = 0.85 / sqrt((double)(nv > 0 ? nv : 1));

    if (cutoff < 60.0) cutoff = 60.0;
    if (cutoff > sr * 0.45) cutoff = sr * 0.45;

    /* consume a trigger: reset phases + start the slow attack */
    if (x->x_trig) {
        x->x_trig = 0;
        x->x_seg = 1;
        x->x_hold = holdn;
        for (v = 0; v < nv; v++) { x->x_ph[v] = 0.0; x->x_ph2[v] = 0.37; }
    }

    while (nsamp--) {
        double s = 0.0, env = x->x_env;

        /* AR envelope: attack -> hold -> release */
        if (x->x_seg == 1) {
            env += atkc * (1.04 - env);
            if (env >= 1.0) { env = 1.0; x->x_seg = 2; }
        } else if (x->x_seg == 2) {
            env = 1.0;
            x->x_hold -= 1.0;
            if (x->x_hold <= 0.0) x->x_seg = 3;
        } else if (x->x_seg == 3) {
            env *= relc;
            if (env < 1e-6) { env = 0.0; x->x_seg = 0; }
        }
        x->x_env = env;

        if (env > 0.0) {
            /* per-voice d/v with a little per-voice detune of the formant */
            double d1 = dbase, d2 = dbase + dvoff;
            if (d1 < 0.01) d1 = 0.01; if (d1 > 0.99) d1 = 0.99;
            if (d2 < 0.01) d2 = 0.01; if (d2 > 0.99) d2 = 0.99;

            for (v = 0; v < nv; v++) {
                double f = x->x_freq[v];
                if (f < 8.0) f = 8.0;
                if (f > sr * 0.45) f = sr * 0.45;
                double inc1 = f / sr, inc2 = f * (1.0 + detune) / sr;
                x->x_ph[v]  = al_wrap01(x->x_ph[v]  + inc1);
                x->x_ph2[v] = al_wrap01(x->x_ph2[v] + inc2);
                /* two VPS layers per note: main + slightly detuned/offset */
                s += al_vps(x->x_ph[v],  d1, vbase);
                s += 0.7 * al_vps(x->x_ph2[v], d2, vbase);
            }
            s *= norm;
            s = al_ladder_tick(&x->lad, s, cutoff, res, sr);
            s *= env;
            s = al_drive(s, drive);
        }

        s = al_dc_tick(&x->dc, s);
        if (!(s > -1e6 && s < 1e6)) { s = 0.0; al_dc_init(&x->dc); }
        s = al_softclip(s * 1.15) * 0.78;
        if (s >  1.5) s =  1.5; else if (s < -1.5) s = -1.5;
        *out++ = (t_sample)s;
    }
    return (w + 4);
}

static void alien_vps_dsp(t_alien_vps *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr; if (x->x_sr < 1000) x->x_sr = 44100;
    dsp_add(alien_vps_perform, 3, x, sp[0]->s_vec, (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_vps_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_vps *x = (t_alien_vps *)pd_new(alien_vps_tilde_class);
    int i;
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    x->x_nv = 1; x->x_freq[0] = al_mtof(52);
    for (i = 0; i < MAXV; i++) { x->x_ph[i] = 0.0; x->x_ph2[i] = 0.37; }
    x->x_macro = 50.0;
    x->x_seg = 0; x->x_env = 0.0; x->x_hold = 0.0; x->x_trig = 0;
    al_ladder_init(&x->lad); al_dc_init(&x->dc);
    al_rng_seed(&x->rng, 0x7b501au); al_wander_init(&x->wnd, &x->rng, 4);

    /* inlet 1: float macro; inlet 2: list of notes */
    floatinlet_new(&x->x_obj, &x->x_macro);
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_list, gensym("notes"));
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_vps_tilde_setup(void)
{
    alien_vps_tilde_class = class_new(gensym("alien_vps~"),
        (t_newmethod)alien_vps_new, 0, sizeof(t_alien_vps),
        CLASS_DEFAULT, A_GIMME, 0);
    class_addbang(alien_vps_tilde_class, vps_bang);
    class_addmethod(alien_vps_tilde_class, (t_method)vps_macro,
                    gensym("macro"), A_FLOAT, 0);
    class_addmethod(alien_vps_tilde_class, (t_method)vps_notes,
                    gensym("notes"), A_GIMME, 0);
    class_addmethod(alien_vps_tilde_class, (t_method)alien_vps_dsp,
                    gensym("dsp"), A_CANT, 0);
}
