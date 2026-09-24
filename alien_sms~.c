/* alien_sms~.c  --  spectral modelling synthesis drone (Serra SMS / MQ)
 *
 * Spectral Modelling Synthesis (Xavier Serra; McAulay-Quatieri) splits a
 * sound into two parts: a set of tracked SINUSOIDAL partials (the
 * "deterministic" part -- the pitched, tonal core) plus a filtered-NOISE
 * RESIDUAL (the "stochastic" part -- the breath, air and scrape that the
 * sines cannot represent).  Re-synthesising from those two banks and, more
 * interestingly, MORPHING the balance and inharmonicity between them, is
 * what turns a pure tone into something breathy, then bell-like.
 *
 * This object is a DRONE voice built on that idea:
 *
 *   DETERMINISTIC   an additive stack of up to 32 sine partials at k*f0 with
 *                   a macro-tilted spectral envelope.  Up top the partials
 *                   are inharmonically STRETCHED (k^stretch * f0) into a bell.
 *                   Each partial has a slow independent shimmer (al_wander)
 *                   so the tone breathes instead of sitting dead.
 *
 *   STOCHASTIC      pink/white noise pushed through a few resonant bandpasses
 *                   that track the partial region -- shaped "air" under the
 *                   tone, not raw hiss (except right at the breathy top).
 *
 * One MACRO knob morphs the balance and character:
 *   0    pure harmonic tone   (sines only, dark tilt, no residual)
 *   ~30  breathy              (residual air fades in, tone still harmonic)
 *   ~65  inharmonic bell      (partials stretch, metallic, shimmer, ring)
 *   100  hollow hybrid        (thinned odd partials + brighter open residual)
 *
 *   inlet 0   signal/float   fundamental in Hz (CLASS_MAINSIGNALIN)
 *   inlet 1   signal/float   MACRO 0..100 (wrapped mod 100)
 *   outlet 0  signal
 *
 * build:
 *   gcc -shared -fPIC -O2 -o alien_sms~.pd_linux alien_sms~.c -lm
 *   (macOS: -bundle -undefined dynamic_lookup -o alien_sms~.pd_darwin)
 */

#include "m_pd.h"
#include "alien.h"
#include <math.h>
#include <string.h>

/* array capacity -- partial count is hard-clamped to MAXN <= MAXP so the
 * additive loop can never index past the phase / amp tables */
#define MAXP 48
#define MAXN 32

/* stochastic residual bandpass count */
#define NRES 4

static t_class *alien_sms_tilde_class;

typedef struct _alien_sms {
    t_object x_obj;
    t_float  x_f;

    double   x_sr;

    /* deterministic partial bank */
    double   x_ph[MAXP];        /* per-partial oscillator phase 0..1        */

    /* stochastic residual */
    al_rng   x_rng;
    al_pink  x_pink;
    al_bq    x_res[NRES];       /* resonant bandpasses tracking partials    */
    al_dc    x_dc;

    /* per-partial shimmer brain */
    al_wander x_wander;

    /* per-block cache (built once per block from first sample's macro+f0) */
    int      x_n;               /* active partial count, clamped [1,MAXN]   */
    double   x_amp[MAXP];       /* per-partial amplitude                    */
    double   x_finc[MAXP];      /* per-partial phase increment fr/sr        */
    double   x_active[MAXP];    /* 1.0 if below Nyquist else 0.0            */
    double   x_shdepth;         /* shimmer depth (0 = off)                  */
    double   x_norm;            /* 1/sum(active amps)                       */
    double   x_resf[NRES];      /* residual bandpass centre freqs (Hz)      */
    double   x_resq;            /* residual bandpass Q                       */
    double   x_reslev;          /* residual mix level                        */
    double   x_detlev;          /* deterministic mix level                   */
    int      x_pinkmix;         /* 1 => shape pink (airy), 0 => whiter top   */
    double   x_lastmac;
    double   x_lastf0;
    int      x_haveblock;
} t_alien_sms;

/* ------------------------------------------------------------------ */

