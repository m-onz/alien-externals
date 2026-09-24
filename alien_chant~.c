/* alien_chant~.c  --  a monophonic object that sings a CHOIR
 *
 * FOF / CHANT vocal synthesis (Fonction d'Onde Formantique, Rodet / IRCAM).
 * Each MIDI note in the list becomes a synthetic VOICE built from formant
 * wave functions: an impulse train at the note's fundamental fires little
 * windowed, damped sinusoids -- one grain per formant per period -- at a
 * vowel's formant centre frequencies.  A formant made this way stays put
 * while the pitch moves, which is exactly what a singing voice does and
 * what a filter bank only approximates.
 *
 * Give it a chord and it is not one voice with harmony, it is a CHOIR:
 * every note is its own singer, each with a slightly different humanised
 * vibrato, micro-detune and vowel drift, so the stack sounds like many
 * throats rather than clones.  One mono object -> a section of singers.
 *
 * This shares the note-list voice interface of the alien_*chord~ family:
 *
 *   inlet 0   bang            TRIGGER -- (re)attack the choir's amp envelope
 *                             (a fresh breath / swell; no note-off needed)
 *   inlet 1   float           MACRO 0..100 -- morphs the whole choir: a soft,
 *                             dark, breathless "ooo" pad at 0, opening the
 *                             vowel u -> o -> a -> e -> i and adding vibrato,
 *                             breath and brightness toward a bright "aaa/eee"
 *                             choir at 100
 *   inlet 2   list            NOTES, MIDI note numbers e.g. 40 47 52 55 59 64
 *                             (stored; sung on the next bang; mtof internally)
 *
 *   outlet 0  signal
 *
 * build:
 *   (macOS: -bundle -undefined dynamic_lookup -o alien_chant~.pd_darwin ... )
 */

#include "m_pd.h"
#include "alien.h"

#define MAXV   8            /* max simultaneous voices (singers)          */
#define NFORM  5            /* formants per voice                         */
#define NGRAIN 6            /* overlapping FOF grains per formant         */

static t_class *alien_chant_tilde_class;

typedef struct _cgrain {
    double t;                  /* seconds since onset                     */
    double amp;
    int    active;
} t_cgrain;

typedef struct _cvoice {
    double   f0;               /* fundamental (Hz)                        */
    double   ph;               /* impulse-train phase                     */
    double   vibph;            /* vibrato LFO phase                       */
    double   driftph;          /* slow vowel/character drift phase        */
    double   viboff;           /* per-voice vibrato phase offset          */
    double   detune;           /* per-voice static micro-detune           */
    double   vowoff;           /* per-voice vowel offset (humanise)       */
    t_cgrain g[NFORM][NGRAIN];
    int      next[NFORM];
} t_cvoice;

typedef struct _alien_chant {
    t_object x_obj;

    double   x_sr;
    int      x_nv;                 /* active voices                       */
    t_cvoice x_v[MAXV];

    t_float  x_macro;              /* inlet 1 float                       */

    /* amp envelope (breath swell) */
    int      x_seg;                /* 0 idle, 1 attack, 2 sustain/decay   */
    double   x_env;
    int      x_trig;               /* set by bang, consumed in perform    */

    al_dc    dc;
    al_rng   rng;
} t_alien_chant;

/* five vowels: formant centre freq / amplitude / bandwidth (Hz), a e i o u */
static const double CV_F[5][NFORM] = {
    {600,1040,2250,2450,2750},   /* a */
    {400,1620,2400,2800,3100},   /* e */
    {250,1750,2600,3050,3340},   /* i */
    {400, 750,2400,2600,2900},   /* o */
    {350, 600,2400,2675,2950}    /* u */
};
static const double CV_A[5][NFORM] = {
    {1.0,0.50,0.40,0.32,0.14},
    {1.0,0.30,0.38,0.28,0.14},
    {1.0,0.06,0.20,0.12,0.06},
    {1.0,0.30,0.10,0.09,0.03},
    {1.0,0.12,0.20,0.10,0.05}
};
static const double CV_B[5][NFORM] = {
    {60,70,110,120,130}, {40,80,100,120,120}, {60,90,100,120,120},
    {40,80,100,120,120}, {40,80,100,120,120}
};

/* interpolate a vowel (v in 0..1 across a-e-i-o-u) into freq/amp/bw arrays */
static inline void chant_vowel(double v, double shift, double bwmul,
                               double *fr, double *am, double *bw)
{
    int i0, i1, k;
    double t;
    if (v < 0.0) v = 0.0;
    if (v > 1.0) v = 1.0;
    t = v * 4.0;
    i0 = (int)t; if (i0 > 3) i0 = 3;
    i1 = i0 + 1;
    t -= i0;
    for (k = 0; k < NFORM; k++) {
        fr[k] = (CV_F[i0][k] + (CV_F[i1][k] - CV_F[i0][k]) * t) * shift;
        am[k] =  CV_A[i0][k] + (CV_A[i1][k] - CV_A[i0][k]) * t;
        bw[k] = (CV_B[i0][k] + (CV_B[i1][k] - CV_B[i0][k]) * t) * bwmul;
    }
}

