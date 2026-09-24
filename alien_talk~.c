/* alien_talk~.c  --  a talking / vowel-wobble bass ("wow-wow")
 *
 * A rich polyBLEP saw + a sub sine an octave below the fundamental are poured
 * through a bank of SIX resonant bandpass formant filters (F1..F5 plus a high
 * "singer's"/brightness band) tuned to the formant centre frequencies of a
 * vowel, for a full, intelligible, vocal timbre.  The MACRO on inlet 1 is the
 * mouth:
 * it morphs the vowel along the dark->bright path u -> o -> a -> e -> i.  Feed
 * an LFO into inlet 1 and the bass "talks" -- a squelchy wow-wow as the vowel
 * sweeps.
 *
 * Why it does not zipper:
 *   The six formant filters are Chamberlin state-variable filters (SVF).
 *   Their two coefficients (f, q) are cheap functions of the target centre
 *   frequency, and crucially they can be recomputed PER SAMPLE without any
 *   trig -- f ~= 2*sin(pi*fc/sr) is approximated by its argument (2*pi*fc/sr)
 *   which is accurate and monotonic for the formant range, so the vowel
 *   position, and thus every formant's centre frequency, is interpolated
 *   per sample.  A fast LFO into inlet 1 therefore sweeps the vowel smoothly
 *   with no stepping.  The expensive per-block work is only the source mix,
 *   drive and the block's target vowel endpoints.
 *
 * A second, very slow internal vowel drift ("throat") is added on top so the
 * voice is never dead still, and a touch of drive lets it cut.
 *
 * SAFETY: build uses -ffast-math, so we rely on magnitude clamps rather than
 * isnan/isinf.  The SVF q is clamped so the poles stay inside the unit circle
 * (damping >= a small floor), centre freqs are clamped to (20, 0.47*sr), the
 * SVF f coefficient is clamped < ~1.4 for stability, all filter state is
 * denormal-flushed and finite-guarded every sample, and the output is
 * finite-guarded and bounded to ~[-1.5,1.5], nominal near 0.9.
 *
 *   inlet 0   signal/float   fundamental in Hz            (CLASS_MAINSIGNALIN)
 *   inlet 1   signal/float   MACRO 0..100 -- the vowel / mouth control
 *   outlet 0  signal
 *
 * build:
 *   gcc -shared -fPIC -O2 -o alien_talk~.pd_linux alien_talk~.c -lm
 *   (macOS: -bundle -undefined dynamic_lookup -o alien_talk~.pd_darwin)
 */

#include "m_pd.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define NFORM 6          /* formant bandpass resonators (F1-F5 + singer's formant) */

static t_class *alien_talk_tilde_class;

typedef struct _alien_talk {
    t_object x_obj;
    t_float  x_f;               /* scalar fallback for inlet 0 */

    double   x_sr;

    /* ---- source oscillators ---- */
    double   x_phsaw;           /* saw phase   */
    double   x_phsub;           /* sub phase   */

    /* ---- formant SVF state (2 states per filter) ---- */
    double   x_lp[NFORM];       /* lowpass state (integrator 1) */
    double   x_bp[NFORM];       /* bandpass state (integrator 2) */

    /* ---- per-sample smoothed vowel position ---- */
    double   x_vpos;            /* smoothed macro-driven vowel position 0..1 */

    /* ---- slow "throat" drift LFO ---- */
    double   x_thph;            /* throat LFO phase */

    /* ---- per-block cache ---- */
    double   x_vtarget;         /* block target vowel position 0..1 */
    double   x_drive;           /* block drive amount 0..1 */
    double   x_norm;            /* output normalisation */
    double   x_subamt;          /* sub sine amount */
    double   x_sawamt;          /* saw amount */

    double   x_lastmac;
    double   x_lastf0;
    int      x_haveblock;
} t_alien_talk;

