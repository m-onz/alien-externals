/* alien_wgnet~.c  --  WAVEGUIDE NETWORK / COMMUTED SYNTHESIS chord (mono object)
 *
 * A note-list voice built around Julius Smith's commuted-synthesis idea: a bang
 * plucks a set of tuned string waveguides (one per note) that ALL feed and are
 * coupled through a SHARED resonant BODY (a small bank of body modes -- the
 * soundboard / guitar-body).  The strings drive the body, and the body couples
 * energy back into EVERY string, so plucking one note excites a sympathetic
 * response in the others.  That shared body "commutes" its resonance into a
 * rich plucked-instrument chord -- distinct from alien_pluck~, whose strings
 * are effectively independent; here the coupling BODY is the whole point.
 *
 * Same note-list voice interface as alien_chord~ / alien_pluck~ (control-only
 * DSP, no signal inlets):
 *
 *   inlet 0   bang            TRIGGER -- (re)pluck every string in the list
 *   inlet 1   float           MACRO 0..100 -- dark/short/tight body + little
 *                             coupling at 0 -> bright/long/resonant body + more
 *                             coupling (more sympathetic bloom) + brighter
 *                             strings at 100
 *   inlet 2   list            NOTES, MIDI note numbers e.g. 40 47 52 55 59 64
 *                             (stored; plucked on the next bang; mtof internally)
 *
 *   outlet 0  signal
 *
 * SAFETY: the TOTAL loop gain around string->body->string is kept strictly < 1
 * (string feedback AND the body->string coupling are both bounded so the
 * coupled network can never self-oscillate to infinity); every in-loop lowpass
 * is always lossy; delay indices clamped in [1, DLEN-2]; body resonators are
 * stable bandpasses with bounded gain; freq clamped to [8, 0.45*sr]; all state
 * denormal-flushed + finite-guarded (magnitude tests survive -ffast-math);
 * output finite-guarded and bounded to ~[-1.5,1.5].
 *
 * build:
 *   (macOS: -bundle -undefined dynamic_lookup -o alien_wgnet~.pd_darwin ... )
 */

#include "m_pd.h"
#include "alien.h"

#define MAXV  10         /* max simultaneous string waveguides                */
#define DLEN  4096       /* per-string delay line (~92ms @ 44.1k -> ~10.7 Hz) */
#define DMASK (DLEN - 1)
#define NBODY 4          /* shared body resonant modes                        */

static t_class *alien_wgnet_tilde_class;

typedef struct _alien_wgnet {
    t_object x_obj;

    double   x_sr;
    int      x_nv;                     /* active strings                      */
    double   x_freq[MAXV];             /* string frequencies (Hz)             */

    /* per-string waveguides (tuned delay + in-loop damping lowpass) */
    double   x_buf[MAXV][DLEN];
    int      x_wi[MAXV];               /* per-string write index              */
    double   x_damplp[MAXV];           /* per-string in-loop damping lowpass  */
    double   x_dcx[MAXV], x_dcy[MAXV]; /* per-string in-loop DC blocker       */
    double   x_burst[MAXV];            /* per-string pluck burst envelope     */
    double   x_burstlp[MAXV];          /* per-string burst band-limit lowpass */

    /* shared coupling BODY: a small bank of resonant bandpasses (body modes) */
    al_bq    x_body[NBODY];            /* body mode resonators                */
    double   x_bodyf[NBODY];           /* body mode frequencies (Hz)          */
    double   x_bodyout;                /* last body output (coupling bus)     */

    t_float  x_macro;                  /* inlet 1 float                       */
    int      x_trig;                   /* set by bang, consumed in perform    */

    al_dc    dc;
    al_wander wnd;
    al_rng   rng;
} t_alien_wgnet;

/* flush denormals and force finite; magnitude tests survive -ffast-math */
static inline double wg_flush(double v)
{
    if (!(v > -1e12 && v < 1e12)) return 0.0;   /* NaN/Inf/huge -> 0          */
    if (v > -1e-20 && v < 1e-20)  return 0.0;   /* denormal -> 0              */
    return v;
}

/* interpolated read from string `s`'s delay line, `d` samples behind write */
static inline double wg_delread(t_alien_wgnet *x, int s, double d)
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

static void wgnet_bang(t_alien_wgnet *x) { x->x_trig = 1; }

static void wgnet_macro(t_alien_wgnet *x, t_floatarg f) { x->x_macro = f; }

/* a list of MIDI notes -> string frequencies */
static void wgnet_notes(t_alien_wgnet *x, t_symbol *s, int argc, t_atom *argv)
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

