/* alien_liquid1~.c  --  tuned + damped feedback: Karplus-Strong done musically
 *
 * Where alien_liquid~ pours an additive swarm through a phaser and a self-
 * sampling feedback delay (which is gloriously noisy at the top), this voice
 * takes the opposite vow: it must stay TONAL, always.  Richness that never
 * turns into broadband hiss.  It gets there the Karplus-Strong way, but built
 * as a musically-tuned resonator bank rather than a single string:
 *
 *   1. EXCITER.  A modest, band-limited-ish harmonic source driven at the
 *      fundamental -- a small summed-harmonic saw (a handful of sines) plus a
 *      short filtered impulse burst re-triggered each period.  Just enough
 *      harmonic content to feed the resonators; kept low in level so it never
 *      dominates.
 *
 *   2. TUNED COMB RESONATORS.  A bank of 2..4 pitch-tuned feedback delay lines,
 *      each length = sr/f0 (fractional, interpolated read like alien_liquid~'s
 *      delread) times a small unison ratio, so every comb's resonant peaks land
 *      ON the harmonic series.  That is what keeps it pitched instead of noisy.
 *      Tiny detune offsets (a few cents) between the copies give a liquid
 *      chorusing shimmer that stays harmonic.
 *
 *   3. LOOP DAMPING.  A one-pole lowpass INSIDE each feedback loop (the K-S
 *      damping filter): every pass loses some top end, so highs decay instead
 *      of accumulating into hiss.  This is what keeps it tonal.  Exposed as part
 *      of the morph: bright/metallic at low macro <-> dark/plucked... actually
 *      the morph opens damping UP as it rises (longer, brighter, singing) but
 *      always keeps enough loss that the loop cannot run away.
 *
 *   4. ALLPASS DIFFUSION.  A short series allpass chain (linear, like a
 *      reverb/phaser diffuser) on the WET signal adds liquid motion and density
 *      WITHOUT adding harmonics or noise -- it only reshuffles phase.
 *
 * There is deliberately NO broadband saturator in the feedback path (that
 * aliases into noise).  Feedback gain is strictly < 1 and the loop lowpass
 * guarantees decay, so it churns and sings forever without ever blowing up or
 * going white.
 *
 *   inlet 0   signal/float   fundamental in Hz            (CLASS_MAINSIGNALIN)
 *   inlet 1   signal/float   MACRO 0..100 (clamped) -- one knob:
 *                            at 0 a clean, short, damped pluck; rising -> longer
 *                            sustain, brighter damping, more/again-detuned tuned
 *                            combs, more diffusion -> a rich, singing, evolving
 *                            string/metal that stays pitched; at 100 dense and
 *                            shimmering but STILL TONAL, not noise.
 *
 *   outlet 0  signal
 *
 * All the discrete/coefficient work (delay lengths, damping, feedback,
 * diffusion coeffs) is done once per block; the per-sample loop is just the
 * exciter, four interpolated comb reads and the allpass chain.  Every filter /
 * delay state is denormal-flushed and finite-guarded (magnitude test, survives
 * -ffast-math), delay lengths clamped to [1, DLEN-2], feedback strictly < 1,
 * and the output is finite-guarded and bounded.
 *
 * build:
 *   gcc -shared -fPIC -O2 -o alien_liquid1~.pd_linux alien_liquid1~.c -lm
 *   (macOS: -bundle -undefined dynamic_lookup -o alien_liquid1~.pd_darwin)
 */

#include "m_pd.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define NCOMB 4           /* number of tuned comb resonators                   */
#define NHARM 6           /* exciter harmonics                                 */
#define NAP   4           /* allpass diffusion stages                          */
#define DLEN  32768       /* per-comb delay line length (~0.74 s @ 44.1k)      */
#define DMASK (DLEN - 1)
#define APLEN 2048        /* allpass buffer length                             */
#define APMASK (APLEN - 1)

static t_class *alien_liquid1_tilde_class;

