/* alien_addchord~.c  --  an ADDITIVE-FUSED chord voice
 *
 * Not a stack of independent voices, but ONE fused additive tone whose partial
 * spectrum is the COMBINED harmonic series of every note in the chord. Each
 * note contributes partials at k*freq (k = 1..K); where notes share a partial
 * (consonances -- octaves, fifths) those partials reinforce, so a chord fuses
 * into a single rich, glowing organ / bell / spectral voice rather than a pile
 * of distinct pitches. Strong harmonic interest: change the chord and the whole
 * timbre reorganises.
 *
 * A single TOTAL partial budget (MAXPART) is shared across all the notes -- a
 * big chord gets fewer partials per note, so the fused sum can never exceed the
 * array capacity. Partials above Nyquist are dropped (never aliases). Per-
 * partial amplitude ~ 1/k with a macro TILT (dark -> bright) and an optional
 * inharmonic STRETCH ((k*freq)^stretch) for bell tones up top. A slow per-
 * partial shimmer (al_wander) keeps the fused tone alive and evolving. A bang
 * fires an AD amp envelope (swell + long decay) so the chord blooms like a
 * struck organ / bell.
 *
 *   inlet 0   bang            TRIGGER -- (re)fire the amp envelope; the fused
 *                             chord blooms and decays (no note-off needed)
 *   inlet 1   float           MACRO 0..100 -- few partials / dark / harmonic /
 *                             short at 0  ->  many partials / bright / slightly
 *                             inharmonic / long / shimmering at 100
 *   inlet 2   list            NOTES, MIDI note numbers e.g.  40 47 52 55 59 64
 *                             (stored; fused on the next bang; mtof internally)
 *
 *   outlet 0  signal
 *
 * build:
 *   (macOS: -bundle -undefined dynamic_lookup -o alien_addchord~.pd_darwin ... )
 */

#include "m_pd.h"
#include "alien.h"

#define MAXNOTE  16       /* max notes in a chord                              */
#define MAXPART  128      /* total partial budget shared across ALL notes      */
#define MAXK     24       /* max harmonics requested from any one note         */

static t_class *alien_addchord_tilde_class;

typedef struct _alien_addchord {
    t_object x_obj;

    double   x_sr;
    int      x_nn;                    /* active note count                     */
    double   x_note[MAXNOTE];         /* note fundamentals (Hz)                */

    t_float  x_macro;                 /* inlet 1 float                         */

    /* per-block partial tables (built when macro / notes / trigger change) */
    int      x_np;                    /* active partial count [0,MAXPART]      */
    double   x_pfreq[MAXPART];        /* partial frequency (Hz)                */
    double   x_pph[MAXPART];          /* partial phase 0..1                    */
    double   x_pamp[MAXPART];         /* partial amplitude (tilt applied)      */
    double   x_pshinc[MAXPART];       /* per-partial shimmer LFO increment     */
    double   x_pshph[MAXPART];        /* per-partial shimmer LFO phase         */
    double   x_shdepth;               /* shimmer depth (0 = off)               */
    double   x_norm;                  /* 1/sum(amps) so the sum stays ~unity   */
    int      x_dirty;                 /* rebuild the partial tables next block */
    double   x_lastmac;               /* last built macro, to detect change    */

    /* AD amp envelope */
    int      x_seg;                   /* 0 idle, 1 attack, 2 decay             */
    double   x_env;
    double   x_atkc, x_decc;          /* cached env coefs                      */
    int      x_trig;                  /* set by bang, consumed in perform      */

    al_dc     dc;
    al_wander wnd;
    al_rng    rng;
} t_alien_addchord;

/* ------------------------------------------------------------------ */

static void addchord_bang(t_alien_addchord *x) { x->x_trig = 1; }

/* a list of MIDI notes -> note fundamentals */
static void addchord_notes(t_alien_addchord *x, t_symbol *s, int argc, t_atom *argv)
{
    int i, n = 0;
    (void)s;
    for (i = 0; i < argc && n < MAXNOTE; i++) {
        double m = atom_getfloat(argv + i);
        if (!(m > 0.0)) continue;                 /* skip 0 / junk            */
        x->x_note[n] = al_mtof(m);
        n++;
    }
    if (n > 0) { x->x_nn = n; x->x_dirty = 1; }
}

