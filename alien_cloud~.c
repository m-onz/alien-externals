/* alien_cloud~ -- a GRANULAR CHORD CLOUD (paraphonic granular pad)
 *
 * One signal path, a whole polyphonic cloud: give it a list of MIDI notes and
 * the object spins up a bank of overlapping windowed grains, each grain pitched
 * to a note picked at random from the current list (and sometimes shifted an
 * octave). The grains overlap continuously to form a shimmering, evolving
 * chord/pad texture -- a mono object that sounds like a stack of voices.
 *
 * This uses the note-list voice interface of the alien_*chord~ family:
 *
 *   inlet 0   bang            (re)seed / BURST the cloud -- fires a swell
 *                             envelope and re-scatters the grains (a shimmer
 *                             that blooms). The cloud also runs continuously,
 *                             so no bang is required to hear it.
 *   inlet 1   float           MACRO 0..100 -- sparse/soft/dark/narrow at 0 ->
 *                             dense/bright/wide/shimmering at 100
 *   inlet 2   list            NOTES, MIDI note numbers e.g.  50 55 58 62 65
 *                             (stored; grains pick from this set; mtof inside)
 *
 *   outlet 0  signal
 *
 * DESIGN: an internal bank of self-scheduled overlapping grains. When a grain
 * is (re)born it picks a random note from the list, occasionally an octave up
 * or down, adds a small detune for stereo-ish width, and plays a Hann-windowed
 * two-partial tone at that pitch. Density, grain size, pitch spread and detune
 * all scale with the macro. A swell envelope (fired by bang, and idling at a
 * gentle floor so the cloud is always audible) and a slow al_wander drift give
 * evolving motion; the grain sum runs a gentle resonant al_ladder, DC block and
 * softclip.
 *
 * SAFETY (build uses -ffast-math -> magnitude clamps, not isfinite): grain
 * count is clamped to capacity; every grain freq is clamped to [8, 0.45*sr];
 * grain amplitudes are normalised by 1/sqrt(capacity) so a dense cloud can't
 * rail; the ladder is state-guarded; an object-level guard
 * `if(!(s>-1e6 && s<1e6)) s=0;` then bounds to ~[-1.5,1.5] and scales ~0.8.
 */

#include "m_pd.h"
#include "alien.h"

#define MAXV     16          /* max simultaneous notes in the list      */
#define NGRAIN   24          /* grain-engine capacity (the cloud size)  */

static t_class *alien_cloud_tilde_class;

typedef struct _clgrain {
    int    act;
    double ph;               /* tone phase 0..1                         */
    double inc;              /* tone phase increment (freq/sr)          */
    double ph2;              /* detuned partial phase                   */
    double inc2;
    double wp;               /* window phase 0..1                       */
    double winc;             /* window increment                        */
    double amp;              /* per-grain amplitude                     */
} t_clgrain;

typedef struct _alien_cloud {
    t_object  x_obj;

    double    x_sr;
    int       x_nv;                 /* active notes in the list          */
    double    x_freq[MAXV];         /* note frequencies (Hz)             */

    t_float   x_macro;              /* inlet 1 float 0..100              */

    t_clgrain x_gr[NGRAIN];         /* the grain cloud                   */
    double    x_gclk;               /* grain scheduler clock             */

    double    x_swell;              /* swell envelope (bang blooms it)   */
    int       x_trig;               /* set by bang, consumed in perform  */

    al_ladder x_lad;
    al_dc     x_dc;
    al_wander x_wnd;
    al_rng    x_rng;

    double    x_msm;                /* smoothed macro (anti-zipper)      */
} t_alien_cloud;

/* ------------------------------------------------------------------ */

static void cloud_bang(t_alien_cloud *x) { x->x_trig = 1; }

static void cloud_macro(t_alien_cloud *x, t_floatarg f) { x->x_macro = f; }

/* a list of MIDI notes -> the note-frequency set the grains draw from */
static void cloud_notes(t_alien_cloud *x, t_symbol *s, int argc, t_atom *argv)
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

/* clamp a frequency into the safe audio band */
static double cloud_clampf(double f, double sr)
{
    if (!(f > 0.0)) f = 8.0;
    if (f < 8.0)        f = 8.0;
    if (f > sr * 0.45)  f = sr * 0.45;
    return f;
}