static t_int *alien_wgnet_perform(t_int *w)
{
    t_alien_wgnet *x = (t_alien_wgnet *)(w[1]);
    t_sample *out = (t_sample *)(w[2]);
    int nsamp = (int)(w[3]);

    double sr = x->x_sr;
    int nv = x->x_nv, v, b;
    if (nv < 1) nv = 1;
    if (nv > MAXV) nv = MAXV;

    /* macro -> character (per block): clamp 0..100 (100 is the max, not a
     * wrap back to 0) so bright/long/resonant really is reachable */
    double t = x->x_macro;
    if (!(t == t)) t = 0.0;                        /* NaN guard              */
    if (t < 0.0)   t = 0.0;
    if (t > 100.0) t = 100.0;
    t *= 0.01;

    /* slow wander for subtle string detune + body drift (shimmer) */
    al_wander_tick(&x->wnd, &x->rng, 0.10 + 0.25 * t, sr);
    double wdmp = al_wander_bip(&x->wnd, 0);      /* damping drift           */
    double wdet = al_wander_bip(&x->wnd, 1);      /* detune drift            */
    double wbod = al_wander_bip(&x->wnd, 2);      /* body-freq drift         */

    /* macro shapes: dark/short/damped -> bright/long/ringing strings */
    double fb    = 0.9980 + 0.0008 * t;           /* 0.9980 .. 0.9988        */
    if (fb > 0.9988) fb = 0.9988;                 /* strict < 1              */
    double damp  = 0.55 + 0.31 * t + 0.06 * wdmp; /* in-loop LP: darker->bright */
    if (damp < 0.05) damp = 0.05;
    if (damp > 0.92) damp = 0.92;                 /* always real HF loss     */
    double detune = 0.0006 + 0.004 * t + 0.002 * wdet;   /* string spread    */

    /* pluck burst: short click at 0, longer/brighter at 100 */
    double burstms  = 2.0 + 8.0 * t;
    double burstdec = exp(-1.0 / (sr * 0.001 * burstms));
    if (burstdec > 0.9995) burstdec = 0.9995;
    double burstlpc = 0.20 + 0.55 * t;            /* burst brightness        */

    /* SHARED BODY: dark/short/tight -> bright/long/resonant.  The body is a
     * bank of bandpasses; higher macro raises Q (longer ring) and brightens
     * the top mode.  Body gain is bounded so the coupling loop stays < 1. */
    double bodyq   = 3.0 + 22.0 * t;              /* mode Q: tight -> resonant*/
    /* body input gain: how hard the summed strings drive the body */
    double bodydrv = 0.5 + 0.5 * t;
    /* COUPLING: body output fed back into every string.  This closes the
     * string->body->string loop, so it MUST stay tiny: string fb is already
     * near unity, so the extra path gain has to keep the TOTAL loop gain < 1.
     * Grows with macro for more sympathetic bloom, but hard-capped. */
    double couple = 0.0015 + 0.0060 * t;          /* 0.0015 .. 0.0075        */
    if (couple > 0.0080) couple = 0.0080;         /* hard cap                */

    double norm = 0.75 / sqrt((double)nv);        /* incoherent-sum scale    */

    /* consume a trigger: (re)pluck every string cleanly */
    if (x->x_trig) {
        x->x_trig = 0;
        for (v = 0; v < nv; v++) {
            x->x_burst[v]   = 1.0;                /* fire the excitation      */
            x->x_burstlp[v] = 0.0;
        }
    }

    while (nsamp--) {
        double strsum = 0.0;                      /* strings -> body drive    */
        double symp   = x->x_bodyout * couple;    /* body -> every string     */

        for (v = 0; v < nv; v++) {
            double f = x->x_freq[v];
            if (!(f > 0.0))    f = 220.0;
            if (f < 8.0)       f = 8.0;
            if (f > 0.45 * sr) f = 0.45 * sr;
            /* each string a hair off exact ratio -> partials beat/shimmer */
            double dl = sr / (f * (1.0 + detune * ((v & 1) ? 1.0 : -1.0) * (0.5 + 0.5 * v)));
            if (dl < 1.0)        dl = 1.0;
            if (dl > DLEN - 2.0) dl = DLEN - 2.0;

            double r = wg_delread(x, v, dl);

            /* in-loop damping lowpass (always lossy) */
            double lp = x->x_damplp[v] + damp * (r - x->x_damplp[v]);
            x->x_damplp[v] = wg_flush(lp);

            /* in-loop DC blocker: DC through the LP would accumulate as
             * 1/(1-fb) and rail the string; strip it */
            double dcy = lp - x->x_dcx[v] + 0.9995 * x->x_dcy[v];
            x->x_dcx[v] = lp;
            x->x_dcy[v] = wg_flush(dcy);
            lp = dcy;

            /* pluck excitation: short band-limited noise burst */
            double wn = al_randb(&x->rng);
            double blp = x->x_burstlp[v] + burstlpc * (wn - x->x_burstlp[v]);
            x->x_burstlp[v] = wg_flush(blp);
            double exc = blp * x->x_burst[v];
            x->x_burst[v] *= burstdec;
            if (x->x_burst[v] < 1e-6) x->x_burst[v] = 0.0;

            /* write = excitation + damped feedback + sympathetic body coupling */
            double wr = exc * 4.5 + fb * lp + symp;
            x->x_buf[v][x->x_wi[v]] = wg_flush(wr);
            x->x_wi[v] = (x->x_wi[v] + 1) & DMASK;

            strsum += r;
        }

        strsum *= norm;

        /* drive the SHARED BODY: sum of strings through the body mode bank.
         * Sum the (bounded-gain) bandpasses -> commuted body resonance. */
        double bin = strsum * bodydrv;
        double body = 0.0;
        for (b = 0; b < NBODY; b++) {
            double bf = x->x_bodyf[b] * (1.0 + 0.01 * wbod);
            /* normalise each mode so the summed body gain stays bounded */
            double bm = al_reson(&x->x_body[b], bin, bf, bodyq, sr);
            body += bm;
        }
        body *= (0.5 / (double)NBODY);            /* bounded body gain        */
        body = wg_flush(body);
        x->x_bodyout = body;                      /* coupling bus (next samp) */

        /* final mix: strings + a helping of the commuted body */
        double s = strsum + 0.9 * body;

        double y = al_dc_tick(&x->dc, s);
        y = al_softclip(y);
        if (!(y > -1e6 && y < 1e6)) y = 0.0;
        if (y >  1.5) y =  1.5;
        if (y < -1.5) y = -1.5;
        *out++ = (t_sample)(y * 0.85);
    }

    /* keep long-lived feedback state clean between blocks */
    for (v = 0; v < nv; v++) {
        x->x_damplp[v] = wg_flush(x->x_damplp[v]);
        x->x_dcy[v]    = wg_flush(x->x_dcy[v]);
    }
    x->x_bodyout = wg_flush(x->x_bodyout);
    return (w + 4);
}

