/* alien_liquid3~.c  --  modal resonator bank (ringing, tonal, vocal->bell)
 *
 * A bank of high-Q 2-pole BANDPASS resonators (biquads), each tuned to a
 * partial of the fundamental, driven continuously by a low-level exciter
 * (an impulse train at f0 plus a whisper of filtered noise).  Because the
 * resonator peaks sit exactly on the partials, the tone stays PITCHED no
 * matter how high the Q -- ring it hard and you get a tall, singing set of
 * partials, never broadband hiss.  This is the "liquid" cousin of
 * alien_impact~/alien_bow~ built purely as a formant/partial bank: it is
 * poured full of a faint exciter and it rings.
 *
 * Where alien_stack~ SUMS partials additively, this SELECTS them out of a
 * flat exciter with resonators, so the emphasis, the ring and the moving
 * formant come from the filters, not from oscillators.  A slow, small,
 * per-resonator frequency/Q wobble gives a liquid, evolving quality
 * without ever smearing into noise.
 *
 *   inlet 0   signal/float   fundamental in Hz  (use [mtof] to play MIDI)
 *   inlet 1   signal/float   MACRO 0..100 (wrapped mod 100) -- one knob that
 *                            morphs the whole voice.  At 0 a soft, low-Q,
 *                            near-flat organ-ish tone on a harmonic set.
 *                            Rising, the Q climbs and the bank peaks up into
 *                            a moving vocal formant, the ratio set stretches
 *                            harmonic -> inharmonic, a brightness/formant
 *                            sweep chimes the upper partials; at 100 a tall,
 *                            ringing, inharmonic bell/metal set of partials
 *                            that is still clearly pitched (low spectral
 *                            flatness).
 *
 *   outlet 0  signal
 *
 * All the trig / pow / coefficient work is done ONCE PER BLOCK (only when
 * the macro / f0 change); the per-sample loop is just the biquad bank plus
 * the exciter.  Every resonator is kept strictly stable (pole radius < 1),
 * every state is clamped and denormal-flushed each sample, the summed bank
 * is normalised so high Q cannot rail, and the output is finite-guarded and
 * bounded -- safe under audio-rate macro modulation.
 *
 * build:
 *   gcc -shared -fPIC -O2 -o alien_liquid3~.pd_linux alien_liquid3~.c -lm
 *   (macOS: -bundle -undefined dynamic_lookup -o alien_liquid3~.pd_darwin)
 */

#include "m_pd.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TWOPI    (2.0 * M_PI)
#define MAXRES   24        /* array capacity for resonators                    */
#define MAXN     20        /* macro may request at most this many (<= MAXRES)  */

static t_class *alien_liquid3_tilde_class;