/* start one grain: pick a note, maybe an octave, add detune + window */
static void cloud_start_grain(t_alien_cloud *x, t_clgrain *g,
                              double spread, double detune, double durms,
                              double amp)
{
    double sr = x->x_sr;
    int    nv = x->x_nv > 0 ? x->x_nv : 1;
    int    k  = (int)(al_rand(&x->x_rng) * nv);
    double f, oct = 1.0, u;
    if (k < 0)   k = 0;
    if (k >= nv) k = nv - 1;
    f = x->x_freq[k];

    /* occasional octave shift widens the cloud as the macro opens up */
    u = al_rand(&x->x_rng);
    if      (u < 0.10 + 0.20 * spread) oct = 2.0;   /* octave up shimmer */
    else if (u > 0.92 - 0.12 * spread) oct = 0.5;   /* octave down body  */
    f *= oct;

    /* fine random pitch scatter (in ratio) for a lush, non-static cloud */
    f *= 1.0 + spread * 0.010 * al_randb(&x->x_rng);
    f  = cloud_clampf(f, sr);

    g->act  = 1;
    g->wp   = 0.0;
    g->winc = 1000.0 / (durms * sr);
    g->ph   = al_rand(&x->x_rng);
    g->ph2  = al_rand(&x->x_rng);
    g->inc  = f / sr;
    g->inc2 = f * (1.0 + detune) / sr;
    g->amp  = amp;
}

/* ------------------------------------------------------------------ */

static t_int *alien_cloud_perform(t_int *w)
{
    t_alien_cloud *x = (t_alien_cloud *)(w[1]);
    t_sample *out = (t_sample *)(w[2]);
    int nsamp = (int)(w[3]);

    double sr  = x->x_sr;
    double msm = x->x_msm;
    double smc = 1.0 - exp(-AL_2PI * 25.0 / sr);   /* macro smoothing     */
    int    i;

    /* macro target 0..1 */
    double pn = al_param100(x->x_macro);

    al_denorm_off();

    /* consume a bang: bloom the swell + re-scatter the whole cloud */
    if (x->x_trig) {
        x->x_trig = 0;
        x->x_swell = 1.0;
        for (i = 0; i < NGRAIN; i++) x->x_gr[i].act = 0;
        x->x_gclk = 0.99;   /* fire grains near-immediately after a bang  */
    }

    while (nsamp--) {
        double s = 0.0, win, gv;
        double density, durms, spread, detune, cutoff, res, gamp, swf;

        /* smooth the macro so a stepped float doesn't zipper */
        msm += smc * (pn - msm);
        if (!(msm >= 0.0 && msm <= 1.0)) msm = al_clamp(msm, 0.0, 1.0);

        /* slow wander brain for evolving motion */
        al_wander_tick(&x->x_wnd, &x->x_rng, 0.12 + 0.5 * msm, sr);
        {
            double wc = al_wander_bip(&x->x_wnd, 0);   /* cutoff drift    */
            double wd = al_wander_val(&x->x_wnd, 1);   /* density drift   */
            double we = al_wander_bip(&x->x_wnd, 2);   /* detune drift    */

            /* swell: bang blooms it to 1, it decays to a sustained floor so
             * the cloud is always alive but a bang gives an audible bloom */
            x->x_swell += (0.0 - x->x_swell) * (1.0 / (sr * 1.8));
            swf = 0.55 + 0.45 * x->x_swell;            /* 0.55 .. 1.0     */

            /* grains/sec, grain length, pitch spread, detune all track macro */
            density = 14.0 + (26.0 + 90.0 * msm) * (0.4 + 0.9 * wd);
            durms   = 70.0 + 110.0 * msm;               /* longer = smoother pad */
            spread  = msm;                              /* 0..1 pitch-scatter amt */
            detune  = 0.0015 + 0.010 * msm + 0.004 * we;
            /* normalise by the number of grains typically overlapping
             * (density * grain-length), not the full capacity, so a sparse
             * cloud isn't crushed -- clamped so a dense cloud can't rail */
            {
                double ov = density * durms * 0.001;    /* expected overlap */
                if (ov < 3.0)  ov = 3.0;
                if (ov > (double)NGRAIN) ov = (double)NGRAIN;
                gamp = 2.6 / sqrt(ov);
            }

            /* gentle resonant ladder: darker/narrower at 0, bright at 100 */
            cutoff = 200.0 * pow(2.0, 1.2 + 4.6 * msm + 0.6 * wc);
            res    = 0.10 + 0.42 * msm;
        }
        if (cutoff < 40.0)        cutoff = 40.0;
        if (cutoff > sr * 0.45)   cutoff = sr * 0.45;

        /* ---- schedule new grains ------------------------------------ */
        x->x_gclk += density / sr;
        if (x->x_gclk >= 1.0) {
            x->x_gclk -= floor(x->x_gclk);
            for (i = 0; i < NGRAIN; i++) if (!x->x_gr[i].act) {
                double dm = durms * (0.7 + 0.6 * al_rand(&x->x_rng));
                cloud_start_grain(x, &x->x_gr[i], spread, detune, dm, gamp);
                break;
            }
        }

        /* ---- run the cloud ------------------------------------------ */
        for (i = 0; i < NGRAIN; i++) if (x->x_gr[i].act) {
            t_clgrain *g = &x->x_gr[i];
            win = 0.5 - 0.5 * cos(AL_2PI * g->wp);      /* Hann window    */
            gv  = 0.6 * al_sin01(g->ph) + 0.4 * al_sin01(g->ph2);
            s  += g->amp * win * gv;
            g->ph  = al_wrap01(g->ph  + g->inc);
            g->ph2 = al_wrap01(g->ph2 + g->inc2);
            g->wp += g->winc;
            if (g->wp >= 1.0) g->act = 0;
        }

        s *= swf;
        s  = al_ladder_tick(&x->x_lad, s, cutoff, res, sr);
        s  = al_dc_tick(&x->x_dc, s);

        if (!(s > -1e6 && s < 1e6)) {                   /* object guard   */
            s = 0.0;
            al_ladder_init(&x->x_lad);
            for (i = 0; i < NGRAIN; i++) x->x_gr[i].act = 0;
        }
        s = al_softclip(s * 1.3);
        if (s >  1.5) s =  1.5; else if (s < -1.5) s = -1.5;
        *out++ = (t_sample)(s * 0.8);
    }

    x->x_msm = msm;
    return (w + 4);
}

