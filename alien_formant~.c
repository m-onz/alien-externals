/* alien_formant~.c  --  FOF formant synthesis
 *
 * Fonction d'Onde Formantique, from IRCAM's CHANT.  A formant is not
 * made by filtering here -- each formant IS a stream of tiny damped
 * sinusoids, one fired per period of the fundamental:
 *
 *   grain(t) = envelope(t) * sin(2*pi*Fc*t)
 *   envelope: raised-cosine attack, then exponential decay set by
 *             the formant bandwidth
 *
 * Because the grains restart in phase with the fundamental, the formant
 * centre stays put while the pitch moves -- which is what a voice does
 * and what a filter bank only approximates.  It also means you can put
 * a formant anywhere, including below the fundamental or at 8 kHz, and
 * it stays there.
 *
 * Two things a filter cannot do at all:
 *   octaviation   fades alternate grains, dropping the perceived pitch
 *                 an octave without moving the formants.  The classic
 *                 CHANT control, and the sound of the bass register.
 *   growl         periodic jitter of the fundamental -- vocal fry,
 *                 throat singing, and dubstep growls.
 *
 *   inlet 0   signal/float   fundamental in Hz
 *   inlet 1   signal/float   MACRO 0..100 (wrapped mod 100) -- one knob that
 *                            morphs the whole voice along a curated path.  At 0
 *                            it is a dark octaved bass "u", mellow and pure; the
 *                            vowel opens u -> o -> a -> e -> i as it rises, the
 *                            head shrinks and brightens, the octaviation lifts,
 *                            then breath, growl and drive come in so that at 100
 *                            it is a bright, buzzing, driven "i" with vocal fry.
 *
 *   outlet 0  signal
 *
 * build:
 *   gcc -shared -fPIC -O2 -o alien_formant~.pd_linux alien_formant~.c -lm
 */

#include "m_pd.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define NFORM  5
#define NGRAIN 8           /* overlapping grains per formant */

static t_class *alien_formant_tilde_class;

typedef struct _grain {
    double t;              /* seconds since onset */
    double amp;
    int    active;
} t_grain;

typedef struct _alien_formant {
    t_object x_obj;
    t_float  x_f;

    double   x_sr, x_ph;
    t_grain  x_g[NFORM][NGRAIN];
    int      x_next[NFORM];
    int      x_parity;

    double   x_freq[NFORM], x_amp[NFORM], x_bw[NFORM];
    double   x_gph;
    unsigned x_rng;
    double   x_dc1, x_dc2;   /* one-pole DC blocker state (in/out) */
} t_alien_formant;

/* five vowels: frequency, amplitude, bandwidth for each of 5 formants */
static const double V_F[5][NFORM] = {
    {600,1040,2250,2450,2750},   /* a */
    {400,1620,2400,2800,3100},   /* e */
    {250,1750,2600,3050,3340},   /* i */
    {400, 750,2400,2600,2900},   /* o */
    {350, 600,2400,2675,2950}    /* u */
};
static const double V_A[5][NFORM] = {
    {1.0,0.45,0.35,0.30,0.12},
    {1.0,0.25,0.35,0.25,0.12},
    {1.0,0.03,0.16,0.10,0.05},
    {1.0,0.28,0.08,0.07,0.02},
    {1.0,0.10,0.18,0.09,0.04}
};
static const double V_B[5][NFORM] = {
    {60,70,110,120,130}, {40,80,100,120,120}, {60,90,100,120,120},
    {40,80,100,120,120}, {40,80,100,120,120}
};

static inline double softclip(double v)
{
    if (v >  3.0) return 1.0;
    if (v < -3.0) return -1.0;
    return v * (27.0 + v*v) / (27.0 + 9.0*v*v);
}

/* flush denormals / non-finite state to zero */
static inline double flush(double v)
{
    if (!isfinite(v)) return 0.0;
    if (v < 1e-30 && v > -1e-30) return 0.0;
    return v;
}

static void form_set(t_alien_formant *x, double v, double shift, double bwmul)
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
        x->x_freq[k] = (V_F[i0][k] + (V_F[i1][k] - V_F[i0][k]) * t) * shift;
        x->x_amp[k]  =  V_A[i0][k] + (V_A[i1][k] - V_A[i0][k]) * t;
        x->x_bw[k]   = (V_B[i0][k] + (V_B[i1][k] - V_B[i0][k]) * t) * bwmul;
    }
}

/* ------------------------------------------------------------------ */

/* map the 0..1 macro onto the whole voice (curated morph).
 * vowel walks a dark->bright path u -> o -> a -> e -> i; the head shrinks
 * and brightens; octaviation lifts early then fades; breath, growl and
 * drive arrive up top. */