/* map the 0..1 macro onto the whole parameter set (curated morph) */
static inline void sms_macro(double t, int *n, double *stretch, double *tilt,
                             double *shimmer, double *odd,
                             double *detlev, double *reslev, double *resq,
                             double *resspread, int *pinkmix)
{
    /* partial count: a handful low, filling out toward the top */
    *n = (int)(6.0 + 24.0 * t);
    if (*n < 1)    *n = 1;
    if (*n > MAXN) *n = MAXN;

    /* stretch: harmonic (1.0) at the bottom, spreading into a bell.  Kept
     * gentle so the deterministic part stays partials-under-Nyquist. */
    *stretch = 1.0 + 0.28 * t * t;

    /* spectral tilt: dark rolloff low, thinning brighter up top */
    *tilt = -1.4 + 3.0 * t;

    /* per-partial shimmer: dead at the bottom, breathing higher up */
    *shimmer = (t < 0.35) ? 0.0 : (t - 0.35) / 0.65;

    /* thin toward odd partials at the very top (hollow hybrid) */
    *odd = (t < 0.78) ? 0.0 : (t - 0.78) / 0.22;

    /* deterministic vs stochastic balance.  Tone dominates at the bottom;
     * the residual air fades in through the breathy middle and stays. */
    *detlev = 1.0 - 0.45 * t;                       /* 1.0 -> 0.55        */

    /* residual: none at 0, up quickly through the breathy region, then eased
     * back a touch so the bell keeps its pitch */
    if (t < 0.12) {
        *reslev = 0.0;
    } else if (t < 0.4) {
        *reslev = (t - 0.12) / 0.28;                /* 0 -> 1 breath in   */
    } else {
        *reslev = 1.0 - 0.35 * (t - 0.4) / 0.6;     /* 1 -> 0.65          */
    }

    /* bandpass Q: high (narrow, tonal air) low, opening (wider, hiss) high */
    *resq = 9.0 - 6.5 * t;                          /* 9 -> 2.5           */

    /* how far the residual bands spread above the fundamental region */
    *resspread = 1.0 + 6.0 * t;

    /* shape pink noise (airy) until the very top, where it opens toward
     * white hiss -- the only place raw HISS is welcome */
    *pinkmix = (t < 0.85) ? 1 : 0;
}

/* Rebuild per-partial + residual tables from one macro value + f0.  Called
 * once per block, only when macro/f0 change. */
static void build_block(t_alien_sms *x, double mac, double f0)
{
    double sr = x->x_sr, nyq = sr * 0.45;
    double t, stretch, tilt, shimmer, odd, detlev, reslev, resq, resspread;
    double sum = 0.0;
    int n, i, pinkmix;

    /* wrap macro into [0,100) then normalise to [0,1) */
    if (!(mac == mac)) mac = 0.0;                   /* NaN guard          */
    t = mac - 100.0 * floor(mac * 0.01);
    if (!(t >= 0.0 && t <= 100.0)) t = 0.0;
    t *= 0.01;

    sms_macro(t, &n, &stretch, &tilt, &shimmer, &odd,
              &detlev, &reslev, &resq, &resspread, &pinkmix);

    if (n < 1)    n = 1;
    if (n > MAXN) n = MAXN;

    /* clamp fundamental: [8, 0.45*sr] */
    if (!(f0 == f0)) f0 = 8.0;
    if (f0 < 8.0)       f0 = 8.0;
    if (f0 > 0.45 * sr) f0 = 0.45 * sr;

    x->x_n       = n;
    x->x_detlev  = detlev;
    x->x_reslev  = reslev;
    x->x_resq    = resq;
    x->x_pinkmix = pinkmix;
    x->x_shdepth = (shimmer > 0.0) ? shimmer * 0.010 : 0.0;

    for (i = 0; i < n; i++) {
        double r = (double)(i + 1);                 /* harmonic number k  */
        double fr, a;

        /* inharmonic stretch: k^stretch (k=1 unchanged, higher spread) */
        fr = f0 * pow(r, stretch);

        /* drop rather than fold -- never aliases */
        if (fr > nyq || fr < 1.0) {
            x->x_active[i] = 0.0;
            x->x_amp[i]    = 0.0;
            x->x_finc[i]   = 0.0;
            continue;
        }

        a = 1.0 / r;
        if (tilt != 0.0) a *= pow(r, -tilt);
        if (odd > 0.0 && (i & 1)) a *= 1.0 - odd;   /* fade even partials */
        if (!(a == a) || a < 0.0) a = 0.0;

        x->x_active[i] = 1.0;
        x->x_amp[i]    = a;
        x->x_finc[i]   = fr / sr;
        sum += a;
    }

    x->x_norm = (sum > 1e-6) ? (1.0 / sum) : 1.0;

    /* residual bandpass centres: spread across the partial region so the
     * air sits under and around the tone.  Clamp each below Nyquist. */
    for (i = 0; i < NRES; i++) {
        double mult = 1.0 + resspread * ((double)(i) / (double)(NRES - 1));
        double cf = f0 * mult * (i == 0 ? 1.0 : 2.0);   /* fan upward     */
        if (!(cf == cf) || cf < 20.0) cf = 20.0;
        if (cf > nyq) cf = nyq;
        x->x_resf[i] = cf;
    }

    x->x_lastmac   = mac;
    x->x_lastf0    = f0;
    x->x_haveblock = 1;
}

