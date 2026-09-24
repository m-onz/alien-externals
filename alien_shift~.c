/* alien_shift~.c  --  frequency-shifting lead (Bode / barberpole)
 *
 * A rich oscillator run through a single-sideband FREQUENCY SHIFTER.  Ring
 * modulation (which [alien_dual~] has) multiplies and gives symmetric
 * sidebands; a frequency shifter moves every partial by the SAME number of
 * HERTZ, not a ratio.  So a harmonic saw (100 200 300 ...) shifted by 60 Hz
 * becomes 160 260 360 ... -- no longer a harmonic series, and instantly
 * metallic and clangorous, in a way ring mod cannot do.
 *
 * Feed the shifted output back into the shifter and the spectrum cascades:
 * partials keep climbing forever -- a Shepard/barberpole glide that never
 * arrives.  That, and the shift itself, are where the alien character lives.
 *
 * The two sidebands come out separately -- up-shift left, down-shift right --
 * so a single note is already a wide, shimmering stereo image.
 *
 * The 90-degree split uses a polyphase-IIR Hilbert transformer (two allpass
 * chains a quarter cycle apart).
 *
 *   inlet 0   signal/float   oscillator pitch in Hz  (use [mtof] for MIDI)
 *   inlet 1   signal/float   MACRO 0..100 (wrapped mod 100) -- one knob that
 *                            morphs the whole voice: a pure mellow sine barely
 *                            shifted at 0, through a saw/square getting a
 *                            growing metallic shift, into a fed-back driven
 *                            noise barberpole clangour at 100.  It sweeps the
 *                            source waveform, shift amount, pitch-tracking,
 *                            feedback, pulse width and drive together along a
 *                            hand-tuned trajectory.
 *
 *   outlet 0  signal         up-shifted
 *   outlet 1  signal         down-shifted   (the two make a stereo pair)
 *
 * build:
 *   gcc -shared -fPIC -O2 -o alien_shift~.pd_linux alien_shift~.c -lm
 *   (macOS: -bundle -undefined dynamic_lookup -o alien_shift~.pd_darwin)
 */

#include "m_pd.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TWOPI (2.0 * M_PI)
#define NHIL  4          /* allpass stages per Hilbert chain */

static t_class *alien_shift_tilde_class;

/* denormal flush: kill subnormals so the allpass/Hilbert chains don't stall
 * the CPU (the classic Pd "stutter").  Uses a small offset test that survives
 * -ffast-math (where isnan/isfinite may be optimised away). */
#define DENORM_THRESH 1e-15
static inline double flush_denorm(double v)
{
    if (v > -DENORM_THRESH && v < DENORM_THRESH) return 0.0;
    return v;
}
/* finite-guard that works under -ffast-math: NaN fails every comparison,
 * Inf is caught by the magnitude bound. */
static inline double finite_or_zero(double v)
{
    if (v > -1e12 && v < 1e12) return v;
    return 0.0;
}

/* polyphase IIR Hilbert allpass coefficients (squared pole radii) */
static const double HA[NHIL] =
    {0.6923877778065970,0.9360654322959236,0.9882295226860060,0.9987488452737050};
static const double HB[NHIL] =
    {0.4021921162426028,0.8561710882420250,0.9722909545651573,0.9952884791278394};

typedef struct _alien_shift {
    t_object x_obj;
    t_float  x_f;

    double   x_sr;
    double   x_ph;             /* source phase */
    double   x_sph;            /* shift-oscillator phase */
    double   x_freq;
    double   x_prev;           /* feedback state */

    /* Hilbert chain states: 2nd-order allpass, need x[n-2],y[n-2] */
    double   x_ax1[NHIL], x_ax2[NHIL], x_ay1[NHIL], x_ay2[NHIL];
    double   x_bx1[NHIL], x_bx2[NHIL], x_by1[NHIL], x_by2[NHIL];
    double   x_bdelay;         /* 1-sample delay feeding chain B */

    unsigned x_rng;
    t_outlet *x_down;
} t_alien_shift;

static unsigned rn(t_alien_shift *x)
{
    x->x_rng ^= x->x_rng << 13; x->x_rng ^= x->x_rng >> 17;
    x->x_rng ^= x->x_rng << 5;  return x->x_rng;
}
static double frnd(t_alien_shift *x){ return (double)(rn(x)&0xffffff)/(double)0x800000 - 1.0; }

static inline double softclip(double v)
{
    if (v >  3.0) return 1.0;
    if (v < -3.0) return -1.0;
    return v * (27.0 + v*v) / (27.0 + 9.0*v*v);
}