typedef struct _alien_liquid1 {
    t_object x_obj;
    t_float  x_f;

    double   x_sr;

    /* ---- exciter ---- */
    double   x_exph;              /* exciter master phase (one period)         */
    double   x_hph[NHARM];        /* per-harmonic phase                        */
    double   x_burst;             /* re-triggered impulse-burst envelope       */
    unsigned x_rng;               /* noise RNG for the burst                   */
    double   x_noiselp;           /* one-pole to band-limit the burst          */

    /* ---- modulation engine (alive-but-controlled) ---- */
    double   x_cph[NCOMB];        /* per-comb pitch-drift LFO phase            */
    double   x_dph[NCOMB];        /* per-comb damping/feedback wobble phase    */
    double   x_glfo[3];           /* global slow LFO phases                    */
    double   x_walk;              /* smoothed random-walk value in ~[-1,1]     */
    double   x_walktarget;        /* random-walk target                        */
    unsigned x_wrng;              /* random-walk RNG                           */
    double   x_moddepth;          /* macro-scaled modulation depth (0..1)      */
    double   x_modrate;           /* macro-scaled modulation rate factor       */
    /* per-block phase increments (Hz/sr) for the modulators */
    double   x_cinc[NCOMB];
    double   x_dinc[NCOMB];
    double   x_ginc[3];
    double   x_walkinc;           /* random-walk retarget probability/step     */

    /* ---- tuned comb resonators ---- */
    double   x_buf[NCOMB][DLEN];  /* one delay line per comb                   */
    int      x_wi[NCOMB];         /* per-comb write index                      */
    double   x_damplp[NCOMB];     /* per-comb loop damping lowpass state       */

    /* ---- allpass diffusion ---- */
    double   x_apbuf[NAP][APLEN];
    int      x_apwi[NAP];

    /* ---- per-block cache ---- */
    double   x_cdelay[NCOMB];     /* tuned delay length per comb (samples)     */
    double   x_cfb[NCOMB];        /* per-comb feedback gain (< 1)              */
    double   x_damp;              /* loop lowpass coeff (0..~0.95)             */

    /* ---- un-modulated base values (modulation perturbs around these) ---- */
    double   x_cdelay0[NCOMB];    /* base tuned delay per comb                 */
    double   x_cfb0[NCOMB];       /* base feedback per comb                    */
    double   x_damp0;             /* base damping                              */
    double   x_wet0;              /* base wet mix                              */
    double   x_apmix0;            /* base diffusion amount                     */
    double   x_exlevel0;          /* base exciter level                        */
    /* smoothed (block-rate) modulated coeffs actually used per sample */
    double   x_cdelay_s[NCOMB];
    double   x_cfb_s[NCOMB];
    double   x_damp_s;
    double   x_wet_s;
    double   x_apmix_s;
    double   x_exlevel_s;
    double   x_exlevel;           /* exciter drive into the combs             */
    double   x_exbright;          /* exciter harmonic tilt (dark->bright)      */
    double   x_burstamt;          /* impulse-burst contribution               */
    double   x_apcoef;            /* allpass diffusion coefficient             */
    int      x_apdelay[NAP];      /* per-stage allpass delay (samples)         */
    double   x_apmix;             /* diffusion wet amount                      */
    double   x_wet;               /* dry(exciter)->wet(resonators) mix         */
    double   x_hinc[NHARM];       /* exciter per-harmonic phase increment      */
    double   x_hamp[NHARM];       /* exciter per-harmonic amplitude            */
    double   x_exinc;             /* exciter master (period) increment         */

    double   x_lastmac;
    double   x_lastf0;
    int      x_haveblock;
} t_alien_liquid1;

static inline double softclip(double v)
{
    if (v >  3.0) return 1.0;
    if (v < -3.0) return -1.0;
    return v * (27.0 + v*v) / (27.0 + 9.0*v*v);
}

/* flush denormals and force finite; magnitude tests survive -ffast-math */
static inline double flushd(double v)
{
    if (!(v > -1e12 && v < 1e12)) return 0.0;   /* NaN/Inf/huge -> 0          */
    if (v > -1e-20 && v < 1e-20) return 0.0;    /* denormal -> 0              */
    return v;
}

/* ------------------------------------------------------------------ */

