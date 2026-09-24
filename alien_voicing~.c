/* alien_voicing~.c  --  a CHORD-MORPH / VOICING engine (paraphonic pad)
 *
 * A note-list voice in the alien_chord~ family, but the MACRO does not just
 * change the timbre -- it re-VOICES the chord. Give it a chord as MIDI notes
 * and bang it, and turning the macro morphs the voicing itself: close root
 * position -> progressive inversions -> open/spread voicing -> octave doubling
 * -> added tensions (9ths / extensions). The harmony moves from voicing alone,
 * Phase Plant "morph between chords with a knob" style. The resulting pitches
 * are played as a lush detuned-saw pad through a shared resonant ladder with a
 * slow attack / long release, so a bang holds and swells while the macro sweep
 * glides the voices between arrangements (portamento per voice).
 *
 *   inlet 0   bang            TRIGGER -- re-voice the current note list from the
 *                             macro and fire the pad envelope (swells, holds)
 *   inlet 1   float           MACRO 0..100 -- sweeps the VOICING transform:
 *                             0 close, rising inversions, mid spread/open,
 *                             higher octave-doubled, top added tensions
 *   inlet 2   list            NOTES, MIDI note numbers e.g.  48 52 55 59
 *                             (stored as raw MIDI; re-voiced on the next bang
 *                             and continuously as the macro changes)
 *
 *   outlet 0  signal
 *
 * build:
 *   (macOS: -bundle -undefined dynamic_lookup -o alien_voicing~.pd_darwin ... )
 */

#include "m_pd.h"
#include "alien.h"

#define MAXIN 16         /* max input chord notes                     */
#define MAXV  32         /* max voices after doublings / extensions   */

static t_class *alien_voicing_tilde_class;

typedef struct _alien_voicing {
    t_object x_obj;

    double   x_sr;

    /* the input chord, stored as RAW MIDI notes so we can re-voice it */
    int      x_ninput;
    double   x_input[MAXIN];       /* MIDI note numbers, sorted low->high */

    /* the current re-voiced target set of pitches (Hz) + gliding pitches */
    int      x_nv;                 /* active voices                       */
    double   x_target[MAXV];       /* target frequency per voice (Hz)     */
    double   x_freq[MAXV];         /* gliding (portamento) frequency (Hz) */
    double   x_ph[MAXV], x_ph2[MAXV];

    t_float  x_macro;              /* inlet 1 float                       */
    double   x_lastmacro;          /* last macro used to build a voicing  */

    /* AR pad envelope: slow attack, hold at sustain, long release */
    int      x_seg;                /* 0 idle, 1 attack, 2 sustain(hold)   */
    double   x_env;
    int      x_trig;               /* set by bang, consumed in perform    */

    al_ladder lad;
    al_dc     dc;
    al_wander wnd;
    al_rng    rng;
} t_alien_voicing;

/* ------------------------------------------------------------------ */

/* clamp a MIDI note to a sane range */
static double voi_clampmidi(double m) {
    if (m < 12.0)  m = 12.0;
    if (m > 108.0) m = 108.0;
    return m;
}

/* Build the re-voiced set of pitches (Hz) for a given macro 0..1.
 * Writes into out[] (capacity MAXV) and returns the voice count.
 * The input chord (raw MIDI, sorted) drives the transform. */
