/* alien_modal~.c  --  MODAL physical-model resonator synthesis (note-list voice)
 *
 * A bang STRIKES a bank of tuned modal resonators -- one bank of ~4-8 modes per
 * note -- and the whole struck chord rings out: bells, plates, marimba, tubular
 * bells, drums. It's the Modalys / SuperCollider Klank idea packed into a single
 * mono object that voices POLYPHONY (paraphonic): give it a MIDI note list, bang
 * it, and every note's modes are excited at once by a short strike burst.
 *
 * Each mode is a high-Q 2-pole bandpass (al_bq / al_reson) tuned to material
 * ratios; higher modes decay faster (lower Q) as real struck bodies do. The
 * macro morphs the MATERIAL, the strike HARDNESS/BRIGHTNESS, the ring TIME and
 * the INHARMONICITY: soft dark short mallet on mellow wood at 0, hard bright
 * long strike on ringing metal / bell with more detune at 100. A slow wander
 * drifts the mode detune for a living shimmer.
 *
 *   inlet 0   bang            STRIKE -- inject an excitation burst into every
 *                             note's modes (they ring and decay, no note-off)
 *   inlet 1   float           MACRO 0..100 -- material / hardness / brightness /
 *                             ring-time / inharmonicity
 *   inlet 2   list            NOTES, MIDI note numbers e.g.  50 55 58 62 65
 *                             (stored; struck on the next bang; mtof internally)
 *
 *   outlet 0  signal
 *
 * build:
 *   (macOS: -bundle -undefined dynamic_lookup -o alien_modal~.pd_darwin ... )
 */

#include "m_pd.h"
#include "alien.h"

#define MAXV   12          /* max simultaneous notes                         */
#define NMODES 8           /* modes per note                                 */

static t_class *alien_modal_tilde_class;

/* material partial-ratio tables (index 0 = fundamental).  The macro morphs
 * (crossfades) between neighbouring materials, so the timbre glides. */
static const double MAT[5][NMODES] = {
    /* 0: harmonic-ish tube / mellow wood */
    { 1.00, 2.00, 3.01, 4.02, 5.03, 6.05, 7.07, 8.10 },
    /* 1: marimba / xylophone bar (odd, stretched) */
    { 1.00, 3.90, 9.20, 15.70, 22.0, 29.0, 37.0, 46.0 },
    /* 2: tubular bell */
    { 1.00, 2.76, 5.40, 8.93, 13.34, 18.64, 24.80, 31.87 },
    /* 3: inharmonic membrane / drum head */
    { 1.00, 1.59, 2.14, 2.30, 2.65, 2.92, 3.16, 3.50 },
    /* 4: metal plate / gong (dense inharmonic) */
    { 1.00, 2.39, 3.58, 4.83, 6.11, 7.42, 8.77, 10.15 }
};

typedef struct _alien_modal {
    t_object x_obj;

    double   x_sr;
    int      x_nv;                          /* active notes                  */
    double   x_freq[MAXV];                  /* note fundamentals (Hz)        */

    /* per-note, per-mode resonator state + strike energy */
    al_bq    x_mode[MAXV][NMODES];
    double   x_exc[MAXV];                   /* remaining strike burst energy */

    t_float  x_macro;                       /* inlet 1 float                 */
    int      x_trig;                        /* set by bang, consumed in dsp  */

    al_dc     dc;
    al_wander wnd;
    al_rng    rng;
} t_alien_modal;

/* ------------------------------------------------------------------ */

static void modal_bang(t_alien_modal *x) { x->x_trig = 1; }

static void modal_macro(t_alien_modal *x, t_floatarg f) { x->x_macro = f; }

