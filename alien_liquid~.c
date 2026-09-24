/* alien_liquid~.c  --  an additive swarm poured through a phaser and its own
 *                      feedback delay, so the spectrum never sits still
 *
 * Built on the alien_stack~ additive engine (a stack of partials from a
 * harmonic / fifths / bell ratio set), but where alien_stack~ just sums the
 * partials, this pours them through three coupled processes that generate
 * emergent, "liquid" harmonic complexity you cannot get from the additive
 * spectrum alone:
 *
 *   1. SWARM.  Every partial has its own slow detune LFO at its own rate, so
 *      the partials continuously phase against each other -- a deep, non-
 *      periodic chorus that already smears the spectrum before any effect.
 *
 *   2. PHASER.  A 6-stage allpass chain whose notches are swept by an LFO,
 *      with resonant feedback around the chain -- classic phasing / vowel
 *      movement that sharpens as the macro opens.
 *
 *   3. LIQUID FEEDBACK DELAY.  One delay line, read by TWO modulated taps and
 *      fed back through a soft saturator:
 *        - a short, fast-modulated tap  -> flanging / comb resonance,
 *        - a long, slowly-drifting tap  -> the line SAMPLES ITS OWN OUTPUT and
 *          re-injects it time-smeared and slightly pitch-shifted, which is
 *          where the bizarre, evolving, self-referential timbres come from.
 *      Feedback is soft-clipped and bounded < 1, so it churns forever without
 *      ever blowing up.
 *
 *   inlet 0   signal/float   fundamental in Hz
 *   inlet 1   signal/float   MACRO 0..100 (wrapped mod 100) -- one knob that
 *                            morphs the whole voice: at 0 a clean, gently
 *                            chorused additive stack; rising, the swarm widens,
 *                            the phaser engages and sweeps faster, the ratio set
 *                            turns inharmonic, and the feedback delay fills in
 *                            with flanging then long self-sampled tails; at 100
 *                            it is a churning, saturated, phasing liquid that
 *                            keeps generating new harmonics from its own echoes.
 *
 *   outlet 0  signal
 *
 * All the expensive/discrete work (ratio set, partial tables, effect
 * coefficients) is done once per block; the per-sample loop is just the
 * oscillator sum, the allpass chain and two interpolated delay reads.  Every
 * feedback path is soft-clipped, denormal-flushed and bounded, and the output
 * is guarded finite -- so it is safe under audio-rate macro modulation.
 *
 * build:
 *   gcc -shared -fPIC -O2 -o alien_liquid~.pd_linux alien_liquid~.c -lm
 *   (macOS: -bundle -undefined dynamic_lookup -o alien_liquid~.pd_darwin)
 */

#include "m_pd.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAXP  48         /* array capacity for partials                       */
#define MAXN  28         /* macro may request at most this many (<= MAXP)     */
#define NAP   6          /* phaser allpass stages                             */
#define DLEN  32768      /* delay line length (power of 2), ~0.74 s @ 44.1k   */
#define DMASK (DLEN - 1)

static t_class *alien_liquid_tilde_class;