/* interpolated read from comb `c`'s delay line, `d` samples behind write head */
static inline double delread(t_alien_liquid1 *x, int c, double d)
{
    double rp;
    int i0, i1;
    double fr;
    double *buf = x->x_buf[c];
    if (d < 1.0)        d = 1.0;
    if (d > DLEN - 2.0) d = DLEN - 2.0;
    rp = (double)x->x_wi[c] - d;
    while (rp < 0.0) rp += (double)DLEN;
    i0 = (int)rp;
    fr = rp - (double)i0;
    i1 = (i0 + 1) & DMASK;
    i0 &= DMASK;
    return buf[i0] * (1.0 - fr) + buf[i1] * fr;
}

/* small unison ratios for the comb bank -- integer/simple so peaks stay on the
 * harmonic series; the detune offsets (in cents) add the liquid shimmer */
static const double COMB_MULT[NCOMB]  = { 1.0, 1.0, 0.5, 2.0 };
static const double COMB_CENTS[NCOMB] = { -3.0, +3.0, -1.5, +2.0 };

/* Build every per-block quantity from one macro value + f0.  Called once per
 * block (only when the macro / f0 change), never per sample. */
static void build_block(t_alien_liquid1 *x, double mac, double f0)
{
    double sr = x->x_sr, nyq = sr * 0.48;
    double t, base;
    int i;

    if (!(mac == mac)) mac = 0.0;                    /* NaN guard              */
    /* macro 0..100 clamped: 0 = still/pluck, 100 = liveliest (100 is the
     * maximum, not a wrap-around back to 0). */
    if (mac < 0.0)   mac = 0.0;
    if (mac > 100.0) mac = 100.0;
    t = mac * 0.01;
    if (!(t >= 0.0 && t <= 1.0)) t = 0.0;             /* t in [0,1]            */

    if (!(f0 == f0)) f0 = 0.01;
    if (f0 < 0.01)   f0 = 0.01;
    if (f0 > 8000.0) f0 = 8000.0;

    /* ---- exciter: a small summed-harmonic saw driven at f0 ---- */
    x->x_exinc = f0 / sr;
    x->x_exbright = 0.25 + 0.7 * t;                  /* dark -> brighter feed  */
    for (i = 0; i < NHARM; i++) {
        double hf = f0 * (double)(i + 1);
        if (hf > nyq || hf < 1.0) {
            x->x_hinc[i] = 0.0; x->x_hamp[i] = 0.0;
        } else {
            /* saw-ish 1/n roll, tilted by brightness so higher macro feeds
             * the resonators a touch more harmonic energy */
            double a = 1.0 / (double)(i + 1);
            a *= pow((double)(i + 1), -(1.0 - x->x_exbright));
            if (!(a == a) || a < 0.0) a = 0.0;
            x->x_hinc[i] = hf / sr;
            x->x_hamp[i] = a;
        }
    }
    /* exciter level: strong pluck attack at 0, easing to a modest steady feed
     * as the resonators take over the sustain.  Trimmed to keep peaks < 1.   */
    x->x_exlevel = 0.090 + 0.038 * (1.0 - t);
    x->x_burstamt = 0.30 * (1.0 - t) * (1.0 - t);    /* pluck click; fade w/ t */

    /* ---- tuned comb resonators: length = sr/f0 * unison ratio ---- */
    base = sr / f0;
    for (i = 0; i < NCOMB; i++) {
        double mult = COMB_MULT[i];
        double det  = pow(2.0, COMB_CENTS[i] / 1200.0);
        double d    = base * mult / det;             /* longer delay = lower det*/
        if (d < 1.0)        d = 1.0;
        if (d > DLEN - 2.0) d = DLEN - 2.0;
        x->x_cdelay[i] = d;
    }

    /* ---- loop feedback: 0 -> short damped pluck, rising -> long sustain ----
     * strictly < 1; the loop lowpass guarantees decay regardless */
    {
        double fb = 0.70 + 0.21 * t;                 /* 0.70 .. ~0.91          */
        if (fb > 0.93) fb = 0.93;                    /* strict < 1             */
        for (i = 0; i < NCOMB; i++) {
            /* the octave-up comb (mult 2.0) sees a shorter loop -> trim its
             * feedback so it doesn't ring hotter than the fundamental combs */
            double g = fb;
            if (COMB_MULT[i] > 1.5) g *= 0.94;
            if (g > 0.985) g = 0.985;
            x->x_cfb[i] = g;
        }
    }

    /* ---- loop damping lowpass: y += damp*(x - y) ----
     * lower damp = darker (more loss of highs each pass) -> more tonal/plucked.
     * as macro rises we let more highs survive (brighter, singing) but never
     * enough to accumulate hiss */
    x->x_damp = 0.22 + 0.42 * t;                     /* 0.22 (dark) -> 0.64    */
    if (x->x_damp > 0.66) x->x_damp = 0.66;          /* always real HF loss    */

    /* ================= modulation engine setup =================
     * depth AND rate scale with macro: near-still at 0, lively at 100.
     * The pitch drift is kept tiny (a few cents) so it stays on the note. */
    {
        double md = t * t;                           /* depth curve (gentle low)*/
        double mr = 0.30 + 0.70 * t;                 /* rate factor 0.30..1.0  */
        x->x_moddepth = md;
        x->x_modrate  = mr;
        /* per-comb slow, desynchronised pitch-drift LFOs (~0.15..0.9 Hz) */
        for (i = 0; i < NCOMB; i++) {
            double phz = (0.23 + 0.17 * (double)i) * mr;     /* Hz             */
            double dhz = (0.17 + 0.11 * (double)i) * mr;     /* wobble Hz      */
            x->x_cinc[i] = phz / sr;
            x->x_dinc[i] = dhz / sr;
        }
        /* 3 global slow LFOs, prime-ish slow rates (~0.1..0.4 Hz) */
        x->x_ginc[0] = (0.37 * mr) / sr;
        x->x_ginc[1] = (0.23 * mr) / sr;
        x->x_ginc[2] = (0.13 * mr) / sr;
        /* random-walk retarget step (per-sample probability proxy) */
        x->x_walkinc = 0.0009 * mr;
    }

    /* ---- allpass diffusion on the wet signal ---- */
    x->x_apcoef = 0.55 + 0.15 * t;                   /* denser as it opens     */
    x->x_apmix  = (t < 0.10) ? 0.0 : (t - 0.10) / 0.90;
    for (i = 0; i < NAP; i++) {
        /* small prime-ish delays, tied loosely to pitch period for musical
         * spacing but capped so diffusion stays short and phase-only */
        double ad = base * (0.03 + 0.02 * (double)i) + 13.0 + 7.0 * i;
        int adi = (int)ad;
        if (adi < 1)          adi = 1;
        if (adi > APLEN - 2)  adi = APLEN - 2;
        x->x_apdelay[i] = adi;
    }

    /* ---- output mix: mostly resonator (wet), a little exciter body ---- */
    x->x_wet = 0.60 + 0.30 * t;                      /* 0.60 -> 0.90           */

    /* ---- stash un-modulated base values; the per-sample modulator
     * perturbs the smoothed copies around these so nothing drifts away ---- */
    for (i = 0; i < NCOMB; i++) {
        x->x_cdelay0[i] = x->x_cdelay[i];
        x->x_cfb0[i]    = x->x_cfb[i];
    }
    x->x_damp0    = x->x_damp;
    x->x_wet0     = x->x_wet;
    x->x_apmix0   = x->x_apmix;
    x->x_exlevel0 = x->x_exlevel;

    if (!x->x_haveblock) {                            /* seed smoothed copies   */
        for (i = 0; i < NCOMB; i++) {
            x->x_cdelay_s[i] = x->x_cdelay0[i];
            x->x_cfb_s[i]    = x->x_cfb0[i];
        }
        x->x_damp_s    = x->x_damp0;
        x->x_wet_s     = x->x_wet0;
        x->x_apmix_s   = x->x_apmix0;
        x->x_exlevel_s = x->x_exlevel0;
    }

    x->x_lastmac = mac;
    x->x_lastf0  = f0;
    x->x_haveblock = 1;
}

