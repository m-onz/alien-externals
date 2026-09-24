/* alien_liquid2~.c  --  a pitch-locked waveshaper: rich harmonic timbre where
 *                       every added partial is an EXACT integer multiple of the
 *                       fundamental, so it goes buzzy / brassy / vocal but never
 *                       collapses into broadband noise
 *
 * The whole voice is built around one identity of the Chebyshev polynomials of
 * the first kind:
 *
 *     T_k(cos ph) = cos(k ph)
 *
 * Drive an INTERNAL cosine oscillator at the fundamental and feed it through a
 * weighted sum of Chebyshev terms and the weights ARE the harmonic amplitudes
 * of the output -- and because T_k(cos ph) is exactly cos(k ph), every partial
 * lands precisely on an integer multiple of f0.  You dial a harmonic recipe and
 * it renders, perfectly pitched, with no additive sine stack to sum.
 *
 * Two pitch-locked mechanisms build the spectrum, both morphed by the macro:
 *
 *   1. CHEBYSHEV waveshaping of the cosine.  A 1/k-ish weight table whose tilt
 *      brightens and whose harmonic COUNT grows as the macro opens, plus a
 *      moving FORMANT emphasis -- a gaussian bump in the harmonic weights that
 *      sweeps up and down the series -- for a vowel-like, vocal quality.
 *
 *   2. Phase-distortion FM with an INTEGER carrier:modulator ratio.  The cosine
 *      phase is warped by a sine of an integer multiple of itself, so every
 *      sideband it creates also lands on a harmonic; the index is swept by the
 *      macro for brassy growth.  (Integer ratio => still perfectly pitched.)
 *
 * The nonlinear stage is OVERSAMPLED 4x: the shaper is evaluated on a phase
 * advancing at 4x the sample rate and the four sub-samples are averaged
 * (a simple lowpass decimation), so the harmonics the shaper mints above
 * Nyquist do NOT fold back as inharmonic hash -- this is what keeps it clean.
 *
 * Finally a gentle detuned-unison CHORUS (three copies a few cents apart, run
 * as three phase-offset oscillators through the same shaper) adds a LIQUID,
 * evolving quality.  It is linear and adds no new harmonics, so it never turns
 * into noise.
 *
 *   inlet 0   signal/float   fundamental in Hz (drives the internal cosine)
 *   inlet 1   signal/float   MACRO 0..100 (wrapped mod 100) -- one knob that
 *                            morphs the whole voice: at 0 a soft near-sine with
 *                            a few low harmonics and no FM; rising, the harmonic
 *                            count and Chebyshev tilt climb, the FM index grows,
 *                            the formant emphasis sweeps -> buzzy, brassy, vocal,
 *                            rich; at 100 a dense harmonic spectrum, still
 *                            perfectly pitched (low spectral flatness).
 *
 *   outlet 0  signal
 *
 * The harmonic weight table, FM index and formant position are recomputed ONCE
 * PER BLOCK (only when the macro or f0 change); the per-sample loop is just the
 * oversampled Chebyshev shaper and the three-voice chorus.  The shaper input is
 * clamped to [-1,1] before evaluation (high-order Chebyshev explodes just
 * outside that domain), f0 is clamped >= 0.01 and below Nyquist, the harmonic
 * count is clamped to the table capacity, and the output is finite-guarded,
 * denormal-flushed and bounded ~[-1.5,1.5] scaled near unity.
 *
 * build:
 *   gcc -shared -fPIC -O2 -o alien_liquid2~.pd_linux alien_liquid2~.c -lm
 *   (macOS: -bundle -undefined dynamic_lookup -o alien_liquid2~.pd_darwin)
 */

#include "m_pd.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TWOPI 6.283185307179586

#define MAXH  32         /* Chebyshev weight-table capacity                  */
#define OS    4          /* oversampling factor for the nonlinear stage      */
#define NVOX  3          /* detuned-unison chorus voices                     */

/* Post-shaper makeup gain.  Weights are L1-normalised so the raw shaper peaks
 * near 1 regardless of harmonic count; this trims the wet path under unity.   */