static t_int *alien_sms_perform(t_int *w)
{
    t_alien_sms *x = (t_alien_sms *)(w[1]);
    t_sample *fin = (t_sample *)(w[2]);
    t_sample *min = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    int nsamp = (int)(w[5]);

    double sr = x->x_sr;
    double mac0 = min[0], f00 = fin[0];
    double norm, shdepth, detlev, reslev, resq;
    int n, i, pinkmix;

    al_denorm_off();

    if (!x->x_haveblock || mac0 != x->x_lastmac || f00 != x->x_lastf0)
        build_block(x, mac0, f00);

    n       = x->x_n;
    norm    = x->x_norm;
    shdepth = x->x_shdepth;
    detlev  = x->x_detlev;
    reslev  = x->x_reslev;
    resq    = x->x_resq;
    pinkmix = x->x_pinkmix;

    /* slow shimmer brain: one tick per block is plenty for a drone */
    al_wander_tick(&x->x_wander, &x->x_rng, 1.6, sr);

    while (nsamp--) {
        double det = 0.0, res = 0.0, s;

        fin++;                                       /* per-block reads    */
        min++;

        /* ---- deterministic: additive sine stack --------------------- */
        for (i = 0; i < n; i++) {
            double d = 1.0;
            if (x->x_active[i] == 0.0) continue;

            if (shdepth > 0.0)                       /* per-partial detune */
                d = 1.0 + shdepth * al_wander_bip(&x->x_wander, i);

            x->x_ph[i] += x->x_finc[i] * d;
            if (x->x_ph[i] >= 1.0) x->x_ph[i] -= floor(x->x_ph[i]);
            else if (x->x_ph[i] < 0.0) x->x_ph[i] -= floor(x->x_ph[i]);

            det += x->x_amp[i] * al_sin01(x->x_ph[i]);
        }
        det *= norm;

        /* ---- stochastic: filtered-noise residual -------------------- */
        if (reslev > 0.0) {
            double src = pinkmix ? al_pink_tick(&x->x_pink, &x->x_rng)
                                 : al_white(&x->x_rng);
            double acc = 0.0;
            for (i = 0; i < NRES; i++)
                acc += al_reson(&x->x_res[i], src, x->x_resf[i], resq, sr);
            res = acc * (1.0 / (double)NRES);
        }

        /* ---- mix ---------------------------------------------------- */
        s = detlev * det + reslev * 0.9 * res;
        s = al_dc_tick(&x->x_dc, s);

        /* nominal level ~ -12 dBFS, then always-on softclip */
        s *= 1.15;
        s = al_softclip(s);

        /* crash net: never emit NaN/Inf, hard bound as last resort */
        if (!(s > -1e6 && s < 1e6)) s = 0.0;
        if (s >  1.5) s =  1.5;
        if (s < -1.5) s = -1.5;

        *out++ = (t_sample)s;
    }
    return (w + 6);
}

static void alien_sms_dsp(t_alien_sms *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    if (x->x_sr < 1000.0) x->x_sr = 44100.0;
    x->x_haveblock = 0;                              /* force rebuild      */
    dsp_add(alien_sms_perform, 5, x,
            sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_sms_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_sms *x = (t_alien_sms *)pd_new(alien_sms_tilde_class);
    int i;
    (void)s; (void)argc; (void)argv;

    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;

    for (i = 0; i < MAXP; i++) x->x_ph[i] = (double)i * 0.0137;

    al_rng_seed(&x->x_rng, 0x51A11EEDu);
    al_pink_init(&x->x_pink);
    for (i = 0; i < NRES; i++) al_bq_init(&x->x_res[i]);
    al_dc_init(&x->x_dc);
    al_wander_init(&x->x_wander, &x->x_rng, MAXN);

    x->x_n = 0;
    x->x_norm = 1.0;
    x->x_shdepth = 0.0;
    x->x_detlev = 1.0;
    x->x_reslev = 0.0;
    x->x_resq = 6.0;
    x->x_pinkmix = 1;
    for (i = 0; i < MAXP; i++) {
        x->x_amp[i] = 0.0; x->x_finc[i] = 0.0; x->x_active[i] = 0.0;
    }
    for (i = 0; i < NRES; i++) x->x_resf[i] = 100.0;
    x->x_haveblock = 0;
    x->x_lastmac = -1e30;
    x->x_lastf0  = -1e30;

    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_sms_tilde_setup(void)
{
    alien_sms_tilde_class = class_new(gensym("alien_sms~"),
        (t_newmethod)alien_sms_new, 0, sizeof(t_alien_sms),
        CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_sms_tilde_class, t_alien_sms, x_f);
    class_addmethod(alien_sms_tilde_class, (t_method)alien_sms_dsp,
                    gensym("dsp"), A_CANT, 0);
}
