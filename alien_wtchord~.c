/* alien_wtchord~ -- WAVETABLE CHORD with Ableton/Serum-style UNISON MODES
 *
 * A bang plays a chord where each note is a morphing single-cycle WAVETABLE plus
 * a per-note UNISON cluster. The macro scans the wavetable POSITION and sweeps
 * through unison MODES (classic detune -> shimmer -> phase/position-spread ->
 * random-note). The signature modern lush evolving wavetable pad, polyphonic.
 *
 * THE STACK (built at init, band-limited-ish -- bounded harmonic sums, peak-
 * normalised so morphing does not jump level):
 *   0 sine  1 triangle  2 saw  3 square  4 bright/growl  5 formant-bumped
 * The macro scans the wavetable read POSITION across this stack -> the timbre
 * morph, the primary move.
 *
 * UNISON MODES (the macro tours across its range, crossfading between them):
 *   CLASSIC        symmetric beat-rate detune (the supersaw spread)
 *   SHIMMER        slow independent random detune drift per unison voice
 *   PHASE-SPREAD   unison voices start at spread phases (wide, no comb)
 *   POSITION-SPREAD each unison voice reads a slightly different table position
 *   RANDOM-NOTE    some unison voices jump an octave / fifth (harmonic sparkle)
 * These are not switched hard -- the macro fades their weights so the character
 * evolves continuously from tight-classic to wide-random as the macro opens.
 *
 * INTERFACE (note-list voice, copied from alien_chord~):
 *   inlet 0   bang            TRIGGER -- (re)voice the note list, fire the amp
 *                             env (slow attack, long sustain/release -> a pad)
 *   inlet 1   float           MACRO 0..100 -- scans table position, widens the
 *                             unison, tours the modes, opens the shared ladder
 *   inlet 2   list            NOTES, MIDI note numbers e.g. 40 47 52 55 59 64
 *   outlet 0  signal
 *
 * BRAIN: a slow al_wander drifts position / detune / mode-bias for life --
 * secondary to the macro, never the main move.
 *
 * SAFETY (-ffast-math -> magnitude clamps): total unison-voice count clamped to
 * array capacity; wavetable indices masked in-bounds; freq clamped [8,0.45*sr];
 * ladder stable; normalise ~1/sqrt(total unison voices) so a big chord cannot
 * rail; finite guard then bound ~[-1.5,1.5]. Handles empty list / many notes /
 * re-bang.
 *
 * build (macOS): -bundle -undefined dynamic_lookup -o alien_wtchord~.pd_darwin
 */

#include "m_pd.h"
#include "alien.h"

#define MAXV     8            /* max simultaneous notes                        */
#define UNI      5            /* unison voices per note                        */
#define WT_FRAMES 6           /* single-cycle frames in the stack              */
#define WT_LEN   2048         /* samples per frame (power of two)              */
#define WT_MASK  (WT_LEN - 1)

static t_class *alien_wtchord_tilde_class;

typedef struct _alien_wtchord {
    t_object x_obj;

    double   x_sr;
    int      x_nv;                         /* active notes                    */
    double   x_freq[MAXV];                 /* note frequencies (Hz)           */
    double   x_ph[MAXV][UNI];              /* per unison-voice read phase     */
    double   x_drift[MAXV][UNI];           /* shimmer detune drift state      */

    t_float  x_macro;                      /* inlet 1 float                   */

    double  *tab;                          /* WT_FRAMES * WT_LEN cycles       */

    /* amp envelope (pad: attack / sustain-hold / long release) */
    int      x_seg;                        /* 0 idle 1 atk 2 sustain 3 rel    */
    double   x_env;
    int      x_trig;                       /* set by bang, consumed in perform*/
    double   x_hold;                       /* sustain-time countdown (samples)*/

    al_ladder lad;
    al_dc     dc;
    al_wander wnd;
    al_rng    rng;
} t_alien_wtchord;

/* one masked, linearly-interpolated read of frame `fr` at phase p (0..1) */
static inline double wt_read(const double *tab, int fr, double p)
{
    double fp = p * WT_LEN;
    int i0 = (int)fp; double frac = fp - i0;
    int a = (fr * WT_LEN) + (i0 & WT_MASK);
    int b = (fr * WT_LEN) + ((i0 + 1) & WT_MASK);
    return al_lerp(tab[a], tab[b], frac);
}