/* ------------------------------------------------------------------ */

static void chant_bang(t_alien_chant *x) { x->x_trig = 1; }

static void chant_macro(t_alien_chant *x, t_floatarg f) { x->x_macro = f; }

/* a list of MIDI notes -> voice frequencies */
static void chant_notes(t_alien_chant *x, t_symbol *s, int argc, t_atom *argv)
{
    int i, n = 0;
    (void)s;
    for (i = 0; i < argc && n < MAXV; i++) {
        double m = atom_getfloat(argv + i);
        double f;
        if (!(m > 0.0)) continue;                 /* skip 0 / junk        */
        f = al_mtof(m);
        if (!(f > 8.0 && f < 0.45 * x->x_sr)) continue;
        x->x_v[n].f0 = f;
        n++;
    }
    if (n > 0) x->x_nv = n;
}

/* ------------------------------------------------------------------ */

static t_int *alien_chant_perform(t_int *w)
{
    t_alien_chant *x = (t_alien_chant *)(w[1]);
    t_sample *out = (t_sample *)(w[2]);
    int nsamp = (int)(w[3]);

    double sr = x->x_sr, dt = 1.0 / sr;
    double nyq = sr * 0.47;
    int nv = x->x_nv, v, f, g;

    al_denorm_off();

    /* macro -> character (per block) */
    double t = x->x_macro; t = t - 100.0 * floor(t * 0.01);
    if (!(t >= 0.0 && t <= 100.0)) t = 0.0;
    t *= 0.01;

    /* macro morph: dark soft "ooo" pad -> bright breathy "aaa/eee" choir.
     * vowel walks the perceptual dark->bright path u -> o -> a -> e -> i. */
    static const double vk[5] = {1.00, 0.75, 0.00, 0.25, 0.50}; /* u o a e i */
    double sv = t * 4.0; int jk = (int)sv; if (jk > 3) jk = 3;
    double vowel = vk[jk] + (vk[jk + 1] - vk[jk]) * (sv - jk);

    double shift   = 0.94 + 0.14 * t;           /* head size / brightness  */
    double bwmul   = 0.70 + 0.90 * t;           /* pure -> breathy/wide    */
    double vibdep  = 0.006 + 0.028 * t;          /* vibrato depth (frac)   */
    double vibrate = 4.2 + 1.8 * t;              /* vibrato rate (Hz)      */
    double breath  = (t < 0.45) ? 0.0 : (t - 0.45) / 0.55 * 0.08;
    double bright  = 0.30 + 0.70 * t;            /* HF formant emphasis    */

    /* amp envelope timing */
    double atkc = 1.0 - exp(-1.0 / (sr * 0.09));       /* ~90ms swell in   */
    double sustain = 0.90;                              /* holds while sung */
    double norm = 0.42 / sqrt((double)(nv > 0 ? nv : 1));

    /* consume a trigger: (re)attack the choir env + reset breath phases */
    if (x->x_trig) {
        x->x_trig = 0;
        x->x_seg = 1;
        for (v = 0; v < nv; v++) {
            x->x_v[v].ph = 0.0;
            x->x_v[v].driftph = 0.0;
        }
    }

    while (nsamp--) {
        double s = 0.0, env = x->x_env;

        /* choir breath envelope */
        if (x->x_seg == 1) {
            env += atkc * (1.03 - env);
            if (env >= sustain) { env = sustain; x->x_seg = 2; }
        } else if (x->x_seg == 2) {
            /* hold near sustain, drifting very slowly downward (a held note) */
            env += 0.00002 * (sustain - env);
        }
        if (env < 0.0) env = 0.0;
        x->x_env = env;

        if (env > 1e-5) {
            for (v = 0; v < nv; v++) {
                t_cvoice *vo = &x->x_v[v];
                double f0 = vo->f0;
                double fr[NFORM], am[NFORM], bw[NFORM];
                double vw;

                /* humanised per-voice vibrato + slow vowel drift */
                vo->vibph  += vibrate / sr;   if (vo->vibph  >= 1.0) vo->vibph  -= 1.0;
                vo->driftph += 0.13 / sr;     if (vo->driftph >= 1.0) vo->driftph -= 1.0;
                double vib = vibdep * sin(AL_2PI * (vo->vibph + vo->viboff));
                double drift = 0.04 * sin(AL_2PI * (vo->driftph)) ;
                double fcur = f0 * (1.0 + vo->detune + vib);
                if (!(fcur > 8.0 && fcur < 0.45 * sr)) fcur = f0;

                /* this voice's vowel, humanised */
                vw = vowel + vo->vowoff + drift;
                chant_vowel(vw, shift, bwmul, fr, am, bw);

                /* fire a fresh grain set once per fundamental period */
                vo->ph += fcur / sr;
                if (vo->ph >= 1.0) {
                    vo->ph -= 1.0;
                    for (f = 0; f < NFORM; f++) {
                        int slot = vo->next[f];
                        double a = am[f];
                        /* upper formants get a brightness weight */
                        if (f >= 2) a *= (0.35 + 0.65 * bright);
                        vo->g[f][slot].t = 0.0;
                        vo->g[f][slot].amp = a;
                        vo->g[f][slot].active = 1;
                        vo->next[f] = (slot + 1) % NGRAIN;
                    }
                }

                /* sum active grains for each formant */
                for (f = 0; f < NFORM; f++) {
                    double fc = fr[f];
                    double bwf = bw[f];
                    double alpha, tex;
                    if (!isfinite(fc)) fc = 0.0;
                    if (!isfinite(bwf)) bwf = 50.0;
                    if (bwf < 10.0) bwf = 10.0;
                    if (bwf > 1000.0) bwf = 1000.0;
                    if (!(fc > 20.0 && fc < nyq)) continue;   /* DC / >Nyq  */
                    alpha = AL_PI * bwf;
                    tex = 1.0 / (2.0 * bwf);
                    for (g = 0; g < NGRAIN; g++) {
                        t_cgrain *gr = &vo->g[f][g];
                        double e;
                        if (!gr->active) continue;
                        if (gr->t > 0.06) { gr->active = 0; continue; }
                        if (gr->t < tex) {
                            e = 0.5 * (1.0 - cos(AL_PI * gr->t / tex));
                        } else {
                            e = exp(-alpha * (gr->t - tex));
                            if (e < 1e-5) { gr->active = 0; continue; }
                        }
                        s += gr->amp * e * sin(AL_2PI * fc * gr->t);
                        gr->t += dt;
                    }
                }

                /* a whisper of breath noise, per-voice, up top only */
                if (breath > 0.0) {
                    s += al_randb(&x->rng) * breath * 0.5;
                }
            }

            s *= norm * env;
        }

        /* DC-block, finite guard, soft-clip, bound */
        s = al_dc_tick(&x->dc, s);
        if (!(s > -1e6 && s < 1e6)) s = 0.0;
        s = al_softclip(s);
        if (s >  1.5) s =  1.5;
        if (s < -1.5) s = -1.5;
        *out++ = (t_sample)s;
    }

    /* flush denormal grain state to keep next block cheap */
    for (v = 0; v < nv; v++)
        for (f = 0; f < NFORM; f++)
            for (g = 0; g < NGRAIN; g++) {
                if (!(fabs(x->x_v[v].g[f][g].t) < 1e6))   x->x_v[v].g[f][g].t = 0.0;
                if (!(fabs(x->x_v[v].g[f][g].amp) < 1e6)) x->x_v[v].g[f][g].amp = 0.0;
            }
    return (w + 4);
}