static inline void formant_macro(double t, double *vowel, double *shift,
                                 double *bwmul, double *oct, double *growl,
                                 double *growlrate, double *noise, double *drive)
{
    /* curated vowel path across the five stored vowels a e i o u, which are
     * indexed 0..1 as a(0) e(.25) i(.5) o(.75) u(1).  We want the perceptual
     * order dark->bright: u -> o -> a -> e -> i.  Map t through those knots. */
    static const double vk[5] = {1.00, 0.75, 0.00, 0.25, 0.50}; /* u o a e i */
    double s = t * 4.0;
    int j = (int)s; if (j > 3) j = 3;
    *vowel = vk[j] + (vk[j + 1] - vk[j]) * (s - j);

    *shift = 0.80 + 0.70 * t;                 /* big dark head -> small bright */
    *bwmul = 0.55 + 1.35 * t;                 /* nasal/pure -> breathy/wide    */
    *oct   = (t < 0.35) ? (1.0 - t / 0.35) : 0.0; /* octave down at the bottom  */
    *growl = (t < 0.6) ? 0.0 : (t - 0.6) / 0.4 * 0.8; /* fry only up top        */
    *growlrate = 70.0;
    *noise = (t < 0.5) ? 0.0 : (t - 0.5) / 0.5 * 0.5; /* breath in the top half */
    *drive = (t < 0.7) ? 0.0 : (t - 0.7) / 0.3;       /* grit at the very top   */
}

/* ------------------------------------------------------------------ */

