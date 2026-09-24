/* alien_pluck~.c  --  a PLUCKED + SYMPATHETIC chord (mono object, polyphony)
 *
 * Bang it with a list of MIDI notes and every note becomes a tuned Karplus-
 * Strong string: a short filtered noise burst is injected into a pitch-tuned
 * delay line whose feedback loop carries a damping lowpass, so each string
 * rings and decays like a plucked string.  The strings are tuned to the chord
 * notes and their partials sit slightly apart, so they BEAT against each other
 * -- that mutual detuning/beating is where the harmonic richness comes from.
 * A little of the summed mix is fed sympathetically back into every string
 * (net feedback kept strictly < 1) so plucking one note stirs the others.
 *
 * Same note-list voice interface as alien_chord~ (control-only DSP, no signal
 * inlets):
 *
 *   inlet 0   bang            TRIGGER -- (re)pluck every string in the list
 *   inlet 1   float           MACRO 0..100 -- dark/short/damped pluck at 0 ->
 *                             bright/long/ringing (less damping, higher
 *                             feedback, brighter burst) at 100
 *   inlet 2   list            NOTES, MIDI note numbers e.g. 40 47 52 55 59 64
 *                             (stored; plucked on the next bang; mtof internally)
 *
 *   outlet 0  signal
 *
 * SAFETY: every string loop feedback strictly < 1; the in-loop lowpass is
 * always lossy so a loop cannot self-oscillate to infinity; delay lengths
 * clamped in [1, DLEN-2]; freq clamped to [8, 0.45*sr]; all state denormal-
 * flushed + finite-guarded; the sympathetic coupling is scaled so net loop
 * gain stays below unity; output finite-guarded and bounded to ~[-1.5,1.5].
 *
 * build:
 *   (macOS: -bundle -undefined dynamic_lookup -o alien_pluck~.pd_darwin ... )
 */

#include "m_pd.h"
#include "alien.h"

#define MAXV 16          /* max simultaneous strings                          */
#define DLEN 4096        /* per-string delay line (~92ms @ 44.1k -> ~10.7 Hz) */
#define DMASK (DLEN - 1)

static t_class *alien_pluck_tilde_class;

typedef struct _alien_pluck {
    t_object x_obj;

    double   x_sr;
    int      x_nv;                 /* active strings                          */
    double   x_freq[MAXV];         /* string frequencies (Hz)                 */

    /* per-string Karplus-Strong delay lines */
    double   x_buf[MAXV][DLEN];
    int      x_wi[MAXV];           /* per-string write index                  */
    double   x_damplp[MAXV];       /* per-string in-loop damping lowpass state*/
    double   x_dcx[MAXV], x_dcy[MAXV]; /* per-string in-loop DC blocker        */
    double   x_burst[MAXV];        /* per-string pluck burst envelope         */
    double   x_burstlp[MAXV];      /* per-string burst band-limit lowpass     */

    double   x_couple;             /* sympathetic feedback bus (last mix)     */

    t_float  x_macro;              /* inlet 1 float                           */
    int      x_trig;               /* set by bang, consumed in perform        */

    al_dc    dc;
    al_wander wnd;
    al_rng   rng;
} t_alien_pluck;

/* flush denormals and force finite; magnitude tests survive -ffast-math */
static inline double pl_flush(double v)
{
    if (!(v > -1e12 && v < 1e12)) return 0.0;   /* NaN/Inf/huge -> 0          */
    if (v > -1e-20 && v < 1e-20)  return 0.0;   /* denormal -> 0              */
    return v;
}

/* interpolated read from string `s`'s delay line, `d` samples behind write */
static inline double pl_delread(t_alien_pluck *x, int s, double d)
{
    double rp, fr, *buf = x->x_buf[s];
    int i0, i1;
    if (d < 1.0)        d = 1.0;
    if (d > DLEN - 2.0) d = DLEN - 2.0;
    rp = (double)x->x_wi[s] - d;
    while (rp < 0.0) rp += (double)DLEN;
    i0 = (int)rp;
    fr = rp - (double)i0;
    i1 = (i0 + 1) & DMASK;
    i0 &= DMASK;
    return buf[i0] * (1.0 - fr) + buf[i1] * fr;
}