#define POSTGAIN 0.9

static t_class *alien_liquid2_tilde_class;

typedef struct _alien_liquid2 {
    t_object x_obj;
    t_float  x_f;

    double   x_sr;

    /* per-voice oscillator phases (radians) and a slow detune-drift LFO       */
    double   x_ph[NVOX];
    double   x_dlfo[NVOX];

    /* ---- internal MODULATION ENGINE state (advanced per sample) ----        *
     * Everything here is a SLOW modulator phase (0..1) or a smoothed random    *
     * walk.  They drive per-block recomputation of the weight table / formant  *
     * / FM index so a held note keeps evolving while staying exactly pitched.  */
    double   x_hlfo[MAXH];      /* per-harmonic tremolo phase (spectral boil)   */
    double   x_hrate[MAXH];     /* per-harmonic tremolo rate (desynchronised)   */
    double   x_glfo[3];         /* 3 global slow LFO phases (formant/FM/tilt)   */
    double   x_grate[3];        /* 3 global slow LFO base rates                 */
    double   x_rw;              /* smoothed random-walk value (vocal drift)     */
    double   x_rwt;             /* random-walk target                          */
    unsigned x_rng;             /* xorshift PRNG state                          */
    double   x_rwtimer;         /* countdown to pick a new random-walk target   */

    /* DC blocker history (even harmonics carry an offset)                     */
    double   x_dc0, x_dc1;

    /* ---- per-block cache (built once per block) ---- */
    int      x_n;               /* live harmonic count                        */
    double   x_w[MAXH];         /* harmonic weights (working, normalised)     */
    double   x_wbase[MAXH];     /* un-modulated weights (formant/tilt baked)  */
    double   x_inc;             /* base phase increment (rad/sample) at 1x     */
    double   x_fmratio;         /* integer carrier:modulator ratio            */
    double   x_fmindex;         /* phase-distortion FM index (modulated)      */

    /* ---- modulation amounts, derived from the macro (0..1 morph t) ----     */
    double   x_moddepth;        /* master modulation depth (0 still -> lively) */
    double   x_modrate;         /* master modulation rate scaler               */
    double   x_htrem;           /* per-harmonic tremolo depth                  */
    double   x_formmod;         /* formant-centre drift range (in harmonics)   */
    double   x_fmmod;           /* FM-index drift range                        */
    double   x_tiltmod;         /* spectral-tilt drift range                   */
    double   x_formbase;        /* base formant centre (harmonics)             */
    double   x_formw;           /* formant width                              */
    double   x_bias;            /* base spectral tilt                          */
    double   x_formamp;         /* formant emphasis amplitude                  */
    double   x_fmbase;          /* base FM index (before drift)                */
    double   x_detune[NVOX];    /* per-voice increment multiplier (cents)     */
    double   x_dinc[NVOX];      /* per-voice drift LFO increment              */
    double   x_ddepth;          /* drift depth (fraction of a semitone-ish)   */
    double   x_vgain;           /* 1/NVOX chorus normalisation                */

    double   x_lastmac;
    double   x_lastf0;
    int      x_haveblock;
} t_alien_liquid2;

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

/* Build every per-block quantity from one macro value + f0.  Called once per
 * block (only when the macro / f0 change), never per sample. */