static int voi_build(const double *in, int nin, double t, double *out)
{
    double notes[MAXV];
    int n = 0, i;

    if (nin < 1) return 0;
    if (nin > MAXIN) nin = MAXIN;

    /* start from a copy of the chord (already sorted low->high) */
    for (i = 0; i < nin; i++) notes[i] = in[i];
    n = nin;

    /* --- stage 1: progressive INVERSIONS (t ~0.0..0.35) ---------------
     * rotate the lowest note up an octave, one at a time, as t rises. */
    {
        int ninv = (int)(t * (1.0 / 0.35) * (double)nin);
        if (ninv > nin - 1) ninv = nin - 1;   /* keep at least one anchor */
        if (ninv < 0) ninv = 0;
        for (i = 0; i < ninv; i++) {
            /* lowest note is notes[0] because we re-sort after each rotate */
            double lo = notes[0] + 12.0;
            int j;
            for (j = 0; j < n - 1; j++) notes[j] = notes[j + 1];
            notes[n - 1] = lo;
            /* keep sorted (single element moved to end may need reinsert) */
            {
                int k;
                for (k = n - 1; k > 0 && notes[k] < notes[k - 1]; k--) {
                    double tmp = notes[k]; notes[k] = notes[k - 1]; notes[k - 1] = tmp;
                }
            }
        }
    }

    /* --- stage 2: SPREAD / open voicing (t ~0.30..0.65) ---------------
     * push alternate (odd-index) notes up an octave to open the voicing;
     * amount ramps in so it eases open as the macro rises. */
    {
        double spread = (t - 0.30) / 0.35;   /* 0..1 across this window */
        if (spread < 0.0) spread = 0.0;
        if (spread > 1.0) spread = 1.0;
        if (spread > 0.0) {
            for (i = 1; i < n; i += 2) {
                notes[i] += 12.0 * spread;    /* glide the note up an 8ve */
            }
        }
    }

    /* --- stage 3: OCTAVE DOUBLING (t ~0.60..0.85) --------------------
     * add a bass an octave below the lowest and a top an octave above the
     * highest for a big spread. amount fades the doublings in. */
    {
        double dbl = (t - 0.60) / 0.25;
        if (dbl > 0.0) {
            /* find current lowest and highest */
            double lo = notes[0], hi = notes[0];
            for (i = 1; i < n; i++) { if (notes[i] < lo) lo = notes[i]; if (notes[i] > hi) hi = notes[i]; }
            if (dbl > 1.0) dbl = 1.0;
            /* bass octave-down: fade its pitch from lo toward lo-12 */
            if (n < MAXV) { notes[n] = lo - 12.0 * dbl; n++; }
            /* top octave-up: fade from hi toward hi+12 */
            if (n < MAXV) { notes[n] = hi + 12.0 * dbl; n++; }
        }
    }

    /* --- stage 4: added TENSIONS (t ~0.80..1.0) ----------------------
     * append +7 (5th up an octave-ish) and +14 (9th) extensions above the
     * top for lush extended chords; glide them in from the top note. */
    {
        double tens = (t - 0.80) / 0.20;
        if (tens > 0.0) {
            double hi = notes[0];
            for (i = 1; i < n; i++) if (notes[i] > hi) hi = notes[i];
            if (tens > 1.0) tens = 1.0;
            if (n < MAXV) { notes[n] = hi + 7.0  * tens; n++; }   /* +5th   */
            if (n < MAXV) { notes[n] = hi + 14.0 * tens; n++; }   /* +9th   */
        }
    }

    /* clamp to a sane MIDI range then to Hz */
    if (n > MAXV) n = MAXV;
    for (i = 0; i < n; i++) {
        double m = voi_clampmidi(notes[i]);
        double f = al_mtof(m);
        if (f < 8.0) f = 8.0;
        out[i] = f;
    }
    return n;
}

/* recompute target voicing from the stored chord + current macro */
static void voi_revoice(t_alien_voicing *x, double t)
{
    double out[MAXV];
    int n = voi_build(x->x_input, x->x_ninput, t, out);
    int i;
    if (n < 1) { x->x_nv = 0; return; }
    x->x_nv = n;
    for (i = 0; i < n; i++) {
        x->x_target[i] = out[i];
        /* if this voice was idle, snap its glide to the target */
        if (!(x->x_freq[i] > 0.0)) x->x_freq[i] = out[i];
    }
    x->x_lastmacro = t;
}

/* ------------------------------------------------------------------ */

static void voicing_bang(t_alien_voicing *x) { x->x_trig = 1; }