/* one 2nd-order allpass: (a + z^-2)/(1 + a z^-2) */
static inline double allpass2(double in, double a,
                              double *x1, double *x2, double *y1, double *y2)
{
    double y = a * in + *x2 - a * (*y2);
    y = flush_denorm(y);
    *x2 = *x1; *x1 = in;
    *y2 = *y1; *y1 = y;
    return y;
}

/* ------------------------------------------------------------------ */

/* map the 0..1 macro onto the whole parameter set (curated morph).
 * source is discrete: sweep sine -> tri -> saw -> square -> pulse -> noise
 * as t climbs, so timbre thickens together with the shift/feedback/drive. */
/* DISCRETE source select, derived once per block (branchy, so keep it out of
 * the per-sample loop).  brightest/noisiest at the top. */
static inline int macro_source(double t)
{
    if      (t < 0.14) return 4;   /* sine   */
    else if (t < 0.30) return 3;   /* tri    */
    else if (t < 0.55) return 0;   /* saw    */
    else if (t < 0.74) return 1;   /* square */
    else if (t < 0.90) return 2;   /* pulse  */
    else               return 5;   /* noise  */
}

/* CONTINUOUS parameters -- cheap arithmetic, safe to read per sample so the
 * sweep stays smooth.  All outputs are bounded and feedback is hard-limited
 * strictly below 1 so the loop can never run away. */
static inline void macro_cont(double t, double *shift, double *track,
                              double *feedback, double *pw, double *drive)
{
    /* shift grows from a gentle chorus to a big clangorous inharmonic move */
    *shift    = 4.0 + 1200.0 * t * t;         /* ~4Hz -> ~1200Hz            */
    /* let it track pitch a little more as we climb so it stays playable    */
    *track    = 0.5 * t;                      /* fixed Hz -> half-tracking   */
    /* feedback only bites in the upper half -> barberpole cascade at top.
     * capped at 0.8 (strictly < 1) so the feedback loop stays stable.      */
    *feedback = (t < 0.5) ? 0.0 : (t - 0.5) / 0.5 * 0.8;
    if (*feedback > 0.8) *feedback = 0.8;     /* hard ceiling, never >= 1    */
    /* pulse width narrows for a thinner, buzzier pulse up top              */
    *pw       = 0.5 - 0.45 * t;               /* 0.5 -> ~0.05                */
    if (*pw < 0.02) *pw = 0.02;
    /* grit only up top                                                     */
    *drive    = (t < 0.6) ? 0.0 : (t - 0.6) / 0.4;
    if (*drive < 0.0) *drive = 0.0;
    if (*drive > 1.0) *drive = 1.0;
}