/* ------------------------------------------------------------------ */
/* vowel formant centre frequencies + relative gains + bandwidths.     */
/* Now a full FIVE-formant bank (F1..F5) plus a sixth high "singer's    */
/* formant"/brightness band (~3000-3300 Hz cluster) that adds vocal     */
/* presence.  Realistic bass-vowel formant values; the higher formants  */
/* roll off in gain so the vowel identity (F1/F2/F3) still dominates.   */
/* Stored in perceptual order u -> o -> a -> e -> i so a rising macro   */
/* walks dark -> bright.                                                */
/*                                                                      */
/* Column 6 is the singer's/brightness formant -- a resonance placed a  */
/* touch above F3 (~2900-3300 Hz) that gives the voice ring and lets it */
/* cut, mirroring the real 3 kHz cluster in trained/loud voices.        */
/* ------------------------------------------------------------------ */
static const double VF[5][NFORM] = {
    /* F1    F2    F3    F4    F5     singer */
    {325,  700, 2530, 3500, 4950,  3050},   /* u */
    {450,  800, 2830, 3500, 4950,  3100},   /* o */
    {800, 1150, 2900, 3900, 4950,  3150},   /* a */
    {400, 1600, 2700, 3300, 4950,  3050},   /* e */
    {350, 1700, 2700, 3700, 4950,  3100}    /* i */
};
static const double VA[5][NFORM] = {
    /* F1    F2    F3    F4    F5    singer */
    {1.00, 0.60, 0.45, 0.35, 0.25, 0.22},   /* u */
    {1.00, 0.60, 0.45, 0.35, 0.25, 0.24},   /* o */
    {1.00, 0.60, 0.45, 0.35, 0.25, 0.30},   /* a */
    {1.00, 0.60, 0.45, 0.35, 0.25, 0.28},   /* e */
    {1.00, 0.60, 0.45, 0.35, 0.25, 0.26}    /* i */
};
static const double VB[5][NFORM] = {
    /* F1  F2   F3   F4   F5   singer */
    {60,  90, 130, 160, 200, 120},   /* u */
    {60,  90, 130, 160, 200, 120},   /* o */
    {70,  90, 130, 160, 200, 120},   /* a */
    {70,  90, 130, 160, 200, 120},   /* e */
    {60,  90, 130, 160, 200, 120}    /* i */
};

/* ------------------------------------------------------------------ */

static inline double softclip(double v)
{
    if (v >  3.0) return 1.0;
    if (v < -3.0) return -1.0;
    return v * (27.0 + v*v) / (27.0 + 9.0*v*v);
}

/* flush denormals and force finite; magnitude tests survive -ffast-math */
static inline double flushd(double v)
{
    if (!(v > -1e12 && v < 1e12)) return 0.0;   /* NaN/Inf/huge -> 0 */
    if (v > -1e-20 && v < 1e-20) return 0.0;    /* denormal   -> 0 */
    return v;
}

/* ------------------------------------------------------------------ */
/* polyBLEP saw: rich harmonics for the formants to shape, anti-aliased */
/* ------------------------------------------------------------------ */
static inline double poly_blep(double t, double dt)
{
    if (t < dt)       { t /= dt;            return t + t - t*t - 1.0; }
    if (t > 1.0 - dt) { t = (t - 1.0) / dt; return t*t + t + t + 1.0; }
    return 0.0;
}

static inline double blep_saw(double phase, double dt)
{
    return (2.0 * phase - 1.0) - poly_blep(phase, dt);
}

/* ------------------------------------------------------------------ */

/* Build every per-block quantity from one macro value + f0.  Called once per
 * block (only when the macro / f0 change).  The only per-block "vowel" work is
 * the target vowel POSITION; the actual per-formant centre freqs are computed
 * per sample from the interpolated position so the wobble never zippers. */
static void build_block(t_alien_talk *x, double mac, double f0)
{
    double t;

    if (!(mac == mac)) mac = 0.0;                    /* NaN guard */
    t = mac - 100.0 * floor(mac * 0.01);             /* wrap 0..100 */
    if (!(t >= 0.0 && t <= 100.0)) t = 0.0;
    t *= 0.01;                                       /* -> 0..1 */
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;

    x->x_vtarget = t;                                /* vowel position target */

    /* source mix + a bit of drive so it cuts.  The saw carries the formant
     * material; the sub sine an octave down fills the bottom.               */
    x->x_sawamt = 1.0;
    x->x_subamt = 0.5;
    x->x_drive  = 0.45;                              /* subtle, always-on grit */
    x->x_norm   = 4.2;                               /* lift the fuller bank   */

    (void)f0;
    x->x_lastmac = mac;
    x->x_lastf0  = f0;
    x->x_haveblock = 1;
}