static void build_block(t_alien_liquid2 *x, double mac, double f0)
{
    double sr = x->x_sr, nyq = sr * 0.48;
    double t, bias, formpos, formw;
    int n, i, fmr;

    if (!(mac == mac)) mac = 0.0;                    /* NaN guard              */
    t = mac - 100.0 * floor(mac * 0.01);
    if (!(t >= 0.0 && t <= 100.0)) t = 0.0;
    t *= 0.01;                                       /* t in [0,1)            */

    if (!(f0 == f0)) f0 = 0.01;
    if (f0 < 0.01)   f0 = 0.01;
    if (f0 > nyq)    f0 = nyq;                        /* below Nyquist         */

    /* base phase increment at the fundamental (1x rate) */
    x->x_inc = TWOPI * f0 / sr;

    /* ---- harmonic count: few near-sine at 0 -> dense at 100 ---- *
     * cap the count so the TOP harmonic stays below Nyquist: the shaper is
     * oversampled OS-fold, so the shaper's internal Nyquist is OS*sr/2, but we
     * still want the harmonics that survive decimation to be below sr/2.       */
    {
        int hcap = (int)(nyq / f0);                  /* highest in-band harm.  */
        if (hcap < 1)    hcap = 1;
        if (hcap > MAXH) hcap = MAXH;
        n = (int)(2.0 + (double)(MAXH - 2) * t * t + 0.5);   /* 2 -> MAXH      */
        if (n < 1)    n = 1;
        if (n > MAXH) n = MAXH;                      /* table-capacity clamp   */
        if (n > hcap) n = hcap;                      /* keep top harm in-band  */
    }
    x->x_n = n;

    /* ---- weight table: 1/k spectrum, tilt brightens as it grows, with a
     * moving GAUSSIAN formant bump for a vocal emphasis ---- *
     * Only the BASE (un-modulated) shape is baked here; the moving formant,
     * spectral boil and tilt drift are applied every block in apply_mod().     */
    bias    = 0.5 + 0.5 * t;                         /* mellow -> bright tilt  */
    /* formant CENTRE sweeps up the series and back as the macro opens          */
    formpos = 1.0 + (double)(n - 1) * (0.30 + 0.55 * (0.5 - 0.5 * cos(TWOPI * t)));
    formw   = 1.0 + 3.0 * t;                         /* wider bump as it opens */
    for (i = 0; i < MAXH; i++) x->x_wbase[i] = 0.0;
    for (i = 0; i < n; i++) {
        double k = (double)(i + 1);
        double base = pow(1.0 / k, 2.0 - bias);
        if (!(base == base) || base < 0.0) base = 0.0;
        x->x_wbase[i] = base;                        /* pure 1/k tilt, no bump */
    }
    x->x_bias    = bias;
    x->x_formbase = formpos;
    x->x_formw   = formw;
    x->x_formamp = 0.8 + 2.2 * t;                    /* base formant emphasis  */

    /* ---- phase-distortion FM: INTEGER ratio so sidebands land on harmonics *
     * ratio climbs 1 -> 4 in integer steps; index grows for brassy growth.    */
    fmr = 1 + (int)(3.0 * t + 0.5);                  /* 1..4                   */
    if (fmr < 1) fmr = 1;
    if (fmr > 6) fmr = 6;
    /* keep the top FM sideband (fmr * f0) in band; else drop the FM           */
    if ((double)fmr * f0 > nyq) fmr = (int)(nyq / f0);
    if (fmr < 1) fmr = 1;
    x->x_fmratio = (double)fmr;
    x->x_fmbase  = (t < 0.15) ? 0.0 : (0.9 * (t - 0.15) / 0.85);  /* 0 .. 0.9  */

    /* ---- MODULATION ENGINE amounts: everything grows with the macro ---- *
     * At macro 0 the engine is nearly still (tiny depth, glacial rate); at
     * macro 100 it is lively.  These scale the per-sample modulator advance
     * (rate) and the amount applied to the weight table / formant / FM (depth).*/
    {
        /* gate: no modulation at all until the macro has opened a little, then
         * ramp in.  This keeps macro 0 genuinely STILL (important when the
         * harmonic count is tiny, where small weight changes swing the RMS).   */
        double g  = (t - 0.04) / 0.96;               /* 0 below 4, ->1 at 100  */
        if (g < 0.0) g = 0.0;
        double td = g;                               /* linear-ish depth ramp  */
        x->x_moddepth = td;                          /* 0 (still) -> 1 (lively)*/
        x->x_modrate  = 0.08 + 0.92 * g;             /* crawl -> livelier rate */
        /* per-harmonic tremolo grows strongly with the macro so the dense
         * spectrum keeps "boiling" instead of averaging flat.                  */
        x->x_htrem    = (0.15 + 0.70 * g) * x->x_moddepth;    /* boil depth    */
        /* formant SWEEP RANGE grows with the harmonic count -> the centroid
         * wanders further (brightness animation) as the voice opens.          */
        x->x_formmod  = (1.0 + (double)(n) * 0.45) * x->x_moddepth; /* harm.   */
        x->x_fmmod    = (0.20 + 0.45 * g) * x->x_moddepth;    /* FM wobble      */
        x->x_tiltmod  = (0.30 + 0.55 * g) * x->x_moddepth;    /* tilt drift     */
    }

    /* ---- detuned-unison chorus: three copies a few cents apart ---- *
     * spread widens gently with the macro for a more liquid motion.           */
    {
        double cents = 0.4 + 11.6 * t;               /* nearly unison at 0     */
        double sp[NVOX] = { 0.0, -1.0, 1.0 };
        for (i = 0; i < NVOX; i++) {
            double semis = sp[i] * cents / 100.0;    /* fraction of a semitone */
            x->x_detune[i] = pow(2.0, semis / 12.0);
            /* slow, per-voice drift so the chorus keeps evolving; rate scaled
             * by macro so the chorus is nearly frozen at macro 0.             */
            x->x_dinc[i] = (0.02 + (0.05 + 0.20 * (double)i) * t) / sr;
        }
        x->x_ddepth = (0.0004 + 0.006 * t);          /* drift depth, small at 0*/
        x->x_vgain  = 1.0 / (double)NVOX;
    }

    x->x_lastmac = mac;
    x->x_lastf0  = f0;
    x->x_haveblock = 1;
}