static t_int *alien_shift_perform(t_int *w)
{
    t_alien_shift *x = (t_alien_shift *)(w[1]);
    t_sample *fin = (t_sample *)(w[2]);
    t_sample *min = (t_sample *)(w[3]);
    t_sample *up  = (t_sample *)(w[4]);
    t_sample *dn  = (t_sample *)(w[5]);
    int nsamp = (int)(w[6]);

    double sr = x->x_sr;
    /* fixed short glide so pitch changes stay smooth (was the [glide( param) */
    double glidea = 1.0 - exp(-1.0 / (0.001 * 5.0 * sr));
    int i;

    /* DISCRETE source: derive ONCE per block from the macro's first sample.
     * quantising per block keeps the branchy select out of the hot loop and
     * avoids zipper noise from a per-sample waveform switch. */
    double mac0 = min[0];
    double t0 = mac0 - 100.0 * floor(mac0 * 0.01);
    if (!(t0 >= 0.0 && t0 <= 100.0)) t0 = 0.0;    /* NaN/inf guard */
    t0 *= 0.01;
    int src = macro_source(t0);

    while (nsamp--) {
        double ftarget = *fin++, mac = *min++;
        double s, in, I, Q, cs, sn, uo, dobd;
        double shifthz, t, shift, track, feedback, pw, drive;

        /* wrap the macro into [0,100) then normalise to [0,1) */
        t = mac - 100.0 * floor(mac * 0.01);
        if (!(t >= 0.0 && t <= 100.0)) t = 0.0;   /* NaN/inf guard */
        t *= 0.01;
        macro_cont(t, &shift, &track, &feedback, &pw, &drive);

        if (!(ftarget > 0.01)) ftarget = 0.01;    /* also catches NaN */
        if (ftarget > 8000.0)  ftarget = 8000.0;
        x->x_freq += glidea * (ftarget - x->x_freq);
        if (x->x_freq < 0.01) x->x_freq = 0.01;   /* clamp f0 >= ~0.01 */

        /* source oscillator */
        x->x_ph += x->x_freq / sr;
        if (x->x_ph >= 1.0) x->x_ph -= 1.0;
        switch (src) {
            case 1:  s = (x->x_ph < 0.5) ? 1.0 : -1.0; break;
            case 2:  s = (x->x_ph < pw) ? 1.0 : -1.0; break;
            case 3:  s = 4.0 * fabs(x->x_ph - 0.5) - 1.0; break;
            case 4:  s = sin(TWOPI * x->x_ph); break;
            case 5:  s = frnd(x); break;
            default: s = 2.0 * x->x_ph - 1.0; break;      /* saw */
        }

        /* feedback: state is already soft-clipped when stored, but re-clamp
         * defensively (tanh bounds it to (-1,1)) and flush any denormal so a
         * quiet loop never stalls the CPU. */
        in = s + feedback * tanh(1.5 * x->x_prev);
        in = flush_denorm(in * 0.7);

        /* Hilbert: chain A on in, chain B on in delayed one sample */
        I = in;
        for (i = 0; i < NHIL; i++)
            I = allpass2(I, HA[i], &x->x_ax1[i], &x->x_ax2[i], &x->x_ay1[i], &x->x_ay2[i]);
        {
            double bd = x->x_bdelay;
            x->x_bdelay = in;
            Q = bd;
            for (i = 0; i < NHIL; i++)
                Q = allpass2(Q, HB[i], &x->x_bx1[i], &x->x_bx2[i], &x->x_by1[i], &x->x_by2[i]);
        }

        /* shift oscillator */
        shifthz = shift + track * x->x_freq;
        x->x_sph += shifthz / sr;
        if (x->x_sph >= 1.0) x->x_sph -= 1.0;
        else if (x->x_sph < 0.0) x->x_sph += 1.0;
        cs = cos(TWOPI * x->x_sph);
        sn = sin(TWOPI * x->x_sph);

        uo   = I * cs - Q * sn;        /* up-shift  */
        dobd = I * cs + Q * sn;        /* down-shift */

        if (drive > 0.0) {
            uo   = softclip(uo   * (1.0 + drive * 6.0)) / (1.0 + drive * 1.5);
            dobd = softclip(dobd * (1.0 + drive * 6.0)) / (1.0 + drive * 1.5);
        } else { uo = softclip(uo); dobd = softclip(dobd); }

        /* final safety: finite, then a soft ceiling well inside [-1.5,1.5].
         * softclip already bounds to (-1,1); this catches any Inf/NaN that
         * slipped through the chains under -ffast-math before it re-enters
         * the feedback loop and poisons every later sample. */
        uo   = finite_or_zero(uo);
        dobd = finite_or_zero(dobd);
        if (uo   >  1.5) uo   =  1.5; else if (uo   < -1.5) uo   = -1.5;
        if (dobd >  1.5) dobd =  1.5; else if (dobd < -1.5) dobd = -1.5;

        /* feedback state: stored post-clip, hard-clamped, denormal-flushed */
        x->x_prev = flush_denorm(uo);
        *up++ = (t_sample)(uo * (0.9 * 0.62));
        *dn++ = (t_sample)(dobd * (0.9 * 0.62));
    }
    return (w + 7);
}

static void alien_shift_dsp(t_alien_shift *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    dsp_add(alien_shift_perform, 6, x,
            sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, sp[3]->s_vec,
            (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_shift_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_shift *x = (t_alien_shift *)pd_new(alien_shift_tilde_class);
    int i;
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    x->x_ph = 0.0; x->x_sph = 0.0; x->x_freq = 110.0; x->x_prev = 0.0;
    x->x_bdelay = 0.0;
    for (i = 0; i < NHIL; i++) {
        x->x_ax1[i]=x->x_ax2[i]=x->x_ay1[i]=x->x_ay2[i]=0.0;
        x->x_bx1[i]=x->x_bx2[i]=x->x_by1[i]=x->x_by2[i]=0.0;
    }
    x->x_rng = 33377u;

    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);   /* macro */
    outlet_new(&x->x_obj, &s_signal);
    x->x_down = outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_shift_tilde_setup(void)
{
    alien_shift_tilde_class = class_new(gensym("alien_shift~"),
        (t_newmethod)alien_shift_new, 0, sizeof(t_alien_shift),
        CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_shift_tilde_class, t_alien_shift, x_f);
    class_addmethod(alien_shift_tilde_class, (t_method)alien_shift_dsp, gensym("dsp"), A_CANT, 0);
}
