/* alien_reese~.c  --  a neurofunk reese / growl bass
 *
 * The reese is the sound of a detuned-saw unison beating against a MOVING
 * notch.  What turns a plain reese into a neurofunk GROWL is putting that
 * movement under an LFO and driving the mids hard: as the notch sweeps, it
 * carves a walking series of holes through the phase-interference spectrum,
 * and the mid-band distortion turns every sweep into a snarl.  This voice
 * makes inlet 1 (the 0..100 macro) that movement control: feed it a signal
 * LFO and the notch, the detune spread, the inter-oscillator FM index and the
 * mid drive all sweep together, so a wobble on inlet 1 is a growling
 * undulation.
 *
 * Signal chain (nonlinear stages at 2x oversampling):
 *
 *   4 detuned polyBLEP saws (unison, cents spread)   ]
 *      + a shared FM modulator at an integer ratio    ]--> feedforward comb /
 *      phase-modulating the saws (metallic edge)      ]    allpass notch (swept)
 *                                                          --> mid-band tanh
 *                                                              distortion
 *                                                          --> recombine + sub
 *
 *   - SOURCE.  Four saws detuned by a few..many cents (spread swept by the
 *     macro) give the classic reese phase-interference beating.  A shared
 *     sine modulator at an integer ratio phase-modulates all four saws; its
 *     index is swept by the macro for a metallic, vocal edge.
 *   - MOVING NOTCH.  A feedforward comb whose delay = SR / notch_freq, with
 *     notch_freq read PER SAMPLE from the (smoothed) macro, so a fast LFO on
 *     inlet 1 slides the notch smoothly with no zipper.  A second allpass
 *     notch adds a resonant second hole.  Comb feedforward/feedback are
 *     strictly < 1 and the delay index is clamped in-bounds.
 *   - MID DISTORTION (pseudo-multiband).  A state-variable bandpass around
 *     ~900 Hz taps the mids, drives them hard through tanh, and recombines
 *     them over the (softer-driven) full band -- the neurofunk trick that
 *     makes the growl sit forward.  The nonlinear stage runs at 2x and is
 *     decimated back down.
 *
 *   inlet 0   signal/float   fundamental in Hz  (CLASS_MAINSIGNALIN)
 *   inlet 1   signal/float   MACRO 0..100 -- the MOVEMENT control (wobble this)
 *   outlet 0  signal
 *
 * Safety (build uses -ffast-math, so magnitude clamps do the work): comb /
 * allpass feedback strictly < 1; delay index clamped in-bounds; FM index and
 * tanh drive clamped; f0 clamped >= 0.01; all state denormal-flushed and
 * finite-guarded; output finite-guarded and bounded ~[-1.5,1.5], nominal near
 * 0.9.  Sub content is welcome and encouraged.
 *
 * build:
 *   gcc -shared -fPIC -O2 -o alien_reese~.pd_linux alien_reese~.c -lm
 *   (macOS: -bundle -undefined dynamic_lookup -o alien_reese~.pd_darwin)
 */

#include "m_pd.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define OS    2              /* oversampling factor for the nonlinear stage    */
#define NSAW  4              /* detuned saws in the unison                      */
#define DLEN  4096           /* comb delay line length (power of 2)            */
#define DMASK (DLEN - 1)

static t_class *alien_reese_tilde_class;

typedef struct _alien_reese {
    t_object x_obj;
    t_float  x_f;              /* scalar fallback for inlet 0                   */

    double   x_sr;             /* oversampled sample rate                       */

    /* ---- oscillators ---- */
    double   x_ph[NSAW];       /* saw phases                                    */
    double   x_phm;            /* shared FM modulator phase                     */
    double   x_phsub;          /* sub oscillator phase                          */

    /* ---- moving comb / notch ---- */
    double   x_buf[DLEN];      /* comb delay line                               */
    int      x_wi;             /* comb write index                              */
    double   x_ap;             /* allpass notch state                           */
    double   x_apz;            /* allpass notch second state                    */

    /* ---- mid-band distortion (state-variable bandpass) ---- */
    double   x_bpl, x_bpb;     /* SVF lowpass / bandpass integrator states      */

    /* ---- macro smoothing (kills zipper on a fast LFO) ---- */
    double   x_mac;            /* smoothed macro 0..1                           */

    /* ---- decimation lowpass ---- */
    double   x_d1, x_d2;

    /* ---- DC blocker ---- */
    double   x_dcx, x_dcy;
} t_alien_reese;