static void voicing_macro(t_alien_voicing *x, t_floatarg f) { x->x_macro = f; }

/* a list of MIDI notes -> stored raw, sorted low->high */
static void voicing_notes(t_alien_voicing *x, t_symbol *s, int argc, t_atom *argv)
{
    double tmp[MAXIN];
    int i, j, n = 0;
    (void)s;
    for (i = 0; i < argc && n < MAXIN; i++) {
        double m = atom_getfloat(argv + i);
        if (!(m > 0.0)) continue;                 /* skip 0 / junk        */
        tmp[n++] = voi_clampmidi(m);
    }
    if (n < 1) return;
    /* insertion sort low->high */
    for (i = 1; i < n; i++) {
        double v = tmp[i];
        for (j = i - 1; j >= 0 && tmp[j] > v; j--) tmp[j + 1] = tmp[j];
        tmp[j + 1] = v;
    }
    for (i = 0; i < n; i++) x->x_input[i] = tmp[i];
    x->x_ninput = n;

    /* rebuild the voicing right away so it is ready for the next bang */
    {
        double t = al_param100((double)x->x_macro);
        voi_revoice(x, t);
    }
}

/* ------------------------------------------------------------------ */

static t_int *alien_voicing_perform(t_int *w)
{
    t_alien_voicing *x = (t_alien_voicing *)(w[1]);
    t_sample *out = (t_sample *)(w[2]);
    int nsamp = (int)(w[3]);

    double sr = x->x_sr;
    int nv, v;

    al_denorm_off();

    /* macro -> 0..1 (per block) */
    double t = al_param100((double)x->x_macro);
    if (!(t >= 0.0 && t <= 1.0)) t = 0.0;

    /* re-voice if the macro has moved enough since we last built (smooth
     * harmonic morph -- the glide below eases the pitches between voicings) */
    if (fabs(t - x->x_lastmacro) > 0.002) voi_revoice(x, t);

    nv = x->x_nv;
    if (nv > MAXV) nv = MAXV;

    /* slow wander for life */
    al_wander_tick(&x->wnd, &x->rng, 0.10 + 0.25 * t, sr);
    double wc = al_wander_bip(&x->wnd, 0);      /* cutoff drift  */
    double wd = al_wander_val(&x->wnd, 1);      /* detune drift  */

    double detune  = 0.002 + 0.010 * t + 0.006 * wd;
    double cutoff  = 220.0 * pow(2.0, 1.6 + 3.6 * t + 0.6 * wc);
    double res     = 0.14 + 0.42 * t;
    double drive   = 0.10 + 0.30 * t;

    /* AR pad envelope timings */
    double atktime = 0.05 + 0.35 * t;                     /* 50..400ms attack */
    double reltime = 1.5 + 4.0 * t;                       /* long release     */
    double atkc    = 1.0 - exp(-1.0 / (sr * atktime));
    double relc    = exp(-1.0 / (sr * reltime));
    double sustain = 0.9;                                 /* hold level       */

    /* glide coefficient for portamento between voicings (~80ms) */
    double gl      = 1.0 - exp(-1.0 / (sr * 0.08));

    double norm    = 0.9 / sqrt((double)(nv > 0 ? nv : 1));

    if (cutoff < 40.0) cutoff = 40.0;
    if (cutoff > sr * 0.45) cutoff = sr * 0.45;

    /* consume a trigger: reset phases + start the attack */
    if (x->x_trig) {
        x->x_trig = 0;
        x->x_seg = 1;
        /* snap glide to targets on a fresh strike so it starts in tune */
        for (v = 0; v < nv; v++) {
            x->x_freq[v] = x->x_target[v];
            x->x_ph[v] = (double)v * 0.13; x->x_ph2[v] = 0.37 + (double)v * 0.07;
            x->x_ph[v]  -= floor(x->x_ph[v]);
            x->x_ph2[v] -= floor(x->x_ph2[v]);
        }
    }

    while (nsamp--) {
        double s = 0.0, env = x->x_env, y;

        /* AR envelope */
        if (x->x_seg == 1) {
            env += atkc * (sustain * 1.02 - env);
            if (env >= sustain) { env = sustain; x->x_seg = 2; }
        } else if (x->x_seg == 2) {
            /* hold; slowly ease toward sustain in case atk overshot */
            env += 0.0005 * (sustain - env);
            (void)relc;
        }
        x->x_env = env;

        if (env > 1e-6 && nv > 0) {
            for (v = 0; v < nv; v++) {
                double ft;
                /* portamento glide toward the target voicing */
                x->x_freq[v] += gl * (x->x_target[v] - x->x_freq[v]);
                ft = x->x_freq[v];
                if (ft < 8.0) ft = 8.0;
                if (ft > sr * 0.45) ft = sr * 0.45;
                {
                    double d1 = ft / sr, d2 = ft * (1.0 + detune) / sr;
                    x->x_ph[v]  = al_wrap01(x->x_ph[v]  + d1);
                    x->x_ph2[v] = al_wrap01(x->x_ph2[v] + d2);
                    s += al_saw_bl(x->x_ph[v], d1) + al_saw_bl(x->x_ph2[v], d2);
                }
            }
            s *= norm;
            s = al_ladder_tick(&x->lad, s, cutoff, res, sr);
            s *= env;
            s = al_drive(s, drive);
        }

        y = al_dc_tick(&x->dc, s);
        if (!(y > -1e6 && y < 1e6)) y = 0.0;
        y = al_softclip(y * 1.4) * 0.8;
        if (y < -1.5) y = -1.5;
        if (y >  1.5) y =  1.5;
        *out++ = (t_sample)y;
    }
    return (w + 4);
}