static t_int *alien_talk_perform(t_int *w)
{
    t_alien_talk *x = (t_alien_talk *)(w[1]);
    t_sample *fin = (t_sample *)(w[2]);
    t_sample *min = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    int nsamp = (int)(w[5]);

    double sr = x->x_sr;
    double nyq = sr * 0.47;
    double mac0 = min[0], f00 = fin[0];

    double vtarget, drive, norm, subamt, sawamt;
    double phsaw = x->x_phsaw, phsub = x->x_phsub;
    double vpos = x->x_vpos, thph = x->x_thph;
    double drive_in, drive_out;
    double vsmooth, throat_inc;
    int i;

    if (!x->x_haveblock || mac0 != x->x_lastmac || f00 != x->x_lastf0)
        build_block(x, mac0, f00);

    vtarget = x->x_vtarget; drive = x->x_drive; norm = x->x_norm;
    subamt = x->x_subamt; sawamt = x->x_sawamt;

    drive_in  = 1.0 + drive * 6.0;
    drive_out = 1.0 / (1.0 + drive * 1.5);

    /* per-sample smoothing coefficient for the vowel position.  Fast enough
     * that an audio-ish LFO into inlet 1 tracks, slow enough to kill any
     * block-boundary step -> the classic zipper cure.  ~800 Hz one-pole.   */
    vsmooth = 1.0 - exp(-2.0 * M_PI * 800.0 / sr);
    if (!(vsmooth > 0.0 && vsmooth < 1.0)) vsmooth = 0.2;

    /* slow secondary "throat" vowel drift, ~0.13 Hz */
    throat_inc = 0.13 / sr;

    while (nsamp--) {
        double f0 = *fin++;
        double vp, throat, dt, saw, sub, src, y, o;
        double vpf, fi0, vf, amp;
        min++;                                        /* macro read per block */

        if (!(f0 > 0.0)) f0 = 0.01;                   /* NaN/inf/<=0 guard */
        if (f0 < 0.01)   f0 = 0.01;
        if (f0 > 8000.0) f0 = 8000.0;

        /* ---- smooth the vowel position toward the block target (per sample)
         * so a fast LFO into inlet 1 wobbles the vowel with no zipper ---- */
        vpos += (vtarget - vpos) * vsmooth;

        /* ---- slow throat drift added on top, tiny so the vowel identity
         * stays but the voice is never perfectly still ---- */
        thph += throat_inc;
        if (thph >= 1.0) thph -= 1.0;
        throat = 0.06 * sin(thph * 2.0 * M_PI);

        vp = vpos + throat;
        if (vp < 0.0) vp = 0.0;
        if (vp > 1.0) vp = 1.0;

        /* ---- source: polyBLEP saw + sub sine an octave below ---- */
        dt = f0 / sr;
        if (dt > 0.45) dt = 0.45;
        phsaw += dt; if (phsaw >= 1.0) phsaw -= 1.0;
        saw = blep_saw(phsaw, dt);

        phsub += dt * 0.5; if (phsub >= 1.0) phsub -= 1.0;
        sub = sin(phsub * 2.0 * M_PI);

        src = sawamt * saw + subamt * sub;

        /* ---- formant bank: 6 resonant bandpass SVFs (F1..F5 + singer's).
         * Centre freq, gain and
         * bandwidth of each formant are interpolated PER SAMPLE from the
         * smoothed vowel position -> smooth talking wobble.  Chamberlin SVF:
         *   lp += f * bp
         *   hp  = in - lp - q * bp
         *   bp += f * hp
         * with f ~= 2*pi*fc/sr (accurate + monotonic in range) and q the
         * damping.  We take the bandpass output.                            */
        vpf = vp * 4.0;                               /* index into 5 vowels */
        fi0 = floor(vpf);
        if (fi0 > 3.0) fi0 = 3.0;
        if (fi0 < 0.0) fi0 = 0.0;
        vf  = vpf - fi0;                              /* 0..1 between vowels */
        {
            int a0 = (int)fi0, a1 = a0 + 1;
            if (a1 > 4) a1 = 4;

            y = 0.0;
            for (i = 0; i < NFORM; i++) {
                double fc, bw, g, f, q, qmin, hp;

                /* interpolate this formant's centre freq / gain / bandwidth */
                fc  = VF[a0][i] + (VF[a1][i] - VF[a0][i]) * vf;
                amp = VA[a0][i] + (VA[a1][i] - VA[a0][i]) * vf;
                bw  = VB[a0][i] + (VB[a1][i] - VB[a0][i]) * vf;

                /* clamp centre freq into (20, 0.47*sr) */
                if (fc < 20.0)  fc = 20.0;
                if (fc > nyq)   fc = nyq;

                /* SVF frequency coefficient; keep < ~1.4 for stability */
                f = 2.0 * M_PI * fc / sr;
                if (f > 1.4)  f = 1.4;
                if (f < 1e-5) f = 1e-5;

                /* damping q from the bandwidth: q = 2*pi*bw/sr, i.e. the pole
                 * radius is ~1 - q/2.  Clamp so poles stay inside the unit
                 * circle (radius <= ~0.9995) -> high resonance but stable.   */
                if (bw < 10.0)   bw = 10.0;
                q = 2.0 * M_PI * bw / sr;
                qmin = 2.0 * (1.0 - 0.9995);           /* pole radius <= 0.9995 */
                if (q < qmin) q = qmin;                /* squelchy vocal peak   */
                if (q > 1.9)  q = 1.9;                 /* stay stable           */

                /* one SVF step (bandpass tap) */
                hp = src - x->x_lp[i] - q * x->x_bp[i];
                x->x_bp[i] = flushd(x->x_bp[i] + f * hp);
                x->x_lp[i] = flushd(x->x_lp[i] + f * x->x_bp[i]);

                /* bandpass, scaled so higher-q peaks don't rail: multiply by q
                 * (band gain ~ 1/q at resonance) and the formant's amp.  Lift
                 * F2/F3 a touch so the vowel-defining motion reads clearly as
                 * the mouth sweeps, but cap the lift so the five upper bands
                 * don't pile up and rail.                                     */
                {
                    double lift = 1.0 + 0.5 * (double)i;
                    if (lift > 2.2) lift = 2.2;         /* cap upper-band boost */
                    g = x->x_bp[i] * q * lift;
                }
                y += g * amp;
            }
        }

        /* ---- drive so it cuts, then normalise ---- */
        y *= norm;
        o = softclip(y * drive_in) * drive_out;

        /* mix a little dry sub back in for weight (keeps the bass grounded) */
        o += 0.12 * sub * subamt;

        /* ---- output safety: finite + bounded ~[-1.5,1.5], nominal ~0.9 --- */
        if (!(o > -1.5 && o < 1.5)) {
            o = softclip(o);
            if (!(o > -1.5 && o < 1.5)) o = 0.0;
        }
        *out++ = (t_sample)(o * 0.9);
    }

    /* persist state */
    x->x_phsaw = phsaw; x->x_phsub = phsub;
    x->x_vpos = flushd(vpos); x->x_thph = thph;
    for (i = 0; i < NFORM; i++) {
        x->x_lp[i] = flushd(x->x_lp[i]);
        x->x_bp[i] = flushd(x->x_bp[i]);
    }
    return (w + 6);
}