static t_int *alien_formant_perform(t_int *w)
{
    t_alien_formant *x = (t_alien_formant *)(w[1]);
    t_sample *fin = (t_sample *)(w[2]);
    t_sample *min = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    int nsamp = (int)(w[5]);

    double sr = x->x_sr, dt = 1.0 / sr;
    double nyq = sr * 0.47;
    int f, g;

    /* ---- once-per-block control: read the macro from its FIRST sample.
     * All the expensive coefficient work (vowel interpolation, formant
     * freq/amp/bw) happens here, ONCE per block, not per sample.  Doing it
     * per sample was the stutter: 15 interpolations plus a full pass over
     * the vowel tables every single sample across the whole sweep. */
    double mac0 = min[0];
    double t, vowel, shift, bwmul, oct, growl, growlrate, noise, drive;

    /* wrap the macro into [0,100) then normalise to [0,1) */
    if (!isfinite(mac0)) mac0 = 0.0;                 /* NaN/inf guard */
    t = mac0 - 100.0 * floor(mac0 * 0.01);
    if (!(t >= 0.0 && t <= 100.0)) t = 0.0;
    t *= 0.01;
    if (t < 0.0) t = 0.0; if (t > 1.0) t = 1.0;      /* keep index in range */
    formant_macro(t, &vowel, &shift, &bwmul, &oct, &growl,
                  &growlrate, &noise, &drive);
    form_set(x, vowel, shift, bwmul);                /* per-block, not per-sample */

    /* precompute per-formant grain coefficients ONCE per block.  These
     * depend only on the (block-constant) formant freq/bandwidth, so there
     * is no reason to redo the divides per sample.  Clamp everything into a
     * stable, finite range: formant centre below Nyquist, bandwidth away
     * from 0 (tex = 1/(2*bw)) and away from blow-up. */
    double kfc[NFORM], kalpha[NFORM], ktex[NFORM];
    int    klive[NFORM];
    for (f = 0; f < NFORM; f++) {
        double fc = x->x_freq[f];
        double bw = x->x_bw[f];
        if (!isfinite(fc)) fc = 0.0;
        if (!isfinite(bw)) bw = 50.0;
        if (bw < 10.0)   bw = 10.0;         /* keep decay + tex bounded */
        if (bw > 1000.0) bw = 1000.0;
        klive[f]  = (fc > 20.0 && fc < nyq);  /* skip DC/inaudible & >Nyq  */
        kfc[f]    = fc;
        kalpha[f] = M_PI * bw;
        ktex[f]   = 1.0 / (2.0 * bw);
    }

    while (nsamp--) {
        double f0 = *fin++;
        double y = 0.0, jitter = 1.0;

        if (!isfinite(f0)) f0 = 20.0;
        if (f0 < 20.0) f0 = 20.0;
        if (f0 > 2000.0) f0 = 2000.0;

        /* growl: periodic modulation of the fundamental.  This is what
         * makes vocal fry and throat-singing registers, and it is also
         * exactly the dubstep growl. */
        if (growl > 0.0) {
            x->x_gph += growlrate / sr;
            if (x->x_gph >= 1.0) x->x_gph -= 1.0;
            jitter = 1.0 + growl * 0.5 * sin(x->x_gph * 2.0 * M_PI);
        }

        /* fire a new grain per formant, once per fundamental period */
        x->x_ph += f0 * jitter / sr;
        if (x->x_ph >= 1.0) {
            x->x_ph -= 1.0;
            x->x_parity = !x->x_parity;
            for (f = 0; f < NFORM; f++) {
                int slot = x->x_next[f];
                double a = x->x_amp[f];
                /* octaviation fades alternate grains: the pitch drops
                 * an octave, the formants do not move */
                if (oct > 0.0 && x->x_parity) {
                    double k = 1.0 - (oct > 1.0 ? 1.0 : oct);
                    a *= k;
                }
                x->x_g[f][slot].t = 0.0;
                x->x_g[f][slot].amp = a;
                x->x_g[f][slot].active = 1;
                x->x_next[f] = (slot + 1) % NGRAIN;
            }
        }

        for (f = 0; f < NFORM; f++) {
            double fc    = kfc[f];
            double alpha = kalpha[f];
            double tex   = ktex[f];
            if (!klive[f]) continue;                  /* >Nyquist / DC */
            for (g = 0; g < NGRAIN; g++) {
                t_grain *gr = &x->x_g[f][g];
                double env;
                if (!gr->active) continue;
                if (gr->t > 0.06) { gr->active = 0; continue; }
                if (gr->t < tex) {
                    env = 0.5 * (1.0 - cos(M_PI * gr->t / tex));
                } else {
                    env = exp(-alpha * (gr->t - tex));
                    /* prune only once we are in the decay tail: the attack
                     * legitimately starts at env==0, so we must not kill the
                     * grain there or every grain dies on its first sample. */
                    if (env < 1e-5) { gr->active = 0; continue; }
                }
                y += gr->amp * env * sin(2.0 * M_PI * fc * gr->t);
                gr->t += dt;
            }
        }

        if (noise > 0.0) {
            x->x_rng ^= x->x_rng << 13; x->x_rng ^= x->x_rng >> 17; x->x_rng ^= x->x_rng << 5;
            y += ((double)(x->x_rng & 0xffffff) / (double)0x800000 - 1.0)
                 * noise * 0.15;
        }

        y *= 0.35;
        if (drive > 0.0)
            y = softclip(y * (1.0 + drive * 8.0)) / (1.0 + drive * 2.0);

        /* DC blocker: the phase-locked FOF grains restart every fundamental
         * period, which pumps a slow DC offset into the sum.  A one-pole
         * high-pass (~20 Hz) removes it without touching the formants. */
        {
            double in = y;
            y = in - x->x_dc1 + 0.9985 * x->x_dc2;
            x->x_dc1 = in;
            x->x_dc2 = y;
        }

        /* final safety net: never let a NaN/Inf reach the DAC, and keep the
         * signal soft-limited to a sane, near-unity range. */
        y = flush(y);
        y = softclip(y);                 /* bounded to (-1,1)-ish, saturates */
        if (y >  1.5) y =  1.5;
        if (y < -1.5) y = -1.5;
        if (!isfinite(y)) y = 0.0;
        *out++ = (t_sample)y;
    }

    /* flush any denormal state that would burn CPU next block */
    x->x_ph  = flush(x->x_ph);
    x->x_gph = flush(x->x_gph);
    x->x_dc1 = flush(x->x_dc1);
    x->x_dc2 = flush(x->x_dc2);
    for (f = 0; f < NFORM; f++)
        for (g = 0; g < NGRAIN; g++) {
            x->x_g[f][g].t   = flush(x->x_g[f][g].t);
            x->x_g[f][g].amp = flush(x->x_g[f][g].amp);
        }
    return (w + 6);
}

static void alien_formant_dsp(t_alien_formant *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    dsp_add(alien_formant_perform, 5, x,
            sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_formant_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_formant *x = (t_alien_formant *)pd_new(alien_formant_tilde_class);
    int f, g;
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    x->x_ph = 0.0; x->x_gph = 0.0; x->x_parity = 0; x->x_rng = 22222u;
    x->x_dc1 = 0.0; x->x_dc2 = 0.0;
    for (f = 0; f < NFORM; f++) {
        x->x_next[f] = 0;
        for (g = 0; g < NGRAIN; g++) x->x_g[f][g].active = 0;
    }
    form_set(x, 1.0, 0.80, 0.55);   /* darkest "u" at macro 0 */

    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_formant_tilde_setup(void)
{
    alien_formant_tilde_class = class_new(gensym("alien_formant~"),
        (t_newmethod)alien_formant_new, 0, sizeof(t_alien_formant),
        CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_formant_tilde_class, t_alien_formant, x_f);
    class_addmethod(alien_formant_tilde_class, (t_method)alien_formant_dsp,
                    gensym("dsp"), A_CANT, 0);
}