typedef struct _alien_liquid {
    t_object x_obj;
    t_float  x_f;

    double   x_sr;

    /* ---- additive swarm ---- */
    double   x_ph[MAXP];        /* partial phases                             */
    double   x_chph[MAXP];      /* per-partial chorus LFO phases              */
    double   x_ratio[MAXP];
    int      x_lastmode;

    /* ---- per-partial "spectral boiling" animation ---- */
    double   x_trph[MAXP];      /* per-partial tremolo LFO phase              */
    double   x_trinc[MAXP];     /* per-partial tremolo LFO increment          */
    double   x_drph[MAXP];      /* per-partial extra-detune LFO phase         */
    double   x_drinc[MAXP];     /* per-partial extra-detune LFO increment     */

    /* ---- global slow modulation engine ---- */
    double   x_g1;              /* slow LFO 1 phase                           */
    double   x_g2;              /* slow LFO 2 phase                           */
    double   x_g3;              /* slow LFO 3 phase                           */
    double   x_g1inc, x_g2inc, x_g3inc;
    double   x_rw;              /* random-walk value (bounded [-1,1])         */
    double   x_rwtar;           /* random-walk target                         */
    double   x_rwcnt;           /* samples until next random-walk target      */
    unsigned int x_rng;         /* xorshift rng state                         */
    double   x_trdepth;         /* per-partial tremolo depth (scaled by liv)  */
    double   x_drdepth;         /* per-partial extra-detune depth (cents-ish) */
    double   x_liv;             /* liveliness amount 0..1 (un-wrapped macro)  */

    /* ---- phaser ---- */
    double   x_ap[NAP];         /* allpass stage states                       */
    double   x_phlast;          /* phaser feedback state                      */
    double   x_phlfo;           /* phaser sweep LFO phase                     */

    /* ---- liquid feedback delay ---- */
    double   x_buf[DLEN];
    int      x_wi;              /* write index                                */
    double   x_clfo;            /* short-tap (comb) mod LFO phase             */
    double   x_llfo;            /* long-tap (sampler) drift LFO phase         */

    /* ---- per-block cache (built once per block) ---- */
    int      x_n;
    double   x_amp[MAXP];
    double   x_finc[MAXP];
    double   x_active[MAXP];
    double   x_chinc[MAXP];
    double   x_norm;
    double   x_chdepth;         /* swarm detune depth                         */

    double   x_phmix;           /* dry->phaser mix          0..1              */
    double   x_phdepth;         /* allpass sweep depth      0..~0.85          */
    double   x_phfb;            /* phaser resonance         0..~0.6           */
    double   x_phinc;           /* phaser LFO increment                       */

    double   x_combD;           /* short tap base delay (samples)             */
    double   x_combmod;         /* short tap mod depth (samples)              */
    double   x_cinc;            /* short-tap LFO increment                    */
    double   x_longD;           /* long tap base delay (samples)             */
    double   x_longmod;         /* long tap drift depth (samples)             */
    double   x_linc;            /* long-tap LFO increment                     */
    double   x_samp;            /* long-tap (self-sampling) amount 0..1       */
    double   x_fbk;             /* delay feedback           0..~0.9           */
    double   x_loopdrive;       /* saturation in the loop   0..1              */
    double   x_wet;             /* dry->wet output mix      0..1              */

    /* ---- base (un-modulated) values for global-mod targets ---- */
    double   x_phdepth0;        /* base phaser sweep depth                    */
    double   x_phinc0;          /* base phaser LFO increment                  */
    double   x_longmod0;        /* base long-tap drift depth                  */

    double   x_lastmac;
    double   x_lastf0;
    int      x_haveblock;
} t_alien_liquid;

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

/* cheap deterministic rng -> uniform in [-1,1] */
static inline double frand(unsigned int *s)
{
    unsigned int r = *s;
    r ^= r << 13; r ^= r >> 17; r ^= r << 5;
    *s = r ? r : 0x9e3779b9u;
    return (double)(*s) * (2.0 / 4294967296.0) - 1.0;
}

/* ------------------------------------------------------------------ */

#define NMODE 3
static const double MODE_RATIO[NMODE][MAXP] = {
    {0},   /* 0 harmonic: filled at load time (1,2,3,...) */
    {1,1.5,2.25,3.375,5.0625,7.594,11.39,17.09,25.63,38.44,57.67,86.5}, /* fifths */
    {1,2.00,3.01,4.15,5.43,6.79,8.21,9.33,10.9,12.4,14.1,15.9}          /* bell   */
};

static void load_mode(t_alien_liquid *x, int mode)
{
    int i;
    if (mode == 0) for (i = 0; i < MAXP; i++) x->x_ratio[i] = i + 1;
    else           for (i = 0; i < MAXP; i++) x->x_ratio[i] = MODE_RATIO[mode][i];
    x->x_lastmode = mode;
}