/* ------------------------------------------------------------------ */
/* polyBLEP band-limited saw                                           */
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

/* cheap tanh-ish saturator, bounded to [-1,1] */
static inline double softclip(double v)
{
    if (v >  3.0) return 1.0;
    if (v < -3.0) return -1.0;
    return v * (27.0 + v*v) / (27.0 + 9.0*v*v);
}

/* flush denormals and force finite; magnitude tests survive -ffast-math */
static inline double flushd(double v)
{
    if (!(v > -1e12 && v < 1e12)) return 0.0;   /* NaN/Inf/huge -> 0            */
    if (v > -1e-20 && v < 1e-20) return 0.0;    /* denormal -> 0               */
    return v;
}

/* interpolated read from the comb line, `d` samples behind the write head */
static inline double combread(t_alien_reese *x, double d)
{
    double rp, fr;
    int i0, i1;
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

/* ------------------------------------------------------------------ */

static t_int *alien_reese_perform(t_int *w)
{
    t_alien_reese *x = (t_alien_reese *)(w[1]);
    t_sample *fin = (t_sample *)(w[2]);
    t_sample *min = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    int nsamp = (int)(w[5]);

    double sr  = x->x_sr;
    double nyq = sr * 0.45;
    if (!(sr > 0.0)) sr = 44100.0 * OS;

    /* pull state into locals */
    double phm = x->x_phm, phsub = x->x_phsub;
    double macs = x->x_mac;
    double bpl = x->x_bpl, bpb = x->x_bpb;
    double d1 = x->x_d1, d2 = x->x_d2;
    double dcx = x->x_dcx, dcy = x->x_dcy;
    double ph[NSAW];
    int i, o;
    for (i = 0; i < NSAW; i++) ph[i] = x->x_ph[i];

    /* fixed decimation coefficient (2-pole @ oversampled rate) */
    const double da_wc = tan(M_PI * 0.22);
    const double da_a  = da_wc / (1.0 + da_wc);

    /* macro smoothing coefficient: fast enough to follow an 8 Hz LFO, slow
     * enough to kill sample-to-sample zipper.  ~250 Hz one-pole. */
    const double mac_a = 1.0 - exp(-2.0 * M_PI * 250.0 / sr);

    while (nsamp--) {
        double f0 = *fin++;
        double mraw = *min++;
        double mt, s, sub, band, wide, wr, fb_read, o_out;
        double detune, fmidx, notchhz, drive, midgain, combfb;
        double dtm, mod;

        /* ---- f0 guard ---- */
        if (!(f0 == f0)) f0 = 0.01;                 /* NaN -> floor            */
        if (f0 < 0.01)   f0 = 0.01;
        if (f0 > 8000.0) f0 = 8000.0;

        /* ---- macro: wrap 0..100 -> [0,1), then smooth PER SAMPLE ---- */
        mt = mraw - 100.0 * floor(mraw * 0.01);
        if (!(mt >= 0.0 && mt <= 100.0)) mt = 0.0;  /* NaN/inf guard           */
        mt *= 0.01;                                  /* [0,1)                   */
        macs += mac_a * (mt - macs);                 /* one-pole smoother       */
        if (!(macs >= 0.0)) macs = 0.0;
        if (macs > 1.0) macs = 1.0;

        /* ---- macro -> the four movement targets (all swept together) ----
         * everything here is cheap so it can run per sample, which is what
         * lets a fast LFO on inlet 1 slide the notch smoothly. */
        detune  = 0.02 + 0.98 * macs;                /* unison spread 0..1     */
        fmidx   = 0.15 * macs * macs;                /* FM index (clamped)     */
        drive   = 1.0 + 7.0 * macs * macs;           /* mid distortion drive   */
        midgain = 0.35 + 0.65 * macs;                /* mid-band emphasis      */
        combfb  = 0.55 + 0.35 * macs;                /* comb resonance < 0.92  */
        if (combfb > 0.92) combfb = 0.92;            /* strictly < 1           */

        /* notch frequency walks with the macro; tuned to land near low
         * harmonics of f0 for a harmonic growl, but bounded to a sane band so
         * the comb delay stays in-bounds.  ~40 Hz (deep, sub-carving) up to a
         * few kHz. */
        notchhz = 40.0 * pow(2.0, 6.2 * macs);       /* ~40 Hz .. ~2.9 kHz     */
        if (notchhz < 20.0)   notchhz = 20.0;
        if (notchhz > nyq*0.9) notchhz = nyq*0.9;

        /* per-sample FM modulator (shared, integer ratio = 1) advanced at the
         * base rate so it phase-modulates the saws coherently */
        dtm = f0 / sr;
        if (dtm > 0.45) dtm = 0.45;

        s = 0.0;
        for (o = 0; o < OS; o++) {
            double edge = 0.0;

            /* shared FM modulator (a sine at ratio 1); its output modulates
             * every saw's phase, adding a metallic sideband structure. */
            phm += dtm; if (phm >= 1.0) phm -= 1.0;
            mod = fmidx * sin(phm * 2.0 * M_PI);      /* bounded: |mod|<=0.15   */

            /* ---- detuned saw unison ----
             * cents spread grows with the macro; four saws at symmetric
             * detune offsets beat against each other -> the reese. */
            {
                double acc = 0.0;
                /* cents offsets: -1.5, -0.5, +0.5, +1.5 units scaled by spread */
                static const double off[NSAW] = { -1.5, -0.5, 0.5, 1.5 };
                for (i = 0; i < NSAW; i++) {
                    double cents = off[i] * (4.0 + 26.0 * detune); /* up to ~+-49c */
                    double ratio = pow(2.0, cents / 1200.0);
                    double dt = f0 * ratio / sr;
                    double pp;
                    if (dt > 0.45) dt = 0.45;
                    ph[i] += dt;
                    if (ph[i] >= 1.0) ph[i] -= 1.0;
                    else if (ph[i] < 0.0) ph[i] += 1.0;
                    /* phase-modulate by the shared modulator (wrapped read) */
                    pp = ph[i] + mod;
                    pp -= floor(pp);
                    acc += blep_saw(pp, dt);
                }
                s = acc * (1.0 / (double)NSAW);
            }
            (void)edge;

            /* ---- MOVING COMB / NOTCH ----
             * feedforward comb: y = x - g*x[n-D], with a bounded feedback
             * term for resonance.  D = SR / notchhz places the first notch at
             * notchhz.  D is clamped in combread(). */
            {
                double D = sr / notchhz;
                fb_read = combread(x, D);
                /* write input + resonant feedback (strictly < 1) */
                wr = s + combfb * fb_read;
                if (wr >  4.0) wr =  4.0;             /* keep the line bounded  */
                else if (wr < -4.0) wr = -4.0;
                x->x_buf[x->x_wi] = flushd(wr);
                x->x_wi = (x->x_wi + 1) & DMASK;
                /* feedforward notch on the output */
                s = 0.5 * (s - fb_read);
            }

            /* ---- allpass notch (a second, resonant hole) ----
             * a first-order allpass whose coefficient tracks the notch freq;
             * subtracting it from the signal makes a notch that moves with the
             * macro too.  Coefficient magnitude < 1 -> stable. */
            {
                double wc = 2.0 * notchhz / sr;       /* normalised            */
                double c  = (1.0 - wc) / (1.0 + wc);  /* |c| < 1               */
                double apo;
                if (c >  0.999) c =  0.999;
                if (c < -0.999) c = -0.999;
                apo = -c * s + x->x_ap;
                x->x_ap  = flushd(s + c * apo);
                /* cascade a second stage for a deeper notch */
                x->x_apz = flushd(0.5 * (apo + x->x_apz));
                s = 0.5 * (s + apo);                  /* notch = mix with AP    */
            }

            /* ---- MID-BAND DISTORTION (pseudo-multiband) ----
             * a state-variable filter taps a band around ~900 Hz, that band is
             * driven hard through tanh, and recombined over the full band
             * (driven softer).  Emphasising the mids is the neurofunk move. */
            {
                double fc = 900.0;
                double g  = tan(M_PI * fc / sr);
                double R  = 0.7;                       /* SVF damping           */
                double denom = 1.0 + 2.0 * R * g + g * g;
                double hp, lp, bp;
                double mid_hot, full;
                if (!(denom > 1e-9)) denom = 1e-9;
                hp = (s - (2.0 * R + g) * bpb - bpl) / denom;
                bp = g * hp + bpb;
                lp = g * bp + bpl;
                bpb = flushd(bp);
                bpl = flushd(lp);
                if (bpb >  8.0) bpb =  8.0; else if (bpb < -8.0) bpb = -8.0;
                if (bpl >  8.0) bpl =  8.0; else if (bpl < -8.0) bpl = -8.0;

                band = bp;                             /* the mid band          */
                wide = s - band;                       /* everything else       */

                mid_hot = softclip(band * drive * midgain);
                full    = softclip(wide * (0.6 + 0.4 * (drive * 0.15)));
                s = mid_hot * 0.7 + full * 0.9;
            }

            /* per-oversample decimation lowpass */
            d1 += da_a * (s - d1);
            d2 += da_a * (d1 - d2);
        }
        s = d2;

        /* ---- sub oscillator (an octave down, always welcome) ----
         * kept out of the oversampled loop (a pure sine, no aliasing). */
        phsub += (f0 * 0.5) / sr;
        if (phsub >= 1.0) phsub -= 1.0;
        sub = sin(phsub * 2.0 * M_PI);
        /* sub fades slightly as the growl opens so the mids can sit forward */
        s = s + sub * (0.55 - 0.20 * macs);

        /* ---- DC blocker (the comb/notch + asymmetric drive add offset) ---- */
        {
            double y = s - dcx + 0.9995 * dcy;
            dcx = s;
            dcy = flushd(y);
            s = y;
        }

        /* ---- output safety: finite + bounded ~[-1.5,1.5], nominal ~0.9 ---- */
        o_out = s * 0.55;
        if (!(o_out > -1.5 && o_out < 1.5)) {         /* catches NaN/Inf too    */
            o_out = softclip(o_out);
            if (!(o_out > -1.5 && o_out < 1.5)) {
                o_out = 0.0;
                /* poisoned state -> reset the fragile integrators */
                bpl = bpb = 0.0; d1 = d2 = 0.0;
            }
        }
        *out++ = (t_sample)(o_out * 0.88);
    }

    /* store state back */
    for (i = 0; i < NSAW; i++) x->x_ph[i] = ph[i];
    x->x_phm = phm; x->x_phsub = phsub;
    x->x_mac = macs;
    x->x_bpl = flushd(bpl); x->x_bpb = flushd(bpb);
    x->x_d1  = flushd(d1);  x->x_d2  = flushd(d2);
    x->x_dcx = flushd(dcx); x->x_dcy = flushd(dcy);
    x->x_ap  = flushd(x->x_ap);
    x->x_apz = flushd(x->x_apz);
    return (w + 6);
}

static void alien_reese_dsp(t_alien_reese *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr * OS;
    if (x->x_sr < 1000) x->x_sr = 44100.0 * OS;
    dsp_add(alien_reese_perform, 5, x,
            sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_reese_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_reese *x = (t_alien_reese *)pd_new(alien_reese_tilde_class);
    int i;
    (void)s; (void)argc; (void)argv;

    x->x_sr = sys_getsr() * OS;
    if (x->x_sr < 1000) x->x_sr = 44100.0 * OS;

    /* spread starting phases so the unison isn't a unison pump at note-on */
    for (i = 0; i < NSAW; i++) x->x_ph[i] = (double)(i * 7 % 13) / 13.0;
    x->x_phm = 0.0;
    x->x_phsub = 0.0;

    memset(x->x_buf, 0, sizeof(x->x_buf));
    x->x_wi = 0;
    x->x_ap = x->x_apz = 0.0;
    x->x_bpl = x->x_bpb = 0.0;
    x->x_mac = 0.0;
    x->x_d1 = x->x_d2 = 0.0;
    x->x_dcx = x->x_dcy = 0.0;

    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_reese_tilde_setup(void)
{
    alien_reese_tilde_class = class_new(gensym("alien_reese~"),
        (t_newmethod)alien_reese_new, 0, sizeof(t_alien_reese),
        CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_reese_tilde_class, t_alien_reese, x_f);
    class_addmethod(alien_reese_tilde_class, (t_method)alien_reese_dsp,
                    gensym("dsp"), A_CANT, 0);
}