static void alien_wgnet_dsp(t_alien_wgnet *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr; if (x->x_sr < 1000) x->x_sr = 44100;
    dsp_add(alien_wgnet_perform, 3, x, sp[0]->s_vec, (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_wgnet_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_wgnet *x = (t_alien_wgnet *)pd_new(alien_wgnet_tilde_class);
    int i;
    /* fixed body mode frequencies (a small guitar-body-ish mode set) */
    static const double bodyf[NBODY] = { 110.0, 220.0, 400.0, 720.0 };
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
    for (i = 0; i < NBODY; i++) {
        al_bq_init(&x->x_body[i]);
        x->x_bodyf[i] = bodyf[i];
    }
    x->x_bodyout = 0.0;
    x->x_macro = 50.0;
    x->x_trig = 0;
    al_dc_init(&x->dc);
    al_rng_seed(&x->rng, 0x7c3d91u);
    al_wander_init(&x->wnd, &x->rng, 4);

    /* inlet 1: float macro; inlet 2: list of notes */
    floatinlet_new(&x->x_obj, &x->x_macro);
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_list, gensym("notes"));
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_wgnet_tilde_setup(void)
{
    alien_wgnet_tilde_class = class_new(gensym("alien_wgnet~"),
        (t_newmethod)alien_wgnet_new, 0, sizeof(t_alien_wgnet),
        CLASS_DEFAULT, A_GIMME, 0);
    class_addbang(alien_wgnet_tilde_class, wgnet_bang);
    class_addmethod(alien_wgnet_tilde_class, (t_method)wgnet_macro,
                    gensym("macro"), A_FLOAT, 0);
    class_addmethod(alien_wgnet_tilde_class, (t_method)wgnet_notes,
                    gensym("notes"), A_GIMME, 0);
    class_addmethod(alien_wgnet_tilde_class, (t_method)alien_wgnet_dsp,
                    gensym("dsp"), A_CANT, 0);
}