/* ------------------------------------------------------------------
 * Build the fused partial spectrum from the current notes + macro.
 * Runs once per block, only when something changed -- all the pow()/tilt/
 * budget work lives here; the per-sample loop is just phase + sin + sum.
 */
static void addchord_build(t_alien_addchord *x)
{
    double sr = x->x_sr, nyq = sr * 0.45;
    double t = x->x_macro;
    int nn = x->x_nn, i, k, np = 0;
    double sum = 0.0;

    /* clamp the macro into [0,100] then normalise to [0,1] (so 100 = full
     * bright/long, not wrapped back to 0) */
    if (!(t == t)) t = 0.0;                        /* NaN guard                */
    if (t < 0.0)   t = 0.0;
    if (t > 100.0) t = 100.0;
    t *= 0.01;

    if (nn < 1) nn = 1;
    if (nn > MAXNOTE) nn = MAXNOTE;

    /* macro trajectory: few/dark/harmonic/short -> many/bright/bell/long/shimmer */
    double tilt    = 0.2 + 1.6 * t;               /* 1/k^tilt: dark -> bright  */
    double stretch = 1.0 + 0.06 * t * t;          /* neutral -> slight bell    */
    double shimmer = (t < 0.35) ? 0.0 : (t - 0.35) / 0.65;
    double shimrate= 0.08 + 0.5 * t;              /* slow drift                */

    /* total partial budget for THIS chord: few at 0, many at 100, but never
     * more than the array holds */
    int budget = (int)(8.0 + (MAXPART - 12) * t);
    if (budget < 4)        budget = 4;
    if (budget > MAXPART)  budget = MAXPART;

    /* harmonics requested per note (round-robin allocation caps the total) */
    int kper = budget / nn;
    if (kper < 1)     kper = 1;
    if (kper > MAXK)  kper = MAXK;

    x->x_shdepth = (shimmer > 0.0) ? shimmer * 0.010 : 0.0;

    /* Allocate partials round-robin across notes (harmonic k = 1 first for
     * every note, then k = 2 for every note, ...) so a big chord still gets its
     * low, loud partials before the budget runs out. */
    for (k = 1; k <= kper && np < budget && np < MAXPART; k++) {
        for (i = 0; i < nn && np < budget && np < MAXPART; i++) {
            double f0 = x->x_note[i];
            double fr, a;

            if (!(f0 == f0)) f0 = 110.0;          /* NaN guard                */
            if (f0 < 8.0)    f0 = 8.0;
            if (f0 > nyq)    f0 = nyq;

            /* stretch: (k*f0) with a mild inharmonic power up top */
            fr = f0 * pow((double)k, stretch);

            if (fr < 8.0 || fr >= nyq) continue;  /* drop, never fold/alias    */

            /* 1/k^tilt spectral slope */
            a = pow((double)k, -tilt);
            if (!(a == a) || a < 0.0) a = 0.0;    /* NaN/neg guard             */
            if (a <= 0.0) continue;

            x->x_pfreq[np] = fr;
            x->x_pamp[np]  = a;
            x->x_pshinc[np]= (x->x_shdepth > 0.0)
                           ? shimrate * (0.5 + 0.5 * (double)np / budget) / sr
                           : 0.0;
            sum += a;
            np++;
        }
    }

    x->x_np      = np;
    x->x_norm    = (sum > 1e-6) ? (1.0 / sum) : 1.0;
    x->x_lastmac = x->x_macro;

    /* AD envelope timing: short at 0 -> long swell/decay at 100 */
    {
        double atktime = 0.004 + 0.05 * t;        /* ~4..54 ms swell           */
        double dectime = 2.2 + 4.0 * t;           /* 2.2..6.2 s decay          */
        x->x_atkc = 1.0 - exp(-1.0 / (sr * atktime));
        x->x_decc = exp(-1.0 / (sr * dectime));
    }

    x->x_dirty = 0;
}

/* ------------------------------------------------------------------ */