/* Apply the current MODULATION-ENGINE state to build the live weight table and
 * FM index for this block.  Called EVERY block (the modulator phases advance
 * per sample in the perform loop); only the pow()/table work happens here, not
 * per sample.  Everything stays an EXACT harmonic -- we only reshape harmonic
 * amplitudes and the formant position, never add inharmonic content.  The table
 * is L1-renormalised afterwards so the modulation cannot pump the level.       */
static void apply_mod(t_alien_liquid2 *x)
{
    int n = x->x_n, i;
    double s;
    /* smoothed random-walk (vocal/talking movement) + slow LFOs feed the
     * formant centre and the FM index; a gentle third LFO tilts the spectrum.  */
    double lf0 = sin(x->x_glfo[0] * TWOPI);
    double lf1 = sin(x->x_glfo[1] * TWOPI);
    double lf2 = sin(x->x_glfo[2] * TWOPI);
    double rw  = x->x_rw;                             /* [-1,1] smoothed        */

    /* drifting formant CENTRE: base + slow LFO + random walk (kept in range)   */
    double formpos = x->x_formbase + x->x_formmod * (0.6 * lf0 + 0.4 * rw);
    double formw   = x->x_formw;
    double formamp = x->x_formamp;
    /* drifting spectral tilt: nudges the 1/k exponent gently up and down       */
    double tilt    = x->x_tiltmod * (0.7 * lf2 + 0.3 * rw);
    /* drifting FM index: slow wobble around the base, clamped >= 0             */
    double fmidx   = x->x_fmbase + x->x_fmmod * (0.6 * lf1 - 0.4 * rw);

    if (n < 1) n = 1;
    if (n > MAXH) n = MAXH;

    if (formpos < 1.0)          formpos = 1.0;
    if (formpos > (double)MAXH) formpos = (double)MAXH;
    if (formw < 0.5)            formw = 0.5;
    if (fmidx < 0.0)            fmidx = 0.0;
    if (fmidx > 1.2)            fmidx = 1.2;          /* keep the shaper tame    */
    x->x_fmindex = fmidx;

    for (i = 0; i < MAXH; i++) x->x_w[i] = 0.0;
    for (i = 0; i < n; i++) {
        double k = (double)(i + 1);
        double base = x->x_wbase[i];
        double d, form, trem, wv;
        /* gentle tilt drift: multiply by k^(-tilt) -- brighter or darker, but
         * every partial stays an exact harmonic.                              */
        if (tilt != 0.0) base *= pow(k, -tilt);
        /* moving gaussian formant emphasis (vocal)                            */
        d    = (k - formpos) / formw;
        form = 1.0 + formamp * exp(-0.5 * d * d);
        /* per-harmonic TREMOLO: each partial swells/fades at its own slow,
         * desynchronised rate -> spectral "boiling" while staying pitched.
         * factor in [1 - htrem, 1 + htrem], floored non-negative.             */
        trem = 1.0 + x->x_htrem * sin(x->x_hlfo[i] * TWOPI);
        if (trem < 0.0) trem = 0.0;
        wv = base * form * trem;
        if (!(wv == wv) || wv < 0.0) wv = 0.0;
        x->x_w[i] = wv;
    }
    /* L1-renormalise so the modulation reshapes the spectrum without pumping
     * the overall level.                                                       */
    s = 0.0;
    for (i = 0; i < n; i++) s += fabs(x->x_w[i]);
    if (s > 1e-9) for (i = 0; i < n; i++) x->x_w[i] /= s;
    else { for (i = 0; i < n; i++) x->x_w[i] = 0.0; x->x_w[0] = 1.0; }
}