/* read a fractional POSITION across the frame stack (crossfade neighbours),
 * each frame read at phase p with linear interpolation. pos in 0..WT_FRAMES-1 */
static inline double wt_scan(const double *tab, double pos, double p)
{
    int fa, fb; double fpos;
    if (pos < 0.0) pos = 0.0;
    if (pos > (double)(WT_FRAMES - 1)) pos = (double)(WT_FRAMES - 1);
    fa = (int)pos; if (fa > WT_FRAMES - 2) fa = WT_FRAMES - 2;
    fb = fa + 1;
    fpos = pos - fa; if (fpos < 0.0) fpos = 0.0; if (fpos > 1.0) fpos = 1.0;
    return al_lerp(wt_read(tab, fa, p), wt_read(tab, fb, p), fpos);
}

/* Build the frame stack: each frame a single cycle from a bounded harmonic sum,
 * peak-normalised so every frame sits near +/-1 (morphing does not jump level). */
static void wt_build(double *tab)
{
    int f, i, k;
    for (f = 0; f < WT_FRAMES; f++) {
        double *fr = tab + f * WT_LEN;
        double peak = 1e-9;
        for (i = 0; i < WT_LEN; i++) {
            double ph = (double)i / WT_LEN;
            double th = AL_2PI * ph;
            double s = 0.0;
            switch (f) {
                case 0:  /* sine */
                    s = sin(th);
                    break;
                case 1:  /* triangle: odd harmonics, 1/k^2, alternating sign */
                    for (k = 1; k <= 15; k += 2) {
                        double sg = (((k - 1) / 2) & 1) ? -1.0 : 1.0;
                        s += sg * sin(k * th) / (double)(k * k);
                    }
                    break;
                case 2:  /* saw: all harmonics, 1/k */
                    for (k = 1; k <= 24; k++) s += sin(k * th) / (double)k;
                    break;
                case 3:  /* square: odd harmonics, 1/k */
                    for (k = 1; k <= 23; k += 2) s += sin(k * th) / (double)k;
                    break;
                case 4:  /* bright/growl: dense stack, gentle rolloff, phase skew */
                    for (k = 1; k <= 22; k++) {
                        double a = pow(0.88, k - 1);
                        s += a * sin(k * th + 0.3 * k);
                    }
                    break;
                default: /* formant-bumped: saw-ish with a boosted band ~h5-8 */
                    for (k = 1; k <= 20; k++) {
                        double a = 1.0 / (double)k;
                        double bump = exp(-((k - 6.0) * (k - 6.0)) / 6.0);
                        s += (a + 1.6 * bump) * sin(k * th);
                    }
                    break;
            }
            fr[i] = s;
        }
        for (i = 0; i < WT_LEN; i++) { double a = fabs(fr[i]); if (a > peak) peak = a; }
        for (i = 0; i < WT_LEN; i++) fr[i] /= peak;
    }
}

/* ------------------------------------------------------------------ */

static void wtchord_bang(t_alien_wtchord *x) { x->x_trig = 1; }

static void wtchord_macro(t_alien_wtchord *x, t_floatarg f) { x->x_macro = f; }

/* a list of MIDI notes -> voice frequencies */
static void wtchord_notes(t_alien_wtchord *x, t_symbol *s, int argc, t_atom *argv)
{
    int i, n = 0;
    (void)s;
    for (i = 0; i < argc && n < MAXV; i++) {
        double m = atom_getfloat(argv + i);
        if (!(m > 0.0)) continue;                 /* skip 0 / junk           */
        x->x_freq[n] = al_mtof(m);
        n++;
    }
    if (n > 0) x->x_nv = n;
}

/* ------------------------------------------------------------------ */