static t_int *alien_liquid1_perform(t_int *w)
{
    t_alien_liquid1 *x = (t_alien_liquid1 *)(w[1]);
    t_sample *fin = (t_sample *)(w[2]);
    t_sample *min = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    int nsamp = (int)(w[5]);

    double mac0 = min[0], f00 = fin[0];
    int i, c, k;
    double burstamt, apcoef;
    double exinc, moddepth, slew;

    if (!x->x_haveblock || mac0 != x->x_lastmac || f00 != x->x_lastf0)
        build_block(x, mac0, f00);

    burstamt = x->x_burstamt;
    apcoef = x->x_apcoef;
    exinc = x->x_exinc;
    moddepth = x->x_moddepth;

    /* ============ block-rate modulation update ============
     * Sample the (per-sample-advanced) modulator phases as they stand at the
     * start of this block, form the modulated targets, and slew the smoothed
     * coeffs toward them.  Sub-Hz LFOs + short blocks => zipper-free. */
    {
        /* global modulators */
        double g0 = sin(x->x_glfo[0] * 2.0 * M_PI);
        double g1 = sin(x->x_glfo[1] * 2.0 * M_PI);
        double g2 = sin(x->x_glfo[2] * 2.0 * M_PI);
        double wk = x->x_walk;                          /* smoothed random walk */

        /* --- brightness (damping) : global g0 + walk --- */
        double dtgt = x->x_damp0 * (1.0 + moddepth * (0.34 * g0 + 0.18 * wk));
        if (dtgt < 0.08) dtgt = 0.08;
        if (dtgt > 0.66) dtgt = 0.66;                   /* always real HF loss  */

        /* --- exciter balance : global g1 --- */
        double etgt = x->x_exlevel0 * (1.0 + moddepth * 0.35 * g1);
        if (etgt < 0.0)  etgt = 0.0;
        if (etgt > 0.30) etgt = 0.30;

        /* --- diffusion amount : global g2 --- */
        double aptgt = x->x_apmix0 + moddepth * 0.30 * g2;
        if (aptgt < 0.0) aptgt = 0.0;
        if (aptgt > 1.0) aptgt = 1.0;

        /* --- overall wet / feedback : g0/g2 blend + walk (feedback stays<1) */
        double wtgt = x->x_wet0 + moddepth * 0.10 * (0.5 * g2 + 0.5 * wk);
        if (wtgt < 0.0) wtgt = 0.0;
        if (wtgt > 0.95) wtgt = 0.95;

        /* block-rate slew of the smoothed copies */
        slew = 0.35;
        x->x_damp_s    += slew * (dtgt  - x->x_damp_s);
        x->x_exlevel_s += slew * (etgt  - x->x_exlevel_s);
        x->x_apmix_s   += slew * (aptgt - x->x_apmix_s);
        x->x_wet_s     += slew * (wtgt  - x->x_wet_s);

        for (c = 0; c < NCOMB; c++) {
            /* per-comb pitch micro-drift: a few cents, so it stays harmonic.
             * cents = +-(3..5) * depth; convert to a delay-length ratio. */
            double dc  = sin(x->x_cph[c] * 2.0 * M_PI);
            double wob = sin(x->x_dph[c] * 2.0 * M_PI);
            double cents = moddepth * (7.0 + 1.5 * (double)c) * dc;
            double ratio = pow(2.0, -cents / 1200.0);   /* +cents -> shorter    */
            double dtgtc = x->x_cdelay0[c] * ratio;
            double ftgtc;
            if (dtgtc < 1.0)        dtgtc = 1.0;
            if (dtgtc > DLEN - 2.0) dtgtc = DLEN - 2.0;
            /* per-comb feedback wobble (kept strictly < 1) */
            ftgtc = x->x_cfb0[c] * (1.0 + moddepth * 0.045 * wob);
            if (ftgtc > 0.985) ftgtc = 0.985;
            if (ftgtc < 0.0)   ftgtc = 0.0;
            x->x_cdelay_s[c] += slew * (dtgtc - x->x_cdelay_s[c]);
            x->x_cfb_s[c]    += slew * (ftgtc - x->x_cfb_s[c]);
        }
    }

    while (nsamp--) {
        double ex = 0.0, burst, exc, wetsig, dry, o, ap;
        double damp = x->x_damp_s, wet = x->x_wet_s, apmix = x->x_apmix_s;
        double exlevel = x->x_exlevel_s;
        fin++; min++;                                  /* read per block only   */

        /* ---- advance modulator phases (cheap, per sample) ---- */
        for (i = 0; i < NCOMB; i++) {
            x->x_cph[i] += x->x_cinc[i]; if (x->x_cph[i] >= 1.0) x->x_cph[i] -= 1.0;
            x->x_dph[i] += x->x_dinc[i]; if (x->x_dph[i] >= 1.0) x->x_dph[i] -= 1.0;
        }
        x->x_glfo[0] += x->x_ginc[0]; if (x->x_glfo[0] >= 1.0) x->x_glfo[0] -= 1.0;
        x->x_glfo[1] += x->x_ginc[1]; if (x->x_glfo[1] >= 1.0) x->x_glfo[1] -= 1.0;
        x->x_glfo[2] += x->x_ginc[2]; if (x->x_glfo[2] >= 1.0) x->x_glfo[2] -= 1.0;
        /* smoothed random-walk: occasional new target, one-pole toward it */
        x->x_wrng = x->x_wrng * 1664525u + 1013904223u;
        if (((double)(x->x_wrng >> 8) * (1.0 / 16777216.0)) < x->x_walkinc) {
            double rr = ((double)(x->x_wrng >> 9) * (1.0 / 8388608.0)) - 1.0;
            x->x_walktarget = rr;                       /* new target [-1,1)    */
        }
        x->x_walk = flushd(x->x_walk + 0.0025 * (x->x_walktarget - x->x_walk));

        /* ---- exciter: summed harmonics (saw-ish) ---- */
        for (i = 0; i < NHARM; i++) {
            if (x->x_hamp[i] == 0.0) continue;
            x->x_hph[i] += x->x_hinc[i];
            if (x->x_hph[i] >= 1.0) x->x_hph[i] -= 1.0;
            ex += x->x_hamp[i] * sin(x->x_hph[i] * 2.0 * M_PI);
        }

        /* ---- re-triggered filtered-noise impulse burst (per period) ---- */
        x->x_exph += exinc;
        if (x->x_exph >= 1.0) { x->x_exph -= 1.0; x->x_burst = 1.0; }
        x->x_rng = x->x_rng * 1664525u + 1013904223u;
        {
            double wn = ((double)(x->x_rng >> 9) * (1.0 / 8388608.0)) - 1.0; /* [-1,1) */
            /* band-limit the noise so the burst never injects broadband hiss */
            x->x_noiselp = flushd(x->x_noiselp + 0.20 * (wn - x->x_noiselp));
            burst = x->x_noiselp * x->x_burst * burstamt;
        }
        x->x_burst *= 0.90;                            /* short click, decays   */
        if (x->x_burst < 1e-6) x->x_burst = 0.0;

        exc = (ex + burst) * exlevel;

        /* ---- tuned comb resonators with in-loop damping ---- */
        wetsig = 0.0;
        for (c = 0; c < NCOMB; c++) {
            double r = delread(x, c, x->x_cdelay_s[c]);
            double lp, wr;
            /* Karplus-Strong damping lowpass inside the loop */
            lp = x->x_damplp[c] + damp * (r - x->x_damplp[c]);
            x->x_damplp[c] = flushd(lp);
            wr = exc + x->x_cfb_s[c] * lp;             /* feed + damped fb      */
            x->x_buf[c][x->x_wi[c]] = flushd(wr);
            x->x_wi[c] = (x->x_wi[c] + 1) & DMASK;
            wetsig += r;
        }
        wetsig *= (1.0 / (double)NCOMB);

        /* ---- allpass diffusion (linear, phase-only) on the wet signal ---- */
        ap = wetsig;
        for (k = 0; k < NAP; k++) {
            int rd = (x->x_apwi[k] - x->x_apdelay[k]) & APMASK;
            double bufv = x->x_apbuf[k][rd];
            double in = ap;
            double y = bufv - apcoef * in;             /* allpass output        */
            x->x_apbuf[k][x->x_apwi[k]] = flushd(in + apcoef * y);
            x->x_apwi[k] = (x->x_apwi[k] + 1) & APMASK;
            ap = y;
        }
        wetsig = wetsig * (1.0 - apmix) + ap * apmix;

        /* ---- mix + output safety ---- */
        dry = exc;
        o = dry * (1.0 - wet) + wetsig * wet;
        if (!(o > -1.5 && o < 1.5)) {                  /* catches NaN/Inf too   */
            o = softclip(o);
            if (!(o > -1.5 && o < 1.5)) o = 0.0;
        }
        *out++ = (t_sample)(o * 0.6448);               /* trimmed: peak < 0.95   */
    }

    /* keep long-lived feedback state clean between blocks */
    for (c = 0; c < NCOMB; c++) x->x_damplp[c] = flushd(x->x_damplp[c]);
    x->x_noiselp = flushd(x->x_noiselp);
    x->x_walk = flushd(x->x_walk);
    return (w + 6);
}