/* evaluate the Chebyshev shaper on a single cosine driver.  xv is the (already
 * clamped) driver in [-1,1]; returns sum_{k=1}^{n} w_k T_k(xv) = the exact
 * harmonic spectrum the weights describe. */
static inline double shape(const double *w, int n, double xv)
{
    double t0 = 1.0, t1 = xv, tk, y;
    int i;
    if (xv >  1.0) xv =  1.0;              /* domain guard (belt & braces)     */
    if (xv < -1.0) xv = -1.0;
    t1 = xv;
    y  = w[0] * t1;                        /* k = 1                            */
    for (i = 1; i < n; i++) {
        tk = 2.0 * xv * t1 - t0;           /* T_{i+1}                          */
        t0 = t1; t1 = tk;
        y += w[i] * tk;                    /* k = i+1                          */
    }
    return y;
}

static t_int *alien_liquid2_perform(t_int *w)
{
    t_alien_liquid2 *x = (t_alien_liquid2 *)(w[1]);
    t_sample *fin = (t_sample *)(w[2]);
    t_sample *min = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    int nsamp = (int)(w[5]);

    double mac0 = min[0], f00 = fin[0];
    int n, v, o, h;
    double inc, fmratio, fmindex, ddepth, vgain, modrate;
    double detune[NVOX], dinc[NVOX];

    if (!x->x_haveblock || mac0 != x->x_lastmac || f00 != x->x_lastf0)
        build_block(x, mac0, f00);

    /* recompute the modulated weight table + FM index ONCE for this block from
     * the current (block-start) modulator state; phases advance per sample.    */
    apply_mod(x);

    n = x->x_n; inc = x->x_inc; fmratio = x->x_fmratio; fmindex = x->x_fmindex;
    ddepth = x->x_ddepth; vgain = x->x_vgain; modrate = x->x_modrate;
    for (v = 0; v < NVOX; v++) { detune[v] = x->x_detune[v]; dinc[v] = x->x_dinc[v]; }

    while (nsamp--) {
        double acc = 0.0, m, ob;
        fin++; min++;                                  /* read per block only   */

        /* ---- advance the MODULATION ENGINE, one step per sample ---- *
         * All wrapped to [0,1).  Rates scaled by macro (modrate) so at macro 0
         * the whole engine crawls and at 100 it is lively.                     */
        for (h = 0; h < n; h++) {
            x->x_hlfo[h] += x->x_hrate[h] * modrate;
            if (x->x_hlfo[h] >= 1.0) x->x_hlfo[h] -= floor(x->x_hlfo[h]);
        }
        for (h = 0; h < 3; h++) {
            x->x_glfo[h] += x->x_grate[h] * modrate;
            if (x->x_glfo[h] >= 1.0) x->x_glfo[h] -= floor(x->x_glfo[h]);
        }
        /* smoothed random-walk: pick a fresh target occasionally, glide to it  *
         * (one-pole smoothing) -> vocal/talking, non-periodic drift.           */
        if (x->x_rwtimer <= 0.0) {
            x->x_rng ^= x->x_rng << 13; x->x_rng ^= x->x_rng >> 17;
            x->x_rng ^= x->x_rng << 5;
            x->x_rwt = ((double)(x->x_rng & 0xffff) / 32768.0) - 1.0;  /* [-1,1)*/
            /* re-arm: slower when macro is low                                 */
            x->x_rwtimer = (0.15 + 0.85 / (0.05 + modrate)) * x->x_sr;
        }
        x->x_rwtimer -= 1.0;
        x->x_rw += (x->x_rwt - x->x_rw) * (0.0008 * (0.2 + modrate));
        if (!(x->x_rw > -2.0 && x->x_rw < 2.0)) x->x_rw = 0.0;

        for (v = 0; v < NVOX; v++) {
            double det, vinc, sub = 0.0;
            /* slow per-voice detune drift for the liquid, evolving motion     */
            x->x_dlfo[v] += dinc[v];
            if (x->x_dlfo[v] >= 1.0) x->x_dlfo[v] -= 1.0;
            det  = detune[v] * (1.0 + ddepth * sin(x->x_dlfo[v] * TWOPI));
            vinc = inc * det / (double)OS;             /* increment at OS rate  */

            /* OVERSAMPLE: advance the phase at OS x sr, shape each sub-sample,
             * and average -> a simple lowpass decimation that rejects the
             * shaper's above-Nyquist harmonics before they can fold.          */
            for (o = 0; o < OS; o++) {
                double ph, warp, xc, y;
                x->x_ph[v] += vinc;
                if (x->x_ph[v] >= TWOPI || x->x_ph[v] < 0.0)
                    x->x_ph[v] -= TWOPI * floor(x->x_ph[v] / TWOPI);
                ph = x->x_ph[v];
                /* phase-distortion FM with INTEGER ratio: warp lands its
                 * sidebands exactly on harmonics -> still perfectly pitched.  */
                warp = ph + fmindex * sin(fmratio * ph);
                xc = cos(warp);
                if (xc >  1.0) xc =  1.0;              /* clamp shaper INPUT    */
                if (xc < -1.0) xc = -1.0;
                y = shape(x->x_w, n, xc);
                sub += y;
            }
            sub *= (1.0 / (double)OS);                 /* decimate (average)    */
            acc += sub;
        }
        acc *= vgain;                                  /* chorus normalisation  */

        acc = softclip(POSTGAIN * acc);

        /* DC blocker: strips the even-harmonic offset the shaper carries      */
        m  = acc;
        ob = m - x->x_dc0 + 0.999 * x->x_dc1;
        x->x_dc0 = flushd(m);
        x->x_dc1 = flushd(ob);

        /* output safety: finite-guard + hard bound near unity                 */
        if (!(ob > -1.5 && ob < 1.5)) {
            ob = softclip(ob);
            if (!(ob > -1.5 && ob < 1.5)) ob = 0.0;
        }
        *out++ = (t_sample)(ob * 1.08);
    }

    /* keep long-lived state clean between blocks */
    x->x_dc0 = flushd(x->x_dc0);
    x->x_dc1 = flushd(x->x_dc1);
    for (v = 0; v < NVOX; v++) {
        if (!(x->x_ph[v] > -1e6 && x->x_ph[v] < 1e6)) x->x_ph[v] = 0.0;
    }
    /* finite-guard the modulator phases / random walk                          */
    for (h = 0; h < MAXH; h++)
        if (!(x->x_hlfo[h] >= 0.0 && x->x_hlfo[h] < 1.0)) x->x_hlfo[h] = 0.0;
    for (h = 0; h < 3; h++)
        if (!(x->x_glfo[h] >= 0.0 && x->x_glfo[h] < 1.0)) x->x_glfo[h] = 0.0;
    if (!(x->x_rw > -2.0 && x->x_rw < 2.0)) x->x_rw = 0.0;
    if (!(x->x_rwt > -2.0 && x->x_rwt < 2.0)) x->x_rwt = 0.0;
    if (!(x->x_rwtimer >= -1.0 && x->x_rwtimer < 1e12)) x->x_rwtimer = 0.0;
    return (w + 6);
}