/* a list of MIDI notes -> note fundamentals */
static void modal_notes(t_alien_modal *x, t_symbol *s, int argc, t_atom *argv)
{
    int i, n = 0;
    (void)s;
    for (i = 0; i < argc && n < MAXV; i++) {
        double m = atom_getfloat(argv + i);
        if (!(m > 0.0)) continue;                 /* skip 0 / junk          */
        x->x_freq[n] = al_mtof(m);
        n++;
    }
    if (n > 0) x->x_nv = n;
}

/* ------------------------------------------------------------------ */

static t_int *alien_modal_perform(t_int *w)
{
    t_alien_modal *x = (t_alien_modal *)(w[1]);
    t_sample *out = (t_sample *)(w[2]);
    int nsamp = (int)(w[3]);

    double sr = x->x_sr;
    int nv = x->x_nv, v, k;

    /* macro -> character (per block) */
    double t = x->x_macro; t = t - 100.0 * floor(t * 0.01);
    if (!(t >= 0.0 && t <= 100.0)) t = 0.0;
    t *= 0.01;

    /* material crossfade: 0..4 range over the macro */
    double mp   = t * 4.0;
    int    mi   = (int)mp; if (mi > 3) mi = 3; if (mi < 0) mi = 0;
    double mf   = mp - mi;
    if (!(mf >= 0.0 && mf <= 1.0)) mf = 0.0;

    /* slow wander for shimmer / detune drift */
    al_wander_tick(&x->wnd, &x->rng, 0.08 + 0.25 * t, sr);
    double wd = al_wander_bip(&x->wnd, 0);      /* detune drift            */

    /* strike character from macro:
     *  hardness   -> burst is shorter/brighter as macro rises
     *  ring time  -> fundamental Q climbs (longer sustain) with macro
     *  inharm     -> extra per-mode detune with macro
     *  bright     -> upper modes get more level as macro rises */
    double hard   = 0.15 + 0.80 * t;                 /* 0..1 strike hardness */
    double burstms = 2.2 - 1.6 * hard;               /* 0.6..2.2 ms burst    */
    double excdec = exp(-1.0 / (0.001 * burstms * sr)); /* per-sample decay  */

    double qbase  = 1500.0 + 4500.0 * t;             /* fundamental Q (ring) */
    double detune = 0.0015 + 0.010 * t + 0.006 * wd; /* inharmonic drift     */

    double norm = 0.9 / sqrt((double)(nv > 0 ? nv : 1));

    /* Q ceiling: al_reson pole radius ~ 1 - sin(w0)/(2Q); the per-mode qlim
     * below keeps radius strictly < 1 so this is just a sane upper bound. */
    double qmax = 8000.0;

    /* consume a trigger: inject a fresh strike burst into every note */
    if (x->x_trig) {
        x->x_trig = 0;
        for (v = 0; v < nv; v++) x->x_exc[v] = 1.0;
    }

    while (nsamp--) {
        double s = 0.0;

        for (v = 0; v < nv; v++) {
            double e = x->x_exc[v];
            if (e < 1e-7 && e > -1e-7) {
                /* still let modes ring: exc silent but resonators keep going.
                 * feed a tiny excitation of 0 so the biquads decay naturally. */
                e = 0.0;
            } else {
                x->x_exc[v] = e * excdec;
            }

            /* strike burst: a short exponentially-decaying click (impulse-like)
             * so the resonators pick their own partials cleanly instead of a
             * broadband hiss.  A small noise term adds strike texture; harder
             * strikes carry more noisy attack. */
            double drive = 0.0;
            if (e > 1e-7) {
                double nz = al_randb(&x->rng);
                drive = e * (1.0 + (0.10 + 0.30 * hard) * nz);
            }

            double f0 = x->x_freq[v];
            if (!(f0 > 8.0)) f0 = 8.0;
            if (f0 > sr * 0.45) f0 = sr * 0.45;

            double bank = 0.0;
            for (k = 0; k < NMODES; k++) {
                double ratio = MAT[mi][k] * (1.0 - mf) + MAT[mi + 1][k] * mf;
                /* per-mode inharmonic detune (grows with mode index + macro) */
                double fm = f0 * ratio * (1.0 + detune * (double)k * 0.5);
                if (!(fm > 0.0)) continue;
                if (fm > sr * 0.47) continue;        /* mode above Nyquist    */

                /* per-mode Q: higher modes decay faster (lower Q).             */
                double q = qbase / (1.0 + 0.9 * (double)k);
                if (q < 1.0) q = 1.0;
                if (q > qmax) q = qmax;
                /* pole-radius safety: al_reson pole radius ~ 1 - sin(w0)/(2Q).
                 * cap Q so radius stays < ~0.99998 (stable under -ffast-math). */
                double sw   = sin(AL_2PI * fm / sr);
                if (sw < 1e-6) sw = 1e-6;
                double qlim = sw / 0.00004;           /* radius <= ~0.99998    */
                if (q > qlim) q = qlim;
                if (q < 1.0) q = 1.0;

                /* al_reson is a CONSTANT-SKIRT bandpass: its impulse-response
                 * amplitude scales ~ sin(w0)/(2Q), so to get a consistent
                 * struck peak across pitches we scale the output by ~2Q/sin(w0).
                 * brightness: upper modes get more gain as the macro rises. */
                double resnorm = (2.0 * q / sw) * 0.010;
                double mg = resnorm / (1.0 + 0.35 * (double)k);   /* rolloff   */
                mg *= (0.55 + 0.85 * t) + 0.7 * t * ((double)k / (NMODES - 1));

                double y = al_reson(&x->x_mode[v][k], drive, fm, q, sr);
                /* denormal / finite guard on state */
                if (!(y > -1e6 && y < 1e6)) {
                    al_bq_init(&x->x_mode[v][k]);
                    y = 0.0;
                }
                bank += y * mg;
            }
            s += bank;
        }

        s *= norm;

        double y = al_dc_tick(&x->dc, s);
        if (!(y > -1e6 && y < 1e6)) y = 0.0;
        y = al_softclip(y);
        if (y > 1.5) y = 1.5; else if (y < -1.5) y = -1.5;
        *out++ = (t_sample)y;
    }
    return (w + 4);
}