typedef struct _alien_liquid3 {
    t_object x_obj;
    t_float  x_f;

    double   x_sr;

    /* ---- resonator bank state (biquad direct form) ---- */
    double   x_y1[MAXRES], x_y2[MAXRES];
    double   x_x1[MAXRES], x_x2[MAXRES];

    /* ---- ratio families (harmonic -> stretched -> bell) ---- */
    double   x_ratio[MAXRES];
    int      x_lastmode;

    /* ---- exciter ---- */
    double   x_phase;          /* fundamental phase for impulse train          */
    double   x_noiselp;        /* one-pole state for filtered exciter noise    */
    double   x_exlp;           /* one-pole state for exciter brightness LP     */
    unsigned x_rng;

    /* ---- output DC blocker (impulse-train drive carries DC) ---- */
    double   x_dcx1, x_dcy1;

    /* ---- slow per-resonator wobble LFOs (exciter-drive breathing) ---- */
    double   x_wob[MAXRES];    /* phase                                        */
    double   x_wobinc[MAXRES]; /* per-block increment                          */

    /* ---- per-resonator amplitude shimmer (spectral boiling) ----
     * Independent slow gain LFOs, one per resonator, each at its own
     * desynchronised rate, so partials swell and fade out of phase. */
    double   x_shim[MAXRES];   /* phase                                        */
    double   x_shiminc[MAXRES];/* per-sample increment (set per block)         */
    double   x_shimoff[MAXRES];/* fixed phase offset per resonator             */

    /* ---- GLOBAL slow modulation engine (formant/brightness/tilt) ---- */
    double   x_lfo1, x_lfo2, x_lfo3;   /* three slow LFO phases                */
    double   x_lfo1inc, x_lfo2inc, x_lfo3inc; /* per-sample increments         */
    double   x_rw;             /* smoothed random-walk value  [-1,1]           */
    double   x_rwtarget;       /* current random-walk target                   */
    int      x_rwcount;        /* samples until next random-walk step          */
    double   x_moddepth;       /* global modulation depth (scaled by macro)    */
    int      x_blockcount;     /* forces periodic coeff rebuild for global mod */

    /* ---- per-block coefficient cache (built once per block) ---- */
    int      x_n;
    double   x_b0[MAXRES];     /* bandpass numerator (0 dB peak scaled)        */
    double   x_a1[MAXRES], x_a2[MAXRES];
    double   x_g[MAXRES];      /* per-resonator emphasis gain                  */
    double   x_active[MAXRES];
    double   x_fc[MAXRES];     /* cached centre freq (for wobble reference)    */
    double   x_qcur[MAXRES];   /* cached Q                                     */
    double   x_norm;           /* summed-bank normalisation                    */
    double   x_exnoise;        /* exciter noise level                          */
    double   x_eximp;          /* exciter impulse level                        */
    double   x_excut;          /* exciter lowpass coeff (opens with knob)      */
    double   x_wobdepth;       /* freq wobble depth (fraction)                 */
    double   x_shimdepth;      /* per-resonator amplitude shimmer depth        */
    double   x_outg;           /* output gain trim                             */

    double   x_lastmac;
    double   x_lastf0;
    int      x_haveblock;
} t_alien_liquid3;

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

static inline unsigned rn(t_alien_liquid3 *x)
{
    x->x_rng ^= x->x_rng << 13; x->x_rng ^= x->x_rng >> 17;
    x->x_rng ^= x->x_rng << 5;  return x->x_rng;
}
static inline double frnd(t_alien_liquid3 *x)
{
    return (double)(rn(x) & 0xffffff) / (double)0x800000 - 1.0;
}

/* ------------------------------------------------------------------ */

/* Ratio families the macro morphs between: harmonic (pitched, organ),
 * stretched (mildly inharmonic, vocal/chiming), bell (inharmonic metal).
 * All stay TONAL because the resonator peaks land on discrete partials. */
#define NMODE 3
static const double MODE_RATIO[NMODE][MAXRES] = {
    {0},   /* 0 harmonic: filled at load time (1,2,3,...) */
    /* 1 stretched-ish: harmonic base pulled slightly sharp up top */
    {1,2.01,3.03,4.07,5.12,6.20,7.30,8.43,9.58,10.8,12.0,13.3,14.7,16.1,17.6,19.2},
    /* 2 bell / metal inharmonic set */
    {1,2.00,3.01,4.15,5.43,6.79,8.21,9.33,10.9,12.4,14.1,15.9,17.8,19.9,22.1,24.5}
};

static void load_mode(t_alien_liquid3 *x, int mode)
{
    int i;
    if (mode == 0) for (i = 0; i < MAXRES; i++) x->x_ratio[i] = i + 1;
    else           for (i = 0; i < MAXRES; i++) x->x_ratio[i] = MODE_RATIO[mode][i];
    x->x_lastmode = mode;
}

/* Build every per-block quantity from one macro value + f0.  Called once per
 * block (only when the macro / f0 change), never per sample.  All the trig,
 * pow and the ratio-set select live here. */