static void alien_liquid2_dsp(t_alien_liquid2 *x, t_signal **sp)
{
    if (sp[0]->s_sr > 1000) x->x_sr = sp[0]->s_sr;
    dsp_add(alien_liquid2_perform, 5, x,
            sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_liquid2_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_liquid2 *x = (t_alien_liquid2 *)pd_new(alien_liquid2_tilde_class);
    int i;
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;

    for (i = 0; i < NVOX; i++) {
        x->x_ph[i]   = (double)i * 0.021 * TWOPI;   /* slight phase offsets    */
        x->x_dlfo[i] = (double)(i * 5 % 13) / 13.0;
        x->x_detune[i] = 1.0;
        x->x_dinc[i]   = 0.0;
    }
    for (i = 0; i < MAXH; i++) { x->x_w[i] = 0.0; x->x_wbase[i] = 0.0; }
    x->x_dc0 = x->x_dc1 = 0.0;
    x->x_n = 1; x->x_w[0] = 1.0; x->x_wbase[0] = 1.0;
    x->x_inc = 0.0; x->x_fmratio = 1.0; x->x_fmindex = 0.0;
    x->x_ddepth = 0.0; x->x_vgain = 1.0 / (double)NVOX;
    x->x_haveblock = 0;

    /* ---- MODULATION ENGINE init: desynchronised slow rates + PRNG ---- *
     * Per-harmonic tremolo: each partial gets its own slow, mutually
     * irrational-ish rate (roughly 0.05..0.9 Hz) so the spectrum "boils"
     * without any two partials locking together.  Rates are scaled per block
     * by the macro (x_modrate).  Phases spread so they start out of step.      */
    for (i = 0; i < MAXH; i++) {
        double hz = 0.05 + 0.85 * ((double)((i * 7 + 3) % MAXH) / (double)MAXH);
        hz *= (1.0 + 0.013 * (double)i);            /* detune so no two align   */
        x->x_hrate[i] = hz / x->x_sr;               /* cycles/sample            */
        x->x_hlfo[i]  = (double)((i * 11 + 4) % 29) / 29.0;
    }
    /* three global LFOs: formant sweep, FM wobble, tilt drift -- all slow      */
    {
        double ghz[3] = { 0.11, 0.071, 0.043 };     /* Hz, mutually incommensurate*/
        for (i = 0; i < 3; i++) {
            x->x_grate[i] = ghz[i] / x->x_sr;
            x->x_glfo[i]  = (double)(i * 7 + 2) / 17.0;
        }
    }
    x->x_rw = 0.0; x->x_rwt = 0.0; x->x_rwtimer = 0.0;
    x->x_rng = 0x9e3779b9u;                          /* nonzero xorshift seed    */
    x->x_moddepth = x->x_modrate = 0.0;
    x->x_htrem = x->x_formmod = x->x_fmmod = x->x_tiltmod = 0.0;
    x->x_formbase = 1.0; x->x_formw = 1.0; x->x_bias = 0.5;
    x->x_formamp = 0.0; x->x_fmbase = 0.0;
    x->x_lastmac = -1e30;
    x->x_lastf0  = -1e30;

    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_liquid2_tilde_setup(void)
{
    alien_liquid2_tilde_class = class_new(gensym("alien_liquid2~"),
        (t_newmethod)alien_liquid2_new, 0, sizeof(t_alien_liquid2),
        CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_liquid2_tilde_class, t_alien_liquid2, x_f);
    class_addmethod(alien_liquid2_tilde_class, (t_method)alien_liquid2_dsp,
                    gensym("dsp"), A_CANT, 0);
}