static void alien_modal_dsp(t_alien_modal *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr; if (x->x_sr < 1000) x->x_sr = 44100;
    dsp_add(alien_modal_perform, 3, x, sp[0]->s_vec, (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_modal_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_modal *x = (t_alien_modal *)pd_new(alien_modal_tilde_class);
    int v, k;
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    x->x_nv = 1; x->x_freq[0] = al_mtof(50);
    for (v = 0; v < MAXV; v++) {
        x->x_exc[v] = 0.0;
        for (k = 0; k < NMODES; k++) al_bq_init(&x->x_mode[v][k]);
    }
    x->x_macro = 50.0;
    x->x_trig  = 0;
    al_dc_init(&x->dc);
    al_rng_seed(&x->rng, 0x30da1u);
    al_wander_init(&x->wnd, &x->rng, 4);

    /* inlet 1: float macro; inlet 2: list of notes */
    floatinlet_new(&x->x_obj, &x->x_macro);
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_list, gensym("notes"));
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_modal_tilde_setup(void)
{
    alien_modal_tilde_class = class_new(gensym("alien_modal~"),
        (t_newmethod)alien_modal_new, 0, sizeof(t_alien_modal),
        CLASS_DEFAULT, A_GIMME, 0);
    class_addbang(alien_modal_tilde_class, modal_bang);
    class_addmethod(alien_modal_tilde_class, (t_method)modal_macro,
                    gensym("macro"), A_FLOAT, 0);
    class_addmethod(alien_modal_tilde_class, (t_method)modal_notes,
                    gensym("notes"), A_GIMME, 0);
    class_addmethod(alien_modal_tilde_class, (t_method)alien_modal_dsp,
                    gensym("dsp"), A_CANT, 0);
}