static t_int *alien_wtchord_perform(t_int *w)
{
    t_alien_wtchord *x = (t_alien_wtchord *)(w[1]);
    t_sample *out = (t_sample *)(w[2]);
    int nsamp = (int)(w[3]);

    double sr = x->x_sr;
    const double *tab = x->tab;
    int nv = x->x_nv, v, u;

    /* macro -> character (per block) */
    double t = x->x_macro; t = t - 100.0 * floor(t * 0.01);
    if (!(t >= 0.0 && t <= 100.0)) t = 0.0;
    t *= 0.01;

    /* slow wander for life */
    al_wander_tick(&x->wnd, &x->rng, 0.12 + 0.35 * t, sr);
    double wpos = al_wander_bip(&x->wnd, 0);      /* position drift          */
    double wdet = al_wander_val(&x->wnd, 1);      /* detune drift            */
    double wmod = al_wander_bip(&x->wnd, 2);      /* mode-bias drift         */

    /* wavetable read position scanned by the macro (+ slow drift) */
    double pos = al_clamp(t * (WT_FRAMES - 1) + 0.5 * wpos, 0.0,
                          (double)(WT_FRAMES - 1));

    /* unison width grows with the macro */
    double width  = 0.0015 + 0.028 * t + 0.006 * wdet;   /* base detune amount */

    /* ---- UNISON MODE WEIGHTS: the macro tours across the modes ------------ */
    /* five overlapping triangular windows across t=0..1, so the character
     * fades continuously classic -> shimmer -> phase -> position -> random.  */
    double centres[5] = { 0.05, 0.30, 0.52, 0.74, 0.95 };
    double wsum = 1e-9, mw[5];
    int mi;
    double tb = al_clamp(t + 0.12 * wmod, 0.0, 1.0);   /* biased tour position */
    for (mi = 0; mi < 5; mi++) {
        double d = (tb - centres[mi]) / 0.24;
        double g = 1.0 - fabs(d); if (g < 0.0) g = 0.0;
        mw[mi] = g; wsum += g;
    }
    for (mi = 0; mi < 5; mi++) mw[mi] /= wsum;
    /* mode weights: 0 classic 1 shimmer 2 phase 3 position 4 random-note */

    /* shared resonant ladder opens with the macro */
    double cutoff = 200.0 * pow(2.0, 1.3 + 4.4 * t + 0.6 * (wpos));
    double res    = 0.10 + 0.55 * t;
    if (cutoff < 40.0) cutoff = 40.0;
    if (cutoff > sr * 0.45) cutoff = sr * 0.45;

    /* pad envelope timings */
    double atkc = 1.0 - exp(-1.0 / (sr * (0.05 + 0.20 * t)));  /* 50..250ms atk */
    double relc = exp(-1.0 / (sr * (1.2 + 3.5 * t)));          /* long release  */
    double holdsamps = sr * (0.6 + 1.5 * t);                   /* sustain hold  */

    /* normalise: total unison voices summed incoherently */
    double totuv = (double)((nv > 0 ? nv : 1) * UNI);
    /* makeup: wide/phase-spread/open-filter positions lose coherent energy,
     * so lift the level a touch as the macro opens to keep the pad present */
    double norm  = (1.30 + 0.45 * t) / sqrt(totuv);

    double drive = 0.10 + 0.40 * t;

    /* random-note interval multipliers per unison voice (fixed pattern) */
    static const double rnmul[UNI] = { 1.0, 2.0, 1.5, 0.5, 3.0 };

    /* consume a trigger: reset phases (with mode-appropriate spread) + attack */
    if (x->x_trig) {
        x->x_trig = 0;
        x->x_seg = 1;
        x->x_hold = holdsamps;
        for (v = 0; v < nv; v++) {
            for (u = 0; u < UNI; u++) {
                /* phase-spread start so PHASE-SPREAD mode is wide from note-on */
                x->x_ph[v][u]    = al_wrap01(0.13 * (v + 1) + (double)u / UNI);
                x->x_drift[v][u] = 0.0;
            }
        }
    }

    while (nsamp--) {
        double s = 0.0, env = x->x_env, y;

        /* pad envelope: attack -> hold sustain -> long release */
        if (x->x_seg == 1) {
            env += atkc * (1.03 - env);
            if (env >= 1.0) { env = 1.0; x->x_seg = 2; }
        } else if (x->x_seg == 2) {
            x->x_hold -= 1.0;
            if (x->x_hold <= 0.0) x->x_seg = 3;
        } else if (x->x_seg == 3) {
            env *= relc;
            if (env < 1e-6) { env = 0.0; x->x_seg = 0; }
        }
        x->x_env = env;

        if (env > 0.0) {
            for (v = 0; v < nv; v++) {
                double f = x->x_freq[v];
                if (!(f > 8.0)) f = 8.0;
                if (f > sr * 0.45) f = sr * 0.45;

                for (u = 0; u < UNI; u++) {
                    /* symmetric detune spread across the unison cluster */
                    double spread = (UNI > 1) ? ((double)u / (UNI - 1) - 0.5) * 2.0
                                              : 0.0;

                    /* SHIMMER: slow independent random detune drift per voice */
                    x->x_drift[v][u] += 0.00002 * (al_randb(&x->rng)
                                        - x->x_drift[v][u] * 0.5);
                    double shim = x->x_drift[v][u] * 18.0;   /* bounded ~[-1,1]*/

                    /* CLASSIC detune + SHIMMER drift blended by mode weights */
                    double det = 1.0
                        + width * spread * (mw[0] + 0.6 * mw[2])   /* classic/phase */
                        + width * shim   * mw[1];                  /* shimmer     */

                    /* RANDOM-NOTE: some unison voices jump to a harmonic */
                    double mul = al_lerp(1.0, rnmul[u], mw[4]);

                    double fu = f * det * mul;
                    if (!(fu > 8.0)) fu = 8.0;
                    if (fu > sr * 0.45) fu = sr * 0.45;

                    x->x_ph[v][u] = al_wrap01(x->x_ph[v][u] + fu / sr);

                    /* POSITION-SPREAD: each unison voice reads a slightly
                     * different wavetable position (weighted by that mode) */
                    double vpos = pos + (spread * 0.9) * mw[3];

                    s += wt_scan(tab, vpos, x->x_ph[v][u]);
                }
            }
            s *= norm;
            s = al_ladder_tick(&x->lad, s, cutoff, res, sr);
            s *= env;
            s = al_drive(s, drive);
        }

        y = al_dc_tick(&x->dc, s);
        if (!(y > -1e6 && y < 1e6)) { y = 0.0; al_ladder_init(&x->lad); }
        y = al_softclip(y);
        if (y >  1.5) y =  1.5; else if (y < -1.5) y = -1.5;
        *out++ = (t_sample)(y * 0.85);
    }
    return (w + 4);
}