static void alien_talk_dsp(t_alien_talk *x, t_signal **sp)
{
    if (sp[0]->s_sr > 1000) x->x_sr = sp[0]->s_sr;
    dsp_add(alien_talk_perform, 5, x,
            sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_talk_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_talk *x = (t_alien_talk *)pd_new(alien_talk_tilde_class);
    int i;
    (void)s; (void)argc; (void)argv;                  /* A_GIMME: ignore args */

    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;

    x->x_phsaw = 0.0; x->x_phsub = 0.0;
    for (i = 0; i < NFORM; i++) { x->x_lp[i] = 0.0; x->x_bp[i] = 0.0; }
    x->x_vpos = 0.0; x->x_thph = 0.0;

    x->x_vtarget = 0.0; x->x_drive = 0.35; x->x_norm = 0.9;
    x->x_subamt = 0.6; x->x_sawamt = 0.9;

    x->x_haveblock = 0;
    x->x_lastmac = -1e30;
    x->x_lastf0  = -1e30;

    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_talk_tilde_setup(void)
{
    alien_talk_tilde_class = class_new(gensym("alien_talk~"),
        (t_newmethod)alien_talk_new, 0, sizeof(t_alien_talk),
        CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_talk_tilde_class, t_alien_talk, x_f);
    class_addmethod(alien_talk_tilde_class, (t_method)alien_talk_dsp,
                    gensym("dsp"), A_CANT, 0);
}