/* interpolated read from the delay line, `d` samples behind the write head */
static inline double delread(t_alien_liquid *x, double d)
{
    double rp;
    int i0, i1;
    double fr;
    if (d < 1.0)        d = 1.0;
    if (d > DLEN - 2.0) d = DLEN - 2.0;
    rp = (double)x->x_wi - d;
    while (rp < 0.0) rp += (double)DLEN;
    i0 = (int)rp;
    fr = rp - (double)i0;
    i1 = (i0 + 1) & DMASK;
    i0 &= DMASK;
    return x->x_buf[i0] * (1.0 - fr) + x->x_buf[i1] * fr;
}

/* Build every per-block quantity from one macro value + f0.  Called once per
 * block (only when the macro / f0 change), never per sample. */
static void build_block(t_alien_liquid *x, double mac, double f0)
{
    double sr = x->x_sr, nyq = sr * 0.48;
    double t, stretch, tilt, odd, chrate;
    double sum = 0.0;
    int n, mode, i;

    double liv;

    if (!(mac == mac)) mac = 0.0;                    /* NaN guard              */
    t = mac - 100.0 * floor(mac * 0.01);
    if (!(t >= 0.0 && t <= 100.0)) t = 0.0;
    t *= 0.01;                                       /* t in [0,1)            */

    /* liveliness amount: un-wrapped macro so 0 -> still, 100 -> lively.  This
     * drives the internal modulation engine DEPTH+RATE independently of the
     * wrapped timbre morph t, so a held note at macro 100 still evolves.     */
    liv = mac * 0.01;
    if (!(liv >= 0.0)) liv = 0.0;
    if (liv > 1.0)     liv = 1.0;
    x->x_liv = liv;

    if (!(f0 == f0)) f0 = 0.01;
    if (f0 < 0.01)   f0 = 0.01;
    if (f0 > 8000.0) f0 = 8000.0;

    /* ---- additive spectrum (curated morph) ---- */
    mode = (t < 0.40) ? 0 : (t < 0.72 ? 1 : 2);      /* -> more inharmonic     */
    if (mode != x->x_lastmode) load_mode(x, mode);

    n = (int)(4.0 + 20.0 * t);                       /* 4..24 partials         */
    if (n < 1)    n = 1;
    if (n > MAXN) n = MAXN;

    stretch = 1.0 + 0.6 * t * t;                     /* neutral -> spread bell */
    tilt    = -1.0 + 2.5 * t;                        /* dark -> bright         */
    odd     = (t < 0.75) ? 0.0 : (t - 0.75) / 0.25;  /* full -> odd only up top*/

    x->x_chdepth = 0.002 + 0.03 * t;                 /* swarm detune depth     */
    chrate       = 0.05 + 0.9 * t;                   /* swarm slow..quicker    */

    /* per-partial spectral-boiling depths, scaled by liveliness (near 0 at
     * macro 0).  Tremolo depth is a fraction of each partial's amplitude;
     * detune depth is tiny (a few cents) so partials never leave the note.  */
    x->x_trdepth = 0.45 * liv;                        /* up to +-45% shimmer   */
    x->x_drdepth = (0.0004 + 0.0016 * liv);           /* ~1..3 cents drift     */

    x->x_n = n;
    for (i = 0; i < n; i++) {
        double r = x->x_ratio[i], fr, a;
        if (r <= 0.0) { x->x_active[i]=0; x->x_amp[i]=0; x->x_finc[i]=0; x->x_chinc[i]=0; continue; }
        fr = f0 * pow(r, stretch);
        if (fr > nyq || fr < 1.0) { x->x_active[i]=0; x->x_amp[i]=0; x->x_finc[i]=0; x->x_chinc[i]=0; continue; }
        a = 1.0 / (double)(i + 1);
        if (tilt != 0.0)          a *= pow(r, -tilt);
        if (odd > 0.0 && (i & 1)) a *= 1.0 - odd;
        if (!(a == a) || a < 0.0) a = 0.0;
        x->x_active[i] = 1.0;
        x->x_amp[i]    = a;
        x->x_finc[i]   = fr / sr;
        /* each partial drifts at its own rate -> continuous inter-partial phasing */
        x->x_chinc[i]  = chrate * (0.37 + 0.63 * (double)((i * 7) % 11) / 11.0) / sr;

        /* per-partial animation rates: slow, sub-Hz, desynchronised via an
         * irrational base (golden ratio) times coprime multipliers so the
         * shimmer of neighbouring partials never phase-locks.  Rate itself
         * also opens a little with liveliness so macro 100 boils faster.    */
        {
            double rr = 0.05 + 0.35 * liv;            /* ~0.05..0.4 Hz base    */
            double gm = 0.6180339887498949;           /* golden-ratio spacing  */
            double tm = 1.0 + gm * (double)((i * 5 + 1) % 13);   /* tremolo    */
            double dm = 1.0 + gm * (double)((i * 3 + 2) % 17);   /* detune     */
            x->x_trinc[i] = rr * tm / sr;
            x->x_drinc[i] = (0.6 * rr) * dm / sr;     /* detune drifts slower  */
        }
        sum += a;
    }
    x->x_norm = (sum > 1e-6) ? (1.0 / sum) : 1.0;

    /* ---- phaser ---- */
    x->x_phmix   = (t < 0.15) ? 0.0 : (t - 0.15) / 0.85;   /* fade in           */
    x->x_phdepth0 = 0.55 + 0.30 * t;                       /* sweep width       */
    x->x_phdepth = x->x_phdepth0;
    x->x_phfb    = 0.60 * x->x_phmix;                      /* resonance grows   */
    x->x_phinc0  = (0.08 + 1.6 * t) / sr;                  /* 0.08..1.7 Hz      */
    x->x_phinc   = x->x_phinc0;

    /* ---- liquid feedback delay ---- */
    x->x_combD   = 180.0 + 300.0 * (1.0 - t);              /* shorter as t rises */
    x->x_combmod = 60.0 + 260.0 * t;                       /* deeper flange     */
    x->x_cinc    = (0.05 + 0.8 * t) / sr;
    x->x_longD   = 3200.0 + 15000.0 * (0.3 + 0.7 * t);     /* long re-inject tap */
    x->x_longmod0 = 400.0 + 4000.0 * t;                    /* slow pitch smear  */
    x->x_longmod = x->x_longmod0;
    x->x_linc    = (0.017 + 0.10 * t) / sr;
    x->x_samp    = (t < 0.30) ? 0.0 : (t - 0.30) / 0.70;   /* self-sampling amt */
    x->x_fbk     = 0.35 * t + 0.52 * t * t;                /* 0 .. ~0.87        */
    if (x->x_fbk > 0.88) x->x_fbk = 0.88;                  /* strict < 1        */
    x->x_loopdrive = (t < 0.5) ? 0.0 : (t - 0.5) / 0.5;
    x->x_wet     = 0.10 + 0.65 * t;                        /* dry -> wet        */

    /* ---- global slow-modulation engine rates ----
     * 3 sub-Hz LFOs at incommensurate rates + a smoothed random walk.  Rates
     * open a touch with liveliness (all stay well under ~1 Hz).             */
    x->x_g1inc = (0.033 + 0.20 * liv) / sr;               /* ~0.03..0.23 Hz    */
    x->x_g2inc = (0.051 + 0.31 * liv) / sr;               /* ~0.05..0.36 Hz    */
    x->x_g3inc = (0.071 + 0.52 * liv) / sr;               /* ~0.07..0.59 Hz    */

    x->x_lastmac = mac;
    x->x_lastf0  = f0;
    x->x_haveblock = 1;
}