static void alien_wtchord_dsp(t_alien_wtchord *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr; if (x->x_sr < 1000) x->x_sr = 44100;
    dsp_add(alien_wtchord_perform, 3, x, sp[0]->s_vec, (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_wtchord_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_wtchord *x = (t_alien_wtchord *)pd_new(alien_wtchord_tilde_class);
    int i, j;
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    x->x_nv = 1; x->x_freq[0] = al_mtof(50);
    for (i = 0; i < MAXV; i++)
        for (j = 0; j < UNI; j++) { x->x_ph[i][j] = 0.0; x->x_drift[i][j] = 0.0; }
    x->x_macro = 50.0;
    x->x_seg = 0; x->x_env = 0.0; x->x_trig = 0; x->x_hold = 0.0;

    x->tab = (double *)getbytes(sizeof(double) * WT_FRAMES * WT_LEN);
    if (x->tab) wt_build(x->tab);

    al_ladder_init(&x->lad); al_dc_init(&x->dc);
    al_rng_seed(&x->rng, 0x7c0d3au); al_wander_init(&x->wnd, &x->rng, 4);

    /* inlet 1: float macro; inlet 2: list of notes */
    floatinlet_new(&x->x_obj, &x->x_macro);
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_list, gensym("notes"));
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

static void alien_wtchord_free(t_alien_wtchord *x)
{
    if (x->tab) freebytes(x->tab, sizeof(double) * WT_FRAMES * WT_LEN);
}

void alien_wtchord_tilde_setup(void)
{
    alien_wtchord_tilde_class = class_new(gensym("alien_wtchord~"),
        (t_newmethod)alien_wtchord_new, (t_method)alien_wtchord_free,
        sizeof(t_alien_wtchord), CLASS_DEFAULT, A_GIMME, 0);
    class_addbang(alien_wtchord_tilde_class, wtchord_bang);
    class_addmethod(alien_wtchord_tilde_class, (t_method)wtchord_macro,
                    gensym("macro"), A_FLOAT, 0);
    class_addmethod(alien_wtchord_tilde_class, (t_method)wtchord_notes,
                    gensym("notes"), A_GIMME, 0);
    class_addmethod(alien_wtchord_tilde_class, (t_method)alien_wtchord_dsp,
                    gensym("dsp"), A_CANT, 0);
}