/* ------------------------------------------------------------------ */

static void pluck_bang(t_alien_pluck *x) { x->x_trig = 1; }

static void pluck_macro(t_alien_pluck *x, t_floatarg f) { x->x_macro = f; }

/* a list of MIDI notes -> string frequencies */
static void pluck_notes(t_alien_pluck *x, t_symbol *s, int argc, t_atom *argv)
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

static t_int *alien_pluck_perform(t_int *w)
{
    t_alien_pluck *x = (t_alien_pluck *)(w[1]);
    t_sample *out = (t_sample *)(w[2]);
    int nsamp = (int)(w[3]);

    double sr = x->x_sr;
    int nv = x->x_nv, v;
    if (nv < 1) nv = 1;
    if (nv > MAXV) nv = MAXV;

    /* macro -> character (per block): clamp 0..100 (100 is the max, not a
     * wrap back to 0) so bright/long/ringing really is reachable */
    double t = x->x_macro;
    if (!(t == t)) t = 0.0;                       /* NaN guard               */
    if (t < 0.0)   t = 0.0;
    if (t > 100.0) t = 100.0;
    t *= 0.01;

    /* slow wander for subtle detune/damping drift */
    al_wander_tick(&x->wnd, &x->rng, 0.10 + 0.25 * t, sr);
    double wdmp = al_wander_bip(&x->wnd, 0);     /* damping drift            */
    double wdet = al_wander_bip(&x->wnd, 1);     /* detune drift            */

    /* macro shapes: dark/short/damped -> bright/long/ringing */
    double fb    = 0.9975 + 0.0017 * t;          /* 0.9975 .. 0.9992        */
    if (fb > 0.9992) fb = 0.9992;                /* strict < 1              */
    /* in-loop lowpass coeff: darker (more loss) at 0, brighter at 100,
     * always < 1 so it is always lossy and can never self-oscillate up */
    double damp  = 0.45 + 0.42 * t + 0.06 * wdmp;
    if (damp < 0.05) damp = 0.05;
    if (damp > 0.92) damp = 0.92;                /* always real HF loss     */
    double detune = 0.0006 + 0.004 * t + 0.002 * wdet;   /* string spread   */
    /* pluck burst length: short click at 0, longer/brighter at 100.  Kept
     * to a few ms so it excites the string harmonically, not a DC thump. */
    double burstms  = 2.0 + 8.0 * t;             /* burst length in ms      */
    double burstdec = exp(-1.0 / (sr * 0.001 * burstms));
    if (burstdec > 0.9995) burstdec = 0.9995;
    double burstlpc = 0.20 + 0.55 * t;           /* burst brightness        */
    /* sympathetic coupling: a whisper of the mix into every string so a
     * pluck stirs the others.  Tiny -- string-fb is already near unity, so
     * this must stay small enough that net loop gain stays below 1 and the
     * chord DECAYS rather than swelling. */
    double couple = 0.002 + 0.006 * t;

    double norm = 0.55 / sqrt((double)nv);       /* incoherent-sum scale    */

    /* consume a trigger: (re)pluck every string cleanly */
    if (x->x_trig) {
        x->x_trig = 0;
        for (v = 0; v < nv; v++) {
            x->x_burst[v]   = 1.0;               /* fire the excitation      */
            x->x_burstlp[v] = 0.0;
        }
    }

    while (nsamp--) {
        double mix = 0.0;
        double symp = x->x_couple * couple;      /* sympathetic feed        */

        for (v = 0; v < nv; v++) {
            double f = x->x_freq[v];
            /* clamp freq: mtof(MIDI 12..127) then clamp to [8, 0.45*sr] */
            if (!(f > 0.0)) f = 220.0;
            if (f < 8.0)        f = 8.0;
            if (f > 0.45 * sr)  f = 0.45 * sr;
            /* each string a hair off the exact ratio -> partials beat */
            double dl = sr / (f * (1.0 + detune * ((v & 1) ? 1.0 : -1.0) * (0.5 + 0.5 * v)));
            if (dl < 1.0)        dl = 1.0;
            if (dl > DLEN - 2.0) dl = DLEN - 2.0;

            double r = pl_delread(x, v, dl);

            /* Karplus-Strong damping lowpass inside the loop (always lossy) */
            double lp = x->x_damplp[v] + damp * (r - x->x_damplp[v]);
            x->x_damplp[v] = pl_flush(lp);

            /* in-loop DC blocker: DC passes the lowpass unchanged and would
             * otherwise accumulate as 1/(1-fb) and rail the string; strip it */
            double dcy = lp - x->x_dcx[v] + 0.9995 * x->x_dcy[v];
            x->x_dcx[v] = lp;
            x->x_dcy[v] = pl_flush(dcy);
            lp = dcy;

            /* pluck excitation: short band-limited noise burst */
            double wn = al_randb(&x->rng);
            double blp = x->x_burstlp[v] + burstlpc * (wn - x->x_burstlp[v]);
            x->x_burstlp[v] = pl_flush(blp);
            double exc = blp * x->x_burst[v];
            x->x_burst[v] *= burstdec;
            if (x->x_burst[v] < 1e-6) x->x_burst[v] = 0.0;

            /* write feed = excitation + damped feedback + sympathetic */
            double wr = exc * 3.0 + fb * lp + symp;
            x->x_buf[v][x->x_wi[v]] = pl_flush(wr);
            x->x_wi[v] = (x->x_wi[v] + 1) & DMASK;

            mix += r;
        }

        mix *= norm;
        x->x_couple = pl_flush(mix);             /* stash for next-sample fb */

        double y = al_dc_tick(&x->dc, mix);
        y = al_softclip(y);
        if (!(y > -1e6 && y < 1e6)) y = 0.0;
        if (y >  1.5) y =  1.5;
        if (y < -1.5) y = -1.5;
        *out++ = (t_sample)(y * 0.85);
    }

    /* keep long-lived feedback state clean between blocks */
    for (v = 0; v < nv; v++) {
        x->x_damplp[v] = pl_flush(x->x_damplp[v]);
        x->x_dcy[v]    = pl_flush(x->x_dcy[v]);
    }
    x->x_couple = pl_flush(x->x_couple);
    return (w + 4);
}

