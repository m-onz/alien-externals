/* alien_hybrid~.c  --  a MULTI-ENGINE HYBRID chord (paraphonic note-list voice)
 *
 * The Serum-2 / Pigments idea in one Pd object: several distinct synthesis
 * ENGINES live under a single macro, and each chord note crossfades across all
 * of them as the knob sweeps. One float therefore tours the whole chord from
 * ANALOG -> WAVETABLE -> FM/BELL -> GRANULAR and loops back toward analog, so a
 * single gesture morphs the *timbre* of the entire stack.
 *
 * Per chord note four lightweight engines run in parallel and are bilinearly
 * crossfaded so only ~2 are ever audible at once (cheap):
 *   A  virtual-analog : detuned band-limited saws (al_saw_bl, supersaw-ish)
 *   B  wavetable      : morphing single-cycle table sine->saw->formant, the
 *                       macro also scans read position across the frame
 *   C  FM / bell      : al_fm2 metallic/glassy 2-op FM
 *   D  granular-ish   : inharmonic partial cloud through a wavefolder (al_fold)
 * A shared resonant ladder, an AR envelope and a slow al_wander give life.
 *
 * Note-list interface (same family as alien_chord~):
 *   inlet 0   bang     TRIGGER -- (re)voice the current notes, fire the envelope
 *   inlet 1   float    MACRO 0..100 -- engine morph A->B->C->D->(A)
 *   inlet 2   list     NOTES, MIDI note numbers e.g.  50 55 58 62 65
 *   outlet 0  signal
 *
 * build:
 *   make alien_hybrid~.pd_darwin
 */

#include "m_pd.h"
#include "alien.h"

#define MAXV  8           /* max simultaneous notes                       */
#define NPART 5           /* inharmonic partials in the granular engine   */

static t_class *alien_hybrid_tilde_class;

typedef struct _alien_hybrid {
    t_object x_obj;

    double   x_sr;
    int      x_nv;                    /* active voices                     */
    double   x_freq[MAXV];            /* voice frequencies (Hz)            */

    /* per-voice phases for the four engines */
    double   x_pa1[MAXV], x_pa2[MAXV];      /* analog: two detuned saws     */
    double   x_pwt[MAXV];                   /* wavetable read phase         */
    double   x_ppart[MAXV][NPART];          /* granular partial phases      */
    al_fm2   x_fm[MAXV];                    /* FM operator state            */

    t_float  x_macro;                 /* inlet 1 float                     */

    /* AR envelope */
    int      x_seg;                   /* 0 idle, 1 attack, 2 decay         */
    double   x_env;
    int      x_trig;

    al_ladder lad;
    al_dc     dc;
    al_wander wnd;
    al_rng    rng;
} t_alien_hybrid;

/* ------------------------------------------------------------------ */
/* three-frame single-cycle wavetable: sine -> saw -> formant, sampled by a
 * fractional read phase p (0..1) and a morph position m (0..1). Everything is
 * synthesised on the fly (no allocation) and always in-bounds. */
static inline double hybrid_wt(double p, double m)
{
    double s, saw, form;
    p -= floor(p);
    if (!(m >= 0.0 && m <= 1.0)) m = 0.0;
    s   = sin(AL_2PI * p);                          /* frame 0: sine        */
    saw = 2.0 * p - 1.0;                             /* frame 1: saw         */
    /* frame 2: a cheap formant-ish stack (odd + shifted partials)          */
    form = 0.6 * sin(AL_2PI * p)
         + 0.5 * sin(AL_2PI * 3.0 * p)
         + 0.4 * sin(AL_2PI * 4.5 * p);
    if (m < 0.5) return al_lerp(s, saw, m * 2.0);
    return al_lerp(saw, form, (m - 0.5) * 2.0);
}

/* ------------------------------------------------------------------ */

static void hybrid_bang(t_alien_hybrid *x) { x->x_trig = 1; }

static void hybrid_macro(t_alien_hybrid *x, t_floatarg f) { x->x_macro = f; }

/* a list of MIDI notes -> voice frequencies */
static void hybrid_notes(t_alien_hybrid *x, t_symbol *s, int argc, t_atom *argv)
{
    int i, n = 0;
    (void)s;
    for (i = 0; i < argc && n < MAXV; i++) {
        double m = atom_getfloat(argv + i);
        if (!(m > 0.0)) continue;                    /* skip 0 / junk       */
        x->x_freq[n] = al_mtof(m);
        n++;
    }
    if (n > 0) x->x_nv = n;
}