static void build_block(t_alien_liquid3 *x, double mac, double f0)
{
    double sr = x->x_sr, nyq = sr * 0.47;
    double t, tbase, stretch, tilt, qbase, qtilt, formant, fw, gsum = 0.0;
    double gmod, gmodf, gmodq;
    int n, mode, i, nactive = 0;

    if (!(mac == mac)) mac = 0.0;                    /* NaN guard              */
    /* Fold to [0,100].  The macro is "wrapped mod 100", but map the endpoint
     * 100 to the FULL-open position rather than folding it back to 0, so the
     * knob spans 0..100 inclusive (macro 100 == liveliest, not == macro 0).
     * Values above 100 still wrap. */
    if (mac >= 0.0 && mac <= 100.0) t = mac;
    else                            t = mac - 100.0 * floor(mac * 0.01);
    if (!(t >= 0.0 && t <= 100.0)) t = 0.0;
    t *= 0.01;                                       /* t in [0,1]            */
    tbase = t;                                       /* un-modulated macro pos */

    /* ---- GLOBAL slow modulation depth scales with macro: nearly still at 0,
     * lively at 100.  Three slow LFOs + a smoothed random walk combine into a
     * gentle drift routed to the formant/brightness position and the Q/tilt.
     * SLOW and small: this recomputes the coefficient set per block only, so it
     * never touches poles per sample.  Read the modulator phases (advanced per
     * sample in perform) here, do the trig here (per block). */
    /* depth grows with the macro but eases back near the very top, where the
     * dense high-Q bell set is spectrally busy and extra coefficient drift would
     * lift the flatness toward noise.  Peaks around t~0.6. */
    x->x_moddepth = 0.02 + 0.20 * tbase * (1.30 - 0.60 * tbase);
    {
        double l1 = sin(x->x_lfo1 * TWOPI);
        double l2 = sin(x->x_lfo2 * TWOPI);
        double l3 = sin(x->x_lfo3 * TWOPI);
        double rw = x->x_rw;
        if (!(l1 == l1)) l1 = 0.0;
        if (!(l2 == l2)) l2 = 0.0;
        if (!(l3 == l3)) l3 = 0.0;
        if (!(rw == rw)) rw = 0.0;
        /* combined normalised drift in ~[-1,1] */
        gmod  = 0.45 * l1 + 0.30 * l2 + 0.25 * rw;
        /* formant/brightness position drift (main centroid animator) */
        gmodf = x->x_moddepth * (0.6 * l1 + 0.4 * l3);
        /* Q/tilt drift (spectral "breathing") */
        gmodq = x->x_moddepth * (0.5 * l2 + 0.5 * rw);
        (void)gmod;
    }
    /* apply the global drift to the timbre position; clamp back into [0,1) so
     * every downstream pow/exp stays finite and the poles stay in range */
    t = tbase + gmodf;
    if (!(t == t)) t = tbase;
    if (t < 0.0) t = 0.0;
    if (t > 0.999) t = 0.999;

    if (!(f0 == f0)) f0 = 0.01;
    if (f0 < 0.01)   f0 = 0.01;
    if (f0 > 8000.0) f0 = 8000.0;

    /* ---- ratio family (discrete morph, harmonic -> stretched -> bell) ----
     * Chosen from the UN-modulated macro so the partial SET and resonator COUNT
     * stay stable while the global mod only breathes the continuous timbre
     * params; this keeps the note steady (no partials popping in/out). */
    mode = (tbase < 0.38) ? 0 : (tbase < 0.70 ? 1 : 2);
    if (mode != x->x_lastmode) load_mode(x, mode);

    n = (int)(6.0 + 10.0 * tbase);                   /* 6..16 resonators       */
    if (n < 1)    n = 1;
    if (n > MAXN) n = MAXN;

    /* mild extra stretch on top of the ratio set as the knob opens */
    stretch = 1.0 + 0.25 * t * t;                    /* neutral -> spread      */
    /* tilt drives the brightness sweep: at 0 a STEEP dark tilt so only the
     * low partials sound (organ); at 100 a bright tilt so the high partials
     * ring out (chime/metal).  This is the main driver of centroid range. */
    tilt    = -2.6 + 3.6 * t;                        /* dark -> bright         */

    /* Q sweep: low-Q broad organ at 0, tall ringing partials at 100.
     * qtilt lets upper partials ring MORE up top (vocal/metal shimmer).
     * The Q base breathes gently with gmodq (bounded so the pole clamp below
     * never has real work to do); tilt gets a small tilt drift too. */
    qbase   = 3.0 + 220.0 * t * t;                   /* ~3 .. ~223            */
    qbase  *= 1.0 + 0.6 * gmodq;                      /* gentle Q breathing     */
    if (qbase < 0.5) qbase = 0.5;
    qtilt   = -0.4 + 1.1 * t + 0.4 * gmodq;          /* highs peak more up top */

    /* moving vocal formant: a brightness bump that slides FAR up with the knob,
     * from near the fundamental (dark/round) to a high chiming partial. */
    formant = 1.0 + 13.0 * t;                        /* formant partial index  */
    fw      = 3.0 - 1.8 * t;                          /* formant width          */

    x->x_wobdepth = 0.0006 + 0.0035 * t;             /* small freq wobble      */

    /* per-resonator amplitude shimmer depth (spectral boiling): each resonator
     * gain swells/fades on its own slow LFO.  Small at macro 0, strong at 100.
     * This is a pure gain animation -> adds NO broadband noise and cannot
     * destabilise a pole. */
    x->x_shimdepth = 0.05 + 0.60 * tbase;            /* 0.05 .. 0.65          */

    /* exciter: keep it FAINT so the resonators make the tone, not the source.
     * impulse train drives the ring; a whisper of noise keeps it alive.  As Q
     * climbs the resonators self-sustain more, so we ease the noise off to
     * keep spectral flatness low (tonal) at the top. */
    x->x_eximp   = 0.9;
    x->x_exnoise = 0.06 * (1.0 - 0.97 * t);          /* 0.06 -> ~0.0018       */
    /* exciter brightness: a one-pole LP on the impulse click whose cutoff
     * opens with the knob, so at 0 the exciter is dull (broad low-Q resonators
     * stay dark/organ) and at 100 the click is bright (chime/metal).  This is
     * what actually swings the spectral centroid across the knob. */
    x->x_excut   = 0.02 + 0.72 * t * t;              /* dull -> bright         */
    if (x->x_excut > 0.90) x->x_excut = 0.90;

    x->x_n = n;
    for (i = 0; i < n; i++) {
        double r = x->x_ratio[i];
        double fc, w0, cw, sw, Q, alpha, denom, g, rel;

        if (r <= 0.0) {
            x->x_active[i] = 0.0; x->x_b0[i] = x->x_a1[i] = x->x_a2[i] = 0.0;
            x->x_g[i] = 0.0; x->x_fc[i] = 0.0; x->x_qcur[i] = 1.0;
            x->x_y1[i] = x->x_y2[i] = x->x_x1[i] = x->x_x2[i] = 0.0;
            continue;
        }
        fc = f0 * pow(r, stretch);
        if (fc < 1.0)   fc = 1.0;
        if (fc > nyq)   fc = nyq;                     /* clamp (0, ~0.47 sr)   */

        /* per-resonator Q: higher partials ring more as qtilt rises */
        Q = qbase * pow(r, qtilt);
        if (Q < 0.5)    Q = 0.5;
        if (Q > 600.0)  Q = 600.0;                    /* stay well inside unit */

        w0    = TWOPI * fc / sr;
        cw    = cos(w0); sw = sin(w0);
        alpha = sw / (2.0 * Q);
        denom = 1.0 + alpha;
        if (denom < 1.0e-9) denom = 1.0e-9;

        /* Guard pole radius strictly < 1: for this BPF the poles have
         * radius sqrt(a2) = sqrt((1-alpha)/(1+alpha)); if alpha ever went
         * negative the pole could reach the unit circle, so force it > 0. */
        {
            double a2 = (1.0 - alpha) / denom;        /* = radius^2           */
            if (a2 > 0.9990) {                        /* radius > ~0.9995     */
                /* raise alpha (lower effective Q) until radius <= 0.9995 */
                double amin = (1.0 - 0.9990) / (1.0 + 0.9990); /* alpha for a2=0.999 */
                if (alpha < amin) { alpha = amin; denom = 1.0 + alpha; }
            }
        }

        x->x_b0[i] = alpha / denom;                   /* 0 dB peak BPF numer  */
        x->x_a1[i] = -2.0 * cw / denom;
        x->x_a2[i] = (1.0 - alpha) / denom;

        /* emphasis: gently spectral-tilt the partials + moving formant bump.
         * base falls with partial index; tilt brightens; formant adds a slide */
        g   = 1.0 / (1.0 + 0.35 * (double)i);
        if (tilt != 0.0) g *= pow(r, tilt * 0.35);
        rel = ((double)i - (formant - 1.0)) / fw;     /* distance from formant */
        g  *= 1.0 + 0.9 * exp(-rel * rel);            /* formant emphasis bump */
        if (!(g == g) || g < 0.0) g = 0.0;

        x->x_g[i]      = g;
        x->x_active[i] = 1.0;
        x->x_fc[i]     = fc;
        x->x_qcur[i]   = Q;
        gsum += g;
        nactive++;
    }

    /* normalise: divide the summed bank by the summed (energy-weighted) gains
     * so more/higher-Q resonators do not blow the level.  A sqrt keeps the
     * bank from going too quiet as many partials add near-incoherently. */
    if (gsum < 1e-6) gsum = 1.0;
    x->x_norm = 1.0 / gsum;

    /* Output lift.  Empirically the raw normalised bank RMS FALLS ~10-20x as
     * the knob opens (narrow high-Q peaks sit near a node most of the time and
     * the growing partial count enlarges gsum), so the lift must RISE with the
     * knob to hold a roughly constant level; it eases back near the top where
     * the ring recovers.  Fitted to a measured level curve and clamped hard so
     * it can never rail. */
    /* base the output lift on the UN-modulated macro so the global timbre
     * drift does not pump the overall level (the level animation comes from the
     * per-resonator shimmer, not from the master gain swinging on the steep
     * exp curve). */
    x->x_outg = 95.0 * exp(3.0 * tbase) * (1.0 - 0.55 * tbase * tbase * tbase);
    if (x->x_outg > 1400.0) x->x_outg = 1400.0;
    if (x->x_outg < 1.0)    x->x_outg = 1.0;

    /* slow per-resonator wobble increments (all distinct, sub-Hz .. few Hz) */
    for (i = 0; i < n; i++)
        x->x_wobinc[i] = (0.07 + 0.9 * (double)((i * 7) % 13) / 13.0
                          + 1.5 * tbase * (double)((i * 3) % 5) / 5.0) / sr;

    /* per-resonator amplitude-shimmer increments: each resonator on its own
     * desynchronised slow rate (~0.1 .. ~1.5 Hz), faster/livelier as the macro
     * opens.  Distinct primes-ish spacing keeps them from beating in lockstep. */
    for (i = 0; i < n; i++) {
        double rate = 0.11 + 0.28 * (double)((i * 11) % 17) / 17.0
                      + (0.15 + 0.9 * tbase) * (double)((i * 5) % 7) / 7.0;
        x->x_shiminc[i] = rate / sr;                  /* cycles per sample     */
    }

    /* global LFO rates (SLOW): 0.05 .. ~0.6 Hz, speeding up with the macro so
     * the drift is livelier at 100.  Random-walk step interval likewise. */
    x->x_lfo1inc = (0.05 + 0.35 * tbase) / sr;
    x->x_lfo2inc = (0.08 + 0.45 * tbase) / sr;
    x->x_lfo3inc = (0.037 + 0.30 * tbase) / sr;

    (void)nactive;
    x->x_lastmac = mac;
    x->x_lastf0  = f0;
    x->x_haveblock = 1;
}