static t_int *alien_liquid_perform(t_int *w)
{
    t_alien_liquid *x = (t_alien_liquid *)(w[1]);
    t_sample *fin = (t_sample *)(w[2]);
    t_sample *min = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    int nsamp = (int)(w[5]);

    double mac0 = min[0], f00 = fin[0];
    int n, i, k;
    double norm, chdepth, phmix, phdepth0, phfb, phinc0;
    double combD, combmod, cinc, longD, longmod0, linc, samp, fbk, ldr, wet;
    double trdepth, drdepth, liv, g1inc, g2inc, g3inc;

    if (!x->x_haveblock || mac0 != x->x_lastmac || f00 != x->x_lastf0)
        build_block(x, mac0, f00);

    n = x->x_n; norm = x->x_norm; chdepth = x->x_chdepth;
    phmix = x->x_phmix; phdepth0 = x->x_phdepth0; phfb = x->x_phfb; phinc0 = x->x_phinc0;
    combD = x->x_combD; combmod = x->x_combmod; cinc = x->x_cinc;
    longD = x->x_longD; longmod0 = x->x_longmod0; linc = x->x_linc;
    samp = x->x_samp; fbk = x->x_fbk; ldr = x->x_loopdrive; wet = x->x_wet;
    trdepth = x->x_trdepth; drdepth = x->x_drdepth; liv = x->x_liv;
    g1inc = x->x_g1inc; g2inc = x->x_g2inc; g3inc = x->x_g3inc;

    while (nsamp--) {
        double s = 0.0, dry, ph, g, sh, lo, fbsig, wr, o;
        double gmod, gtilt, phdepth, phinc, longmod;
        fin++; min++;                                  /* read per block only   */

        /* ---- global slow-modulation engine (cheap per-sample advance) ----
         * 3 incommensurate sub-Hz LFOs plus a smoothed random walk, blended
         * into one gentle signal `gmod` in ~[-1,1] with no obvious period.  */
        x->x_g1 += g1inc; if (x->x_g1 >= 1.0) x->x_g1 -= 1.0;
        x->x_g2 += g2inc; if (x->x_g2 >= 1.0) x->x_g2 -= 1.0;
        x->x_g3 += g3inc; if (x->x_g3 >= 1.0) x->x_g3 -= 1.0;
        x->x_rwcnt -= 1.0;
        if (x->x_rwcnt <= 0.0) {                        /* pick a new RW target  */
            x->x_rwtar = frand(&x->x_rng);
            x->x_rwcnt = (0.7 + 0.8 * frand(&x->x_rng)) * x->x_sr; /* ~0.7..1.5 s */
        }
        x->x_rw += (x->x_rwtar - x->x_rw) * 0.0009;     /* glide, then dwells    */
        gmod = 0.34 * sin(x->x_g1 * 2.0 * M_PI)
             + 0.28 * sin(x->x_g2 * 2.0 * M_PI)
             + 0.22 * sin(x->x_g3 * 2.0 * M_PI)
             + 0.30 * x->x_rw;                          /* combined, ~[-1.14,1.14] */

        /* route the wanderer onto a few timbre params, scaled by liveliness,
         * around their per-block base values (kept within their safe ranges) */
        phdepth = phdepth0 + 0.18 * liv * gmod;
        if (phdepth > 0.85) phdepth = 0.85;             /* keep allpass stable   */
        if (phdepth < 0.0)  phdepth = 0.0;
        phinc   = phinc0 * (1.0 + 0.5 * liv * gmod);    /* sweep rate wanders    */
        if (phinc < 0.0) phinc = 0.0;
        longmod = longmod0 * (1.0 + 0.35 * liv * gmod); /* delay drift depth     */
        if (longmod < 0.0) longmod = 0.0;

        /* slow spectral-tilt wander: rocks brightness up/down over the partial
         * index so the centroid genuinely drifts even on a bare stack.  Small
         * per-octave slope, scaled by liveliness (near flat at macro 0).      */
        gtilt = 0.34 * liv * (0.6 * sin(x->x_g3 * 2.0 * M_PI)
                            + 0.25 * sin(x->x_g1 * 2.0 * M_PI)
                            + 0.15 * x->x_rw);

        /* ---- additive swarm ---- */
        for (i = 0; i < n; i++) {
            double det, tr, gw;
            if (x->x_active[i] == 0.0) continue;
            /* swarm chorus (existing) */
            x->x_chph[i] += x->x_chinc[i];
            if (x->x_chph[i] >= 1.0) x->x_chph[i] -= 1.0;
            det = 1.0 + chdepth * sin(x->x_chph[i] * 2.0 * M_PI);
            /* per-partial extra micro-detune drift (a few cents, controlled) */
            x->x_drph[i] += x->x_drinc[i];
            if (x->x_drph[i] >= 1.0) x->x_drph[i] -= 1.0;
            det += drdepth * sin(x->x_drph[i] * 2.0 * M_PI);
            x->x_ph[i] += x->x_finc[i] * det;
            if (x->x_ph[i] >= 1.0) x->x_ph[i] -= 1.0;
            else if (x->x_ph[i] < 0.0) x->x_ph[i] += 1.0;
            /* per-partial amplitude shimmer (tremolo), desynchronised */
            x->x_trph[i] += x->x_trinc[i];
            if (x->x_trph[i] >= 1.0) x->x_trph[i] -= 1.0;
            tr = 1.0 + trdepth * sin(x->x_trph[i] * 2.0 * M_PI);
            /* global tilt wander: emphasise low or high partials over time */
            gw = 1.0 + gtilt * ((double)i - 0.5 * (double)(n - 1));
            if (gw < 0.0) gw = 0.0;
            s += x->x_amp[i] * tr * gw * sin(x->x_ph[i] * 2.0 * M_PI);
        }
        s *= norm;
        dry = s;

        /* ---- phaser: 6 swept allpass stages with resonant feedback ---- */
        x->x_phlfo += phinc;
        if (x->x_phlfo >= 1.0) x->x_phlfo -= 1.0;
        g = phdepth * sin(x->x_phlfo * 2.0 * M_PI);    /* |g| < 0.85 -> stable  */
        ph = s + phfb * x->x_phlast;
        for (k = 0; k < NAP; k++) {
            double apout = -g * ph + x->x_ap[k];
            x->x_ap[k] = flushd(ph + g * apout);
            ph = apout;
        }
        x->x_phlast = flushd(ph);
        s = s * (1.0 - phmix) + ph * phmix;            /* blend in the phasing  */

        /* ---- liquid feedback delay: short comb tap + long self-sampled tap ---- */
        x->x_clfo += cinc; if (x->x_clfo >= 1.0) x->x_clfo -= 1.0;
        x->x_llfo += linc; if (x->x_llfo >= 1.0) x->x_llfo -= 1.0;
        sh = delread(x, combD + combmod * sin(x->x_clfo * 2.0 * M_PI));   /* flange */
        lo = delread(x, longD + longmod * sin(x->x_llfo * 2.0 * M_PI));   /* sampler */
        fbsig = sh * (1.0 - samp) + lo * samp;         /* convex -> bounded     */

        /* write input + saturated feedback back into the line */
        wr = s + fbk * softclip(fbsig * (1.0 + ldr * 2.0));
        x->x_buf[x->x_wi] = flushd(wr);
        x->x_wi = (x->x_wi + 1) & DMASK;

        /* ---- mix + output safety ---- */
        o = dry * (1.0 - wet) + fbsig * wet;
        /* gentle global amplitude wander (slow, unipolar so always in phase),
         * scaled by liveliness -> the whole texture breathes at macro 100 even
         * when the timbre is a bare stack.  Depth small: gain stays in ~[0.6,1] */
        {
            double gamp = 1.0 - 0.38 * liv
                        * (1.0 - (0.30 * sin(x->x_g1 * 2.0 * M_PI)
                               + 0.70 * x->x_rw));        /* rw dwells -> slow    */
            if (gamp < 0.3) gamp = 0.3;
            if (gamp > 1.0) gamp = 1.0;
            o *= gamp;
        }
        if (ldr > 0.0) o = softclip(o * (1.0 + ldr * 3.0)) / (1.0 + ldr);
        if (!(o > -1.5 && o < 1.5)) {                  /* catches NaN/Inf too   */
            o = softclip(o);
            if (!(o > -1.5 && o < 1.5)) o = 0.0;
        }
        *out++ = (t_sample)(o * 0.9);
    }

    /* keep long-lived feedback state clean between blocks */
    x->x_phlast = flushd(x->x_phlast);
    for (k = 0; k < NAP; k++) x->x_ap[k] = flushd(x->x_ap[k]);
    return (w + 6);
}