static void alien_pluck_dsp(t_alien_pluck *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr; if (x->x_sr < 1000) x->x_sr = 44100;
    dsp_add(alien_pluck_perform, 3, x, sp[0]->s_vec, (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_pluck_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_pluck *x = (t_alien_pluck *)pd_new(alien_pluck_tilde_class);
    int i;
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    x->x_nv = 1; x->x_freq[0] = al_mtof(52);
    for (i = 0; i < MAXV; i++) {
        memset(x->x_buf[i], 0, sizeof(x->x_buf[i]));
        x->x_wi[i] = 0;
        x->x_damplp[i] = 0.0;
        x->x_dcx[i] = 0.0;
        x->x_dcy[i] = 0.0;
        x->x_burst[i] = 0.0;
        x->x_burstlp[i] = 0.0;
        if (i > 0) x->x_freq[i] = al_mtof(52 + i);
    }
    x->x_couple = 0.0;
    x->x_macro = 50.0;
    x->x_trig = 0;
    al_dc_init(&x->dc);
    al_rng_seed(&x->rng, 0x9a2b17u);
    al_wander_init(&x->wnd, &x->rng, 4);

    /* inlet 1: float macro; inlet 2: list of notes */
    floatinlet_new(&x->x_obj, &x->x_macro);
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_list, gensym("notes"));
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_pluck_tilde_setup(void)
{
    alien_pluck_tilde_class = class_new(gensym("alien_pluck~"),
        (t_newmethod)alien_pluck_new, 0, sizeof(t_alien_pluck),
        CLASS_DEFAULT, A_GIMME, 0);
    class_addbang(alien_pluck_tilde_class, pluck_bang);
    class_addmethod(alien_pluck_tilde_class, (t_method)pluck_macro,
                    gensym("macro"), A_FLOAT, 0);
    class_addmethod(alien_pluck_tilde_class, (t_method)pluck_notes,
                    gensym("notes"), A_GIMME, 0);
    class_addmethod(alien_pluck_tilde_class, (t_method)alien_pluck_dsp,
                    gensym("dsp"), A_CANT, 0);
}