static void alien_cloud_dsp(t_alien_cloud *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr; if (x->x_sr < 1000) x->x_sr = 44100.0;
    dsp_add(alien_cloud_perform, 3, x, sp[0]->s_vec, (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_cloud_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_cloud *x = (t_alien_cloud *)pd_new(alien_cloud_tilde_class);
    int i;
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100.0;

    x->x_nv = 1; x->x_freq[0] = al_mtof(50);
    x->x_macro = 50.0; x->x_msm = 0.0;
    x->x_gclk = 0.0; x->x_swell = 0.0; x->x_trig = 0;
    for (i = 0; i < NGRAIN; i++) x->x_gr[i].act = 0;

    al_ladder_init(&x->x_lad);
    al_dc_init(&x->x_dc);
    al_rng_seed(&x->x_rng, 0xc10ddau);
    al_wander_init(&x->x_wnd, &x->x_rng, 4);

    /* inlet 1: float macro; inlet 2: list of notes */
    floatinlet_new(&x->x_obj, &x->x_macro);
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_list, gensym("notes"));
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_cloud_tilde_setup(void)
{
    alien_cloud_tilde_class = class_new(gensym("alien_cloud~"),
        (t_newmethod)alien_cloud_new, 0, sizeof(t_alien_cloud),
        CLASS_DEFAULT, A_GIMME, 0);
    class_addbang(alien_cloud_tilde_class, cloud_bang);
    class_addmethod(alien_cloud_tilde_class, (t_method)cloud_macro,
                    gensym("macro"), A_FLOAT, 0);
    class_addmethod(alien_cloud_tilde_class, (t_method)cloud_notes,
                    gensym("notes"), A_GIMME, 0);
    class_addmethod(alien_cloud_tilde_class, (t_method)alien_cloud_dsp,
                    gensym("dsp"), A_CANT, 0);
}