static void alien_voicing_dsp(t_alien_voicing *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr; if (x->x_sr < 1000) x->x_sr = 44100;
    dsp_add(alien_voicing_perform, 3, x, sp[0]->s_vec, (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_voicing_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_voicing *x = (t_alien_voicing *)pd_new(alien_voicing_tilde_class);
    int i;
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;

    /* default chord: a simple triad (C E G) */
    x->x_ninput = 3;
    x->x_input[0] = 48.0; x->x_input[1] = 52.0; x->x_input[2] = 55.0;

    for (i = 0; i < MAXV; i++) {
        x->x_ph[i] = 0.0; x->x_ph2[i] = 0.37;
        x->x_freq[i] = 0.0; x->x_target[i] = 0.0;
    }
    x->x_macro = 50.0;
    x->x_lastmacro = -1.0;
    x->x_seg = 0; x->x_env = 0.0; x->x_trig = 0;
    x->x_nv = 0;

    al_ladder_init(&x->lad); al_dc_init(&x->dc);
    al_rng_seed(&x->rng, 0x901ce5u); al_wander_init(&x->wnd, &x->rng, 4);

    /* build an initial voicing so it is ready before the first macro tick */
    voi_revoice(x, al_param100((double)x->x_macro));

    /* inlet 1: float macro; inlet 2: list of notes */
    floatinlet_new(&x->x_obj, &x->x_macro);
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_list, gensym("notes"));
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_voicing_tilde_setup(void)
{
    alien_voicing_tilde_class = class_new(gensym("alien_voicing~"),
        (t_newmethod)alien_voicing_new, 0, sizeof(t_alien_voicing),
        CLASS_DEFAULT, A_GIMME, 0);
    class_addbang(alien_voicing_tilde_class, voicing_bang);
    class_addmethod(alien_voicing_tilde_class, (t_method)voicing_macro,
                    gensym("macro"), A_FLOAT, 0);
    class_addmethod(alien_voicing_tilde_class, (t_method)voicing_notes,
                    gensym("notes"), A_GIMME, 0);
    class_addmethod(alien_voicing_tilde_class, (t_method)alien_voicing_dsp,
                    gensym("dsp"), A_CANT, 0);
}