static t_int *alien_liquid3_perform(t_int *w)
{
    t_alien_liquid3 *x = (t_alien_liquid3 *)(w[1]);
    t_sample *fin = (t_sample *)(w[2]);
    t_sample *min = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    int nsamp = (int)(w[5]);

    double mac0 = min[0], f00 = fin[0];
    double sr = x->x_sr;
    double norm, exnoise, eximp, wobdepth, shimdepth, outg, phinc, excut;
    int n, i;

    /* Rebuild the coefficient set EVERY block: not only when the inlets change
     * but also so the GLOBAL slow modulation (whose phases advanced last block)
     * re-drifts the formant/brightness/Q.  build_block reads the modulator
     * phases and does ALL the trig/pow/ratio-select, so the per-sample loop
     * stays cheap and the poles are only ever moved by the safe per-block
     * rebuild -- never per sample.  We intentionally always recompute so a held
     * note keeps evolving. */
    build_block(x, mac0, f00);

    n = x->x_n; norm = x->x_norm; exnoise = x->x_exnoise; eximp = x->x_eximp;
    wobdepth = x->x_wobdepth; shimdepth = x->x_shimdepth;
    outg = x->x_outg; excut = x->x_excut;

    {
        double f0 = x->x_lastf0;
        if (f0 < 0.01) f0 = 0.01;
        phinc = f0 / sr;
    }

    while (nsamp--) {
        double exc, imp, y = 0.0, o;
        fin++; min++;                                  /* read per block only   */

        /* ---- advance GLOBAL modulator phases (cheap: just accumulate) ----
         * Trig on these phases is done once per block in build_block; here we
         * only integrate the phase and step the smoothed random walk. */
        x->x_lfo1 += x->x_lfo1inc; if (x->x_lfo1 >= 1.0) x->x_lfo1 -= 1.0;
        x->x_lfo2 += x->x_lfo2inc; if (x->x_lfo2 >= 1.0) x->x_lfo2 -= 1.0;
        x->x_lfo3 += x->x_lfo3inc; if (x->x_lfo3 >= 1.0) x->x_lfo3 -= 1.0;
        if (--x->x_rwcount <= 0) {                      /* pick a new RW target  */
            x->x_rwtarget = frnd(x);                    /* in [-1,1]             */
            x->x_rwcount  = (int)(sr * (0.6 + 0.9 * (double)(rn(x) & 0xff) / 255.0));
            if (x->x_rwcount < 1) x->x_rwcount = 1;
        }
        /* one-pole smoothing toward the target -> gentle band-limited walk */
        x->x_rw += 0.00003 * (x->x_rwtarget - x->x_rw);
        x->x_rw = flushd(x->x_rw);
        if (x->x_rw >  1.0) x->x_rw =  1.0;
        if (x->x_rw < -1.0) x->x_rw = -1.0;

        /* ---- exciter: one impulse per fundamental period + faint noise ---- */
        imp = 0.0;
        x->x_phase += phinc;
        if (x->x_phase >= 1.0) {
            x->x_phase -= 1.0;
            if (x->x_phase >= 1.0) x->x_phase = 0.0;   /* audio-rate f0 guard   */
            imp = eximp;                               /* single-sample impulse */
        }
        /* faint pink-ish noise (one-pole lowpassed white) keeps the ring alive */
        x->x_noiselp += 0.15 * (frnd(x) - x->x_noiselp);
        x->x_noiselp = flushd(x->x_noiselp);
        exc = imp + exnoise * x->x_noiselp;
        /* brightness LP on the whole exciter: dull at low knob, open at high  */
        x->x_exlp += excut * (exc - x->x_exlp);
        x->x_exlp = flushd(x->x_exlp);
        exc = x->x_exlp;

        /* ---- resonator bank ----
         * The biquad COEFFICIENTS are held fixed per block (touching a1/a2 on
         * a high-Q resonator moves the poles and can push them outside the
         * unit circle -> blow up).  The slow "liquid" wobble is applied to the
         * EXCITER DRIVE per resonator instead, which is always stable: each
         * resonator gets a slightly different, slowly-breathing drive so the
         * partials shimmer and evolve without ever destabilising. */
        for (i = 0; i < n; i++) {
            double xin, ap, bp, wob, shim, yv;
            if (x->x_active[i] == 0.0) continue;

            x->x_wob[i] += x->x_wobinc[i];
            if (x->x_wob[i] >= 1.0) x->x_wob[i] -= 1.0;
            wob = 1.0 + wobdepth * 40.0 * sin(x->x_wob[i] * TWOPI); /* breathe drive */

            /* PER-RESONATOR amplitude shimmer (spectral boiling): each partial
             * swells and fades on its own desynchronised slow LFO.  This is a
             * pure gain animation on the drive -> always stable, no broadband
             * noise.  Kept strictly >= 0 so a partial never flips phase. */
            x->x_shim[i] += x->x_shiminc[i];
            if (x->x_shim[i] >= 1.0) x->x_shim[i] -= 1.0;
            shim = 1.0 + shimdepth * sin((x->x_shim[i] + x->x_shimoff[i]) * TWOPI);
            if (shim < 0.1) shim = 0.1;   /* never fully mute a partial (tonal) */

            xin = exc * wob * shim;
            /* 0 dB bandpass biquad: y = b0*(x - x2) - a1*y1 - a2*y2 */
            ap = x->x_b0[i] * (xin - x->x_x2[i]);
            bp = x->x_a1[i] * x->x_y1[i] + x->x_a2[i] * x->x_y2[i];
            yv = ap - bp;
            yv = flushd(yv);
            /* hard magnitude clamp so a stray transient can never run away */
            if (yv >  1e4) yv =  1e4;
            else if (yv < -1e4) yv = -1e4;

            x->x_x2[i] = x->x_x1[i]; x->x_x1[i] = xin;
            x->x_y2[i] = x->x_y1[i]; x->x_y1[i] = yv;

            y += yv * x->x_g[i];
        }

        y *= norm;

        /* soft-bound, then DC-block: the impulse train injects DC and an
         * asymmetric soft clip re-introduces an offset, so remove any residual
         * mean AFTER the nonlinearity. */
        o = softclip(y * outg) * 0.9;
        {
            double dcy = o - x->x_dcx1 + 0.9995 * x->x_dcy1;
            x->x_dcx1 = o;
            x->x_dcy1 = flushd(dcy);
            o = dcy;
        }
        if (!(o > -1.5 && o < 1.5)) {                  /* catches NaN/Inf too   */
            o = softclip(o);
            if (!(o > -1.5 && o < 1.5)) o = 0.0;
        }
        *out++ = (t_sample)(o * 0.80);
    }

    /* keep long-lived resonator state clean between blocks */
    for (i = 0; i < n; i++) {
        x->x_y1[i] = flushd(x->x_y1[i]); x->x_y2[i] = flushd(x->x_y2[i]);
        x->x_x1[i] = flushd(x->x_x1[i]); x->x_x2[i] = flushd(x->x_x2[i]);
    }
    return (w + 6);
}