/* ------------------------------------------------------------------ */

static t_int *alien_hybrid_perform(t_int *w)
{
    t_alien_hybrid *x = (t_alien_hybrid *)(w[1]);
    t_sample *out = (t_sample *)(w[2]);
    int nsamp = (int)(w[3]);

    double sr = x->x_sr;
    int nv = x->x_nv, v, k;
    double fnyq = 0.45 * sr;

    /* macro -> morph position 0..1 (per block) */
    double t = x->x_macro; t = t - 100.0 * floor(t * 0.01);
    if (!(t >= 0.0 && t <= 100.0)) t = 0.0;
    t *= 0.01;

    /* slow wander for life */
    al_wander_tick(&x->wnd, &x->rng, 0.2 + 0.5 * t, sr);
    double wc = al_wander_bip(&x->wnd, 0);           /* cutoff drift        */
    double wd = al_wander_val(&x->wnd, 1);           /* detune drift        */
    double wm = al_wander_bip(&x->wnd, 2);           /* morph drift         */

    /* -------- engine crossfade weights --------------------------------- *
     * one macro tours 4 engines placed at 0, 1/3, 2/3, 1 and loops back to
     * analog past the end. Only the two nearest engines are ever nonzero, so
     * CPU stays near two-engine cost. `mpos` is the position along the tour. */
    double mpos = t * 4.0;                            /* 0..4 (4 == back to A) */
    if (mpos > 4.0) mpos = 4.0;
    double wA = 0.0, wB = 0.0, wC = 0.0, wD = 0.0;
    {
        /* engine anchors: A=0, B=1, C=2, D=3, A(again)=4 */
        double seg = floor(mpos); double fr = mpos - seg;
        int s0 = (int)seg;
        double we0 = 1.0 - fr, we1 = fr;
        switch (s0) {
            case 0: wA = we0; wB = we1; break;
            case 1: wB = we0; wC = we1; break;
            case 2: wC = we0; wD = we1; break;
            case 3: wD = we0; wA = we1; break;   /* morph back toward analog */
            default: wA = 1.0; break;            /* mpos == 4                */
        }
    }

    /* wavetable morph position: macro scans frames, wander adds motion */
    double wtmorph = al_clamp(t + 0.15 * wm, 0.0, 1.0);
    /* wavetable read-position scan (formant-ish sweep) */
    double wtscan  = al_clamp(0.15 + 0.7 * t, 0.0, 0.95);

    /* FM parameters (bounded for -ffast-math safety) */
    double fm_ratio = 2.0 + 1.01 * t;                 /* 2.0..3.01           */
    double fm_index = al_clamp(0.5 + 3.0 * t, 0.0, 4.0);
    double fm_fb    = al_clamp(0.1 + 0.30 * t, 0.0, 0.5);

    /* granular/additive fold amount */
    double gr_fold  = al_clamp(0.12 + 0.28 * t, 0.0, 0.5);

    double detune = 0.002 + 0.02 * t + 0.008 * wd;
    double cutoff = 300.0 * pow(2.0, 2.4 + 2.6 * t + 0.5 * wc);
    double res    = 0.12 + 0.45 * t;
    double dectime = 1.8 + 3.5 * t;
    double drive  = 0.12 + 0.30 * t;
    double atkc   = 1.0 - exp(-1.0 / (sr * 0.008));   /* ~8ms attack         */
    double decc   = exp(-1.0 / (sr * dectime));
    double norm   = 1.15 / sqrt((double)(nv > 0 ? nv : 1));

    if (cutoff < 40.0) cutoff = 40.0;
    if (cutoff > fnyq) cutoff = fnyq;

    /* inharmonic partial ratios (stretched, glassy) for the granular engine */
    static const double partr[NPART] = { 1.0, 2.76, 4.07, 5.41, 6.83 };

    /* consume a trigger: reset phases + start the attack */
    if (x->x_trig) {
        x->x_trig = 0;
        x->x_seg = 1;
        for (v = 0; v < nv; v++) {
            x->x_pa1[v] = 0.0; x->x_pa2[v] = 0.37;
            x->x_pwt[v] = 0.0;
            for (k = 0; k < NPART; k++) x->x_ppart[v][k] = 0.11 * k;
        }
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
                if (f < 8.0) f = 8.0; if (f > fnyq) f = fnyq;
                double d1 = f / sr;
                double vs = 0.0;

                /* --- ENGINE A : virtual-analog detuned saws --------------- */
                if (wA > 0.0) {
                    double fa2 = f * (1.0 + detune);
                    double d2 = fa2 / sr;
                    x->x_pa1[v] = al_wrap01(x->x_pa1[v] + d1);
                    x->x_pa2[v] = al_wrap01(x->x_pa2[v] + d2);
                    double a = 0.5 * (al_saw_bl(x->x_pa1[v], d1)
                                    + al_saw_bl(x->x_pa2[v], d2));
                    vs += wA * a;
                }

                /* --- ENGINE B : morphing wavetable ----------------------- */
                if (wB > 0.0) {
                    x->x_pwt[v] = al_wrap01(x->x_pwt[v] + d1);
                    double rp = x->x_pwt[v] + wtscan;
                    double b = hybrid_wt(rp, wtmorph);
                    vs += wB * b;
                }

                /* --- ENGINE C : FM / bell -------------------------------- */
                if (wC > 0.0) {
                    double c = al_fm2_tick(&x->x_fm[v], f, fm_ratio,
                                           fm_index, fm_fb, sr);
                    vs += wC * c;
                } else {
                    /* keep FM phase running so re-entry has no click */
                    (void)al_fm2_tick(&x->x_fm[v], f, fm_ratio, 0.0, 0.0, sr);
                }

                /* --- ENGINE D : granular / inharmonic partial cloud ------ */
                if (wD > 0.0) {
                    double g = 0.0; double gamp = 1.0;
                    for (k = 0; k < NPART; k++) {
                        double fp = f * partr[k];
                        if (fp > fnyq) { continue; }   /* skip aliasing tops */
                        x->x_ppart[v][k] = al_wrap01(x->x_ppart[v][k] + fp / sr);
                        g += gamp * sin(AL_2PI * x->x_ppart[v][k]);
                        gamp *= 0.7;
                    }
                    g *= 0.5;
                    g = al_fold(g, gr_fold);           /* bright shimmer     */
                    vs += wD * g;
                }

                s += vs;
            }
            s *= norm;
            s = al_ladder_tick(&x->lad, s, cutoff, res, sr);
            s *= env;
            s = al_drive(s, drive);
        }

        y = al_dc_tick(&x->dc, s);
        if (!(y > -1e6 && y < 1e6)) y = 0.0;
        y = al_softclip(y);
        if (y >  1.5) y =  1.5;
        if (y < -1.5) y = -1.5;
        *out++ = (t_sample)(y * 0.82);
    }
    return (w + 4);
}