static void alien_liquid_dsp(t_alien_liquid *x, t_signal **sp)
{
    if (sp[0]->s_sr > 1000) x->x_sr = sp[0]->s_sr;
    dsp_add(alien_liquid_perform, 5, x,
            sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_liquid_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_liquid *x = (t_alien_liquid *)pd_new(alien_liquid_tilde_class);
    int i;
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;

    for (i = 0; i < MAXP; i++) {
        x->x_ph[i]   = (double)i * 0.0137;
        x->x_chph[i] = (double)(i * 5 % 13) / 13.0;
        x->x_amp[i] = 0.0; x->x_finc[i] = 0.0;
        x->x_active[i] = 0.0; x->x_chinc[i] = 0.0;
        /* per-partial animation: spread starting phases so shimmer is already
         * desynchronised at note-on (no unison pump)                         */
        x->x_trph[i] = (double)(i * 11 % 17) / 17.0; x->x_trinc[i] = 0.0;
        x->x_drph[i] = (double)(i * 9  % 19) / 19.0; x->x_drinc[i] = 0.0;
    }
    for (i = 0; i < NAP; i++) x->x_ap[i] = 0.0;
    memset(x->x_buf, 0, sizeof(x->x_buf));
    x->x_wi = 0;
    x->x_phlast = x->x_phlfo = x->x_clfo = x->x_llfo = 0.0;
    x->x_lastmode = -1;
    load_mode(x, 0);
    x->x_haveblock = 0;
    x->x_lastmac = -1e30;
    x->x_lastf0  = -1e30;
    x->x_n = 0; x->x_norm = 1.0; x->x_chdepth = 0.0;
    x->x_phmix = x->x_phdepth = x->x_phfb = x->x_phinc = 0.0;
    x->x_combD = 300.0; x->x_combmod = 0.0; x->x_cinc = 0.0;
    x->x_longD = 8000.0; x->x_longmod = 0.0; x->x_linc = 0.0;
    x->x_samp = x->x_fbk = x->x_loopdrive = 0.0; x->x_wet = 0.0;

    /* global slow-modulation engine */
    x->x_g1 = 0.11; x->x_g2 = 0.47; x->x_g3 = 0.83;   /* spread start phases   */
    x->x_g1inc = x->x_g2inc = x->x_g3inc = 0.0;
    x->x_rw = 0.0; x->x_rwtar = 0.0; x->x_rwcnt = 0.0;
    x->x_rng = 0x1234567u;
    x->x_trdepth = x->x_drdepth = x->x_liv = 0.0;
    x->x_phdepth0 = 0.55; x->x_phinc0 = 0.0; x->x_longmod0 = 400.0;

    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_liquid_tilde_setup(void)
{
    alien_liquid_tilde_class = class_new(gensym("alien_liquid~"),
        (t_newmethod)alien_liquid_new, 0, sizeof(t_alien_liquid),
        CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_liquid_tilde_class, t_alien_liquid, x_f);
    class_addmethod(alien_liquid_tilde_class, (t_method)alien_liquid_dsp,
                    gensym("dsp"), A_CANT, 0);
}