static void alien_liquid1_dsp(t_alien_liquid1 *x, t_signal **sp)
{
    if (sp[0]->s_sr > 1000) x->x_sr = sp[0]->s_sr;
    dsp_add(alien_liquid1_perform, 5, x,
            sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_liquid1_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_liquid1 *x = (t_alien_liquid1 *)pd_new(alien_liquid1_tilde_class);
    int i, c;
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;

    x->x_exph = 0.0;
    for (i = 0; i < NHARM; i++) {
        x->x_hph[i]  = (double)i * 0.0137;
        x->x_hinc[i] = 0.0; x->x_hamp[i] = 0.0;
    }
    x->x_burst = 0.0;
    x->x_rng = 22222u;
    x->x_noiselp = 0.0;

    /* ---- modulation engine init (desynchronised start phases) ---- */
    for (i = 0; i < NCOMB; i++) {
        x->x_cph[i] = (double)i * 0.211 + 0.05;
        x->x_dph[i] = (double)i * 0.373 + 0.17;
        x->x_cinc[i] = 0.0;
        x->x_dinc[i] = 0.0;
    }
    x->x_glfo[0] = 0.10; x->x_glfo[1] = 0.53; x->x_glfo[2] = 0.81;
    x->x_ginc[0] = x->x_ginc[1] = x->x_ginc[2] = 0.0;
    x->x_walk = 0.0; x->x_walktarget = 0.0; x->x_wrng = 987654321u;
    x->x_moddepth = 0.0; x->x_modrate = 0.3; x->x_walkinc = 0.0;

    for (c = 0; c < NCOMB; c++) {
        memset(x->x_buf[c], 0, sizeof(x->x_buf[c]));
        x->x_wi[c] = 0;
        x->x_damplp[c] = 0.0;
        x->x_cdelay[c] = 100.0;
        x->x_cfb[c] = 0.0;
        x->x_cdelay0[c] = 100.0;
        x->x_cfb0[c] = 0.0;
        x->x_cdelay_s[c] = 100.0;
        x->x_cfb_s[c] = 0.0;
    }
    x->x_damp0 = 0.5; x->x_wet0 = 0.6; x->x_apmix0 = 0.0; x->x_exlevel0 = 0.0;
    x->x_damp_s = 0.5; x->x_wet_s = 0.6; x->x_apmix_s = 0.0; x->x_exlevel_s = 0.0;
    for (i = 0; i < NAP; i++) {
        memset(x->x_apbuf[i], 0, sizeof(x->x_apbuf[i]));
        x->x_apwi[i] = 0;
        x->x_apdelay[i] = 17 + 7 * i;
    }

    x->x_damp = 0.5; x->x_exlevel = 0.0; x->x_exbright = 0.5;
    x->x_burstamt = 0.0; x->x_apcoef = 0.55; x->x_apmix = 0.0;
    x->x_wet = 0.6; x->x_exinc = 0.0;

    x->x_haveblock = 0;
    x->x_lastmac = -1e30;
    x->x_lastf0  = -1e30;

    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_liquid1_tilde_setup(void)
{
    alien_liquid1_tilde_class = class_new(gensym("alien_liquid1~"),
        (t_newmethod)alien_liquid1_new, 0, sizeof(t_alien_liquid1),
        CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_liquid1_tilde_class, t_alien_liquid1, x_f);
    class_addmethod(alien_liquid1_tilde_class, (t_method)alien_liquid1_dsp,
                    gensym("dsp"), A_CANT, 0);
}