static void alien_chant_dsp(t_alien_chant *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr; if (x->x_sr < 1000) x->x_sr = 44100;
    dsp_add(alien_chant_perform, 3, x, sp[0]->s_vec, (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_chant_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_chant *x = (t_alien_chant *)pd_new(alien_chant_tilde_class);
    int v, f, g;
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    al_rng_seed(&x->rng, 0xc4a17u);

    x->x_nv = 1;
    for (v = 0; v < MAXV; v++) {
        t_cvoice *vo = &x->x_v[v];
        vo->f0 = al_mtof(50 + v);
        vo->ph = (double)v * 0.13;
        vo->vibph = 0.0;
        vo->driftph = 0.0;
        /* humanise: each singer gets its own vibrato phase, micro-detune,
         * and a small static vowel offset so they never sound like clones */
        vo->viboff = al_rand(&x->rng);
        vo->detune = (al_randb(&x->rng)) * 0.004;
        vo->vowoff = (al_randb(&x->rng)) * 0.03;
        for (f = 0; f < NFORM; f++) {
            vo->next[f] = 0;
            for (g = 0; g < NGRAIN; g++) vo->g[f][g].active = 0;
        }
    }
    x->x_v[0].f0 = al_mtof(50);

    x->x_macro = 50.0;
    x->x_seg = 0; x->x_env = 0.0; x->x_trig = 0;
    al_dc_init(&x->dc);

    /* inlet 1: float macro; inlet 2: list of notes */
    floatinlet_new(&x->x_obj, &x->x_macro);
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_list, gensym("notes"));
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_chant_tilde_setup(void)
{
    alien_chant_tilde_class = class_new(gensym("alien_chant~"),
        (t_newmethod)alien_chant_new, 0, sizeof(t_alien_chant),
        CLASS_DEFAULT, A_GIMME, 0);
    class_addbang(alien_chant_tilde_class, chant_bang);
    class_addmethod(alien_chant_tilde_class, (t_method)chant_macro,
                    gensym("macro"), A_FLOAT, 0);
    class_addmethod(alien_chant_tilde_class, (t_method)chant_notes,
                    gensym("notes"), A_GIMME, 0);
    class_addmethod(alien_chant_tilde_class, (t_method)alien_chant_dsp,
                    gensym("dsp"), A_CANT, 0);
}