static t_int *alien_addchord_perform(t_int *w)
{
    t_alien_addchord *x = (t_alien_addchord *)(w[1]);
    t_sample *out = (t_sample *)(w[2]);
    int nsamp = (int)(w[3]);

    double sr = x->x_sr;
    int np, i;

    al_denorm_off();

    /* rebuild the fused spectrum if the chord or macro changed */
    if (x->x_dirty || x->x_macro != x->x_lastmac) addchord_build(x);

    /* consume a trigger: (re)fire the amp envelope + reset phases */
    if (x->x_trig) {
        x->x_trig = 0;
        x->x_seg  = 1;
        for (i = 0; i < x->x_np; i++) x->x_pph[i] = 0.0;
    }

    /* slow wander gives the fused tone a living, breathing motion */
    al_wander_tick(&x->wnd, &x->rng, 0.20, sr);

    np = x->x_np;
    double norm = x->x_norm, shdepth = x->x_shdepth;
    double atkc = x->x_atkc, decc = x->x_decc;

    while (nsamp--) {
        double s = 0.0, env = x->x_env;

        /* AD envelope */
        if (x->x_seg == 1) {
            env += atkc * (1.04 - env);
            if (env >= 1.0) { env = 1.0; x->x_seg = 2; }
        } else if (x->x_seg == 2) {
            env *= decc;
            if (env < 1e-6) { env = 0.0; x->x_seg = 0; }
        }
        x->x_env = env;

        if (env > 0.0) {
            for (i = 0; i < np; i++) {
                double det = 1.0, inc;

                if (shdepth > 0.0) {
                    x->x_pshph[i] += x->x_pshinc[i];
                    if (x->x_pshph[i] >= 1.0) x->x_pshph[i] -= 1.0;
                    det = 1.0 + shdepth * al_sin01(x->x_pshph[i]);
                }

                inc = x->x_pfreq[i] * det / sr;
                x->x_pph[i] = al_wrap01(x->x_pph[i] + inc);
                s += x->x_pamp[i] * al_sin01(x->x_pph[i]);
            }
            s *= norm * env;
        }

        s = al_dc_tick(&x->dc, s);

        /* finite guard, then bound */
        if (!(s > -1e6 && s < 1e6)) s = 0.0;
        s = al_softclip(s * 2.0);
        if (!(s > -1.5 && s < 1.5)) s = 0.0;

        *out++ = (t_sample)(s * 0.9);
    }
    return (w + 4);
}

static void alien_addchord_dsp(t_alien_addchord *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr; if (x->x_sr < 1000) x->x_sr = 44100;
    x->x_dirty = 1;
    dsp_add(alien_addchord_perform, 3, x, sp[0]->s_vec, (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_addchord_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_addchord *x = (t_alien_addchord *)pd_new(alien_addchord_tilde_class);
    int i;
    (void)s; (void)argc; (void)argv;

    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;

    /* default: a single note so an empty/absent list still sounds */
    x->x_nn = 1; x->x_note[0] = al_mtof(50);
    for (i = 1; i < MAXNOTE; i++) x->x_note[i] = al_mtof(50);

    x->x_macro = 50.0; x->x_lastmac = -1e30;
    x->x_np = 0; x->x_norm = 1.0; x->x_shdepth = 0.0; x->x_dirty = 1;
    for (i = 0; i < MAXPART; i++) {
        x->x_pph[i]   = (double)i * 0.0137;
        x->x_pshph[i] = (double)i / (double)MAXPART;
        x->x_pfreq[i] = 0.0; x->x_pamp[i] = 0.0; x->x_pshinc[i] = 0.0;
    }

    x->x_seg = 0; x->x_env = 0.0; x->x_trig = 0;
    x->x_atkc = 0.02; x->x_decc = 0.9999;

    al_dc_init(&x->dc);
    al_rng_seed(&x->rng, 0xADDC0Du);
    al_wander_init(&x->wnd, &x->rng, 6);

    /* inlet 1: float macro; inlet 2: list of notes */
    floatinlet_new(&x->x_obj, &x->x_macro);
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_list, gensym("notes"));
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_addchord_tilde_setup(void)
{
    alien_addchord_tilde_class = class_new(gensym("alien_addchord~"),
        (t_newmethod)alien_addchord_new, 0, sizeof(t_alien_addchord),
        CLASS_DEFAULT, A_GIMME, 0);
    class_addbang(alien_addchord_tilde_class, addchord_bang);
    class_addmethod(alien_addchord_tilde_class, (t_method)addchord_notes,
                    gensym("notes"), A_GIMME, 0);
    class_addmethod(alien_addchord_tilde_class, (t_method)alien_addchord_dsp,
                    gensym("dsp"), A_CANT, 0);
}