static void alien_hybrid_dsp(t_alien_hybrid *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr; if (x->x_sr < 1000) x->x_sr = 44100;
    dsp_add(alien_hybrid_perform, 3, x, sp[0]->s_vec, (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_hybrid_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_hybrid *x = (t_alien_hybrid *)pd_new(alien_hybrid_tilde_class);
    int i, k;
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    x->x_nv = 1; x->x_freq[0] = al_mtof(50);
    for (i = 0; i < MAXV; i++) {
        x->x_pa1[i] = 0.0; x->x_pa2[i] = 0.37; x->x_pwt[i] = 0.0;
        for (k = 0; k < NPART; k++) x->x_ppart[i][k] = 0.11 * k;
        al_fm2_init(&x->x_fm[i]);
    }
    x->x_macro = 50.0;
    x->x_seg = 0; x->x_env = 0.0; x->x_trig = 0;
    al_ladder_init(&x->lad); al_dc_init(&x->dc);
    al_rng_seed(&x->rng, 0xa11e7u); al_wander_init(&x->wnd, &x->rng, 4);

    /* inlet 1: float macro; inlet 2: list of notes */
    floatinlet_new(&x->x_obj, &x->x_macro);
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_list, gensym("notes"));
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_hybrid_tilde_setup(void)
{
    alien_hybrid_tilde_class = class_new(gensym("alien_hybrid~"),
        (t_newmethod)alien_hybrid_new, 0, sizeof(t_alien_hybrid),
        CLASS_DEFAULT, A_GIMME, 0);
    class_addbang(alien_hybrid_tilde_class, hybrid_bang);
    class_addmethod(alien_hybrid_tilde_class, (t_method)hybrid_macro,
                    gensym("macro"), A_FLOAT, 0);
    class_addmethod(alien_hybrid_tilde_class, (t_method)hybrid_notes,
                    gensym("notes"), A_GIMME, 0);
    class_addmethod(alien_hybrid_tilde_class, (t_method)alien_hybrid_dsp,
                    gensym("dsp"), A_CANT, 0);
}