static void alien_liquid3_dsp(t_alien_liquid3 *x, t_signal **sp)
{
    if (sp[0]->s_sr > 1000) x->x_sr = sp[0]->s_sr;
    x->x_haveblock = 0;             /* sr may have changed: force coeff rebuild */
    dsp_add(alien_liquid3_perform, 5, x,
            sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_liquid3_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_liquid3 *x = (t_alien_liquid3 *)pd_new(alien_liquid3_tilde_class);
    int i;
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;

    for (i = 0; i < MAXRES; i++) {
        x->x_y1[i] = x->x_y2[i] = x->x_x1[i] = x->x_x2[i] = 0.0;
        x->x_b0[i] = x->x_a1[i] = x->x_a2[i] = 0.0;
        x->x_g[i] = 0.0; x->x_active[i] = 0.0;
        x->x_fc[i] = 0.0; x->x_qcur[i] = 1.0;
        x->x_wob[i] = (double)(i * 5 % 17) / 17.0;
        x->x_wobinc[i] = 0.0;
        /* desynchronise the shimmer LFOs at load so partials boil out of phase */
        x->x_shim[i]    = (double)(i * 3 % 19) / 19.0;
        x->x_shiminc[i] = 0.0;
        x->x_shimoff[i] = (double)(i * 13 % 23) / 23.0;
    }
    x->x_phase = 0.0;
    x->x_noiselp = 0.0;
    x->x_exlp = 0.0;
    x->x_dcx1 = x->x_dcy1 = 0.0;
    x->x_rng = 22222u;
    x->x_lastmode = -1;
    load_mode(x, 0);

    x->x_haveblock = 0;
    x->x_lastmac = -1e30;
    x->x_lastf0  = -1e30;
    x->x_n = 0; x->x_norm = 1.0;
    x->x_exnoise = 0.05; x->x_eximp = 0.9; x->x_excut = 0.5;
    x->x_wobdepth = 0.0; x->x_shimdepth = 0.0; x->x_outg = 0.9;

    /* global modulation engine */
    x->x_lfo1 = 0.0;  x->x_lfo2 = 0.37; x->x_lfo3 = 0.71;
    x->x_lfo1inc = x->x_lfo2inc = x->x_lfo3inc = 0.0;
    x->x_rw = 0.0; x->x_rwtarget = 0.0; x->x_rwcount = 1;
    x->x_moddepth = 0.0; x->x_blockcount = 0;

    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_liquid3_tilde_setup(void)
{
    alien_liquid3_tilde_class = class_new(gensym("alien_liquid3~"),
        (t_newmethod)alien_liquid3_new, 0, sizeof(t_alien_liquid3),
        CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_liquid3_tilde_class, t_alien_liquid3, x_f);
    class_addmethod(alien_liquid3_tilde_class, (t_method)alien_liquid3_dsp,
                    gensym("dsp"), A_CANT, 0);
}
