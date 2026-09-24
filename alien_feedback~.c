/* alien_feedback~.c  --  a no-input feedback machine (self-oscillating)
 *
 * There is no oscillator here and no input.  A ring of delay lines is wired
 * back into itself through a lossless mixing matrix and a saturator, and left
 * to feed back on its own -- a no-input mixing desk / Larsen feedback system
 * in a box.  From the noise floor it climbs into howls, whistles, chirps and
 * screams; where it settles depends on the loop gain, and the whole instrument
 * lives at the edge of chaos where gain is near 1 and the system can neither
 * quite die nor quite explode.
 *
 * The delays are tuned from the pitch inlet, so the howl has a pitch you can
 * play; their inharmonic ratios (the "circuit") decide its character.  The
 * saturator keeps it bounded however hard it is driven, so it screams but
 * never blows the buffer.
 *
 *   inlet 0   signal/float   tuning in Hz (sets the delay lengths / howl pitch)
 *   inlet 1   signal/float   MACRO 0..100 (wrapped mod 100) -- one knob that
 *                            morphs the whole voice: a dark, barely-sustaining
 *                            harmonic hum at 0, climbing through singing tones
 *                            and inharmonic circuits, into a bright, over-driven
 *                            golden-ratio scream at 100.  It sweeps gain, damp,
 *                            chaos, drive, line count and the circuit ratios
 *                            together along a hand-tuned trajectory.
 *
 *   outlet 0  signal
 *   outlet 1  signal         energy readout (it surges and collapses)
 *
 * build:
 *   gcc -shared -fPIC -O2 -o alien_feedback~.pd_linux alien_feedback~.c -lm
 *   (macOS: -bundle -undefined dynamic_lookup -o alien_feedback~.pd_darwin)
 */

#include "m_pd.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAXN     8
#define DLEN     8192          /* per-line delay buffer (power of 2) */
#define NCIRC    5

static t_class *alien_feedback_tilde_class;

typedef struct _alien_feedback {
    t_object x_obj;
    t_float  x_f;

    double   x_sr;
    double   x_buf[MAXN][DLEN];
    int      x_wr[MAXN];
    double   x_len[MAXN];         /* current delay length in samples */
    double   x_ratio[MAXN];       /* inharmonic length ratios */
    double   x_lp[MAXN];          /* per-line damping state */
    int      x_n, x_circ;

    double   x_freq;
    double   x_energy;
    double   x_burst;             /* kick envelope */

    unsigned x_rng;
    t_outlet *x_eout;
} t_alien_feedback;

static unsigned rn(t_alien_feedback *x)
{
    x->x_rng ^= x->x_rng << 13; x->x_rng ^= x->x_rng >> 17;
    x->x_rng ^= x->x_rng << 5;  return x->x_rng;
}
static double frnd(t_alien_feedback *x){ return (double)(rn(x)&0xffffff)/(double)0x800000 - 1.0; }

static inline double softclip(double v)
{
    if (v >  3.0) return 1.0;
    if (v < -3.0) return -1.0;
    return v * (27.0 + v*v) / (27.0 + 9.0*v*v);
}

/* flush denormals to zero and clamp any non-finite / runaway state.
 * feedback loops manufacture denormals (huge CPU stalls) and, if left
 * unchecked, NaN/Inf that then propagate forever -- so we sanitise every
 * delay-line sample. */
#define DENORM_EPS 1e-20
/* NOTE: this TU is built with -ffast-math, under which isfinite() may be
 * folded to "true".  We therefore detect NaN with the self-compare identity
 * (v==v is false only for NaN) via volatile to defeat the optimiser, and
 * catch Inf with an explicit magnitude test -- both survive -ffast-math. */
static inline int is_bad(double v)
{
    volatile double t = v;
    if (t != t) return 1;                  /* NaN                             */
    if (v > 1e30 || v < -1e30) return 1;   /* Inf / runaway                   */
    return 0;
}
static inline double sanitise(double v)
{
    if (is_bad(v)) return 0.0;             /* NaN / Inf -> silence            */
    if (v < DENORM_EPS && v > -DENORM_EPS) return 0.0;  /* flush denormals    */
    if (v >  4.0) return  4.0;             /* hard state clamp (loop can't    */
    if (v < -4.0) return -4.0;             /* diverge to infinity)           */
    return v;
}

static const double CIRC[NCIRC][MAXN] = {
 {1,1.33,1.5,1.7,2.1,2.4,2.9,3.4},        /* 0 ring */
 {1,1.9,3.1,4.2,5.7,7.3,9.1,11.0},        /* 1 wide */
 {1,1.02,1.05,1.08,1.11,1.15,1.19,1.24},  /* 2 tight */
 {1,2,3,4,5,6,7,8},                        /* 3 harmonic */
 {1,1.618,2.618,4.236,6.854,11.09,17.94,29.03} /* 4 golden */
};

static inline void fb_set_circuit(t_alien_feedback *x, int c)
{
    int i;
    c = ((c % NCIRC) + NCIRC) % NCIRC;
    x->x_circ = c;
    for (i = 0; i < MAXN; i++) x->x_ratio[i] = CIRC[c][i];
}

/* ------------------------------------------------------------------ */

/* map the 0..1 macro onto the whole parameter set (curated morph).
 * gain/damp/chaos/drive climb together; line count grows; the circuit
 * steps through harmonic -> ring -> tight -> wide -> golden as it opens up. */
static inline void fb_macro(double t, double *gain, double *damp,
                            double *chaos, double *drive, int *n, int *circ)
{
    *gain  = 0.90 + 0.09 * t;                 /* barely alive -> edge (<0.99)*/
    *damp  = 0.05 + 0.90 * t;                 /* dark -> bright/screech      */
    *chaos = 0.05 + 0.90 * t * t;             /* gentle -> wildly nonlinear  */
    *drive = 0.10 + 0.75 * t;                 /* output grit                 */
    *n     = (int)(3.0 + 5.0 * t);            /* few -> many lines (3..8)    */
    if (*n > MAXN) *n = MAXN;

    /* discrete circuit: mellow harmonic at the bottom, exotic golden on top */
    if      (t < 0.20) *circ = 3;             /* harmonic */
    else if (t < 0.45) *circ = 0;             /* ring     */
    else if (t < 0.65) *circ = 2;             /* tight    */
    else if (t < 0.85) *circ = 1;             /* wide     */
    else               *circ = 4;             /* golden   */
}

static t_int *alien_feedback_perform(t_int *w)
{
    t_alien_feedback *x = (t_alien_feedback *)(w[1]);
    t_sample *fin = (t_sample *)(w[2]);
    t_sample *min = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    t_sample *eout= (t_sample *)(w[5]);
    int nsamp = (int)(w[6]);

    double sr = x->x_sr;
    int i;

    /* ---- derive DISCRETE quantities ONCE PER BLOCK from the macro's first
     * sample (line count + circuit select).  Recomputing these per-sample is
     * pure CPU waste and switching the circuit per-sample would zipper.  The
     * continuous parameters (gain/damp/chaos/drive) are still read per-sample
     * so the morph stays smooth. */
    {
        double mac0 = (nsamp > 0) ? (double)min[0] : 0.0;
        double t0, g0, d0, c0, dr0;
        int n0, circ0;
        t0 = mac0 - 100.0 * floor(mac0 * 0.01);
        if (!(t0 >= 0.0 && t0 <= 100.0)) t0 = 0.0;   /* NaN/inf guard */
        t0 *= 0.01;
        fb_macro(t0, &g0, &d0, &c0, &dr0, &n0, &circ0);
        if (n0 < 1)    n0 = 1;
        if (n0 > MAXN) n0 = MAXN;
        x->x_n = n0;
        if (circ0 != x->x_circ) fb_set_circuit(x, circ0);   /* act on change */
    }

    while (nsamp--) {
        double ftarget = *fin++, mac = *min++;
        double t, gain, mdamp, chaos, mdrive, damp, nl;
        int n = x->x_n, circ;
        double f[MAXN], sum = 0.0, o = 0.0, base;

        /* wrap the macro into [0,100) then normalise to [0,1) */
        t = mac - 100.0 * floor(mac * 0.01);
        if (!(t >= 0.0 && t <= 100.0)) t = 0.0;   /* NaN/inf guard */
        t *= 0.01;
        fb_macro(t, &gain, &mdamp, &chaos, &mdrive, &n, &circ);
        n = x->x_n;                               /* count fixed for the block */

        damp = 0.02 + 0.98 * (1.0 - mdamp);   /* 1 = no loss, small = dark */
        nl   = 1.0 + chaos * 6.0;

        if (!(ftarget > 0.0))  ftarget = 5.0;    /* NaN / <=0 guard           */
        if (ftarget < 5.0)     ftarget = 5.0;
        if (ftarget > 4000.0)  ftarget = 4000.0;
        x->x_freq += 0.001 * (ftarget - x->x_freq);      /* smooth tuning */
        if (!(x->x_freq > 0.01)) x->x_freq = 0.01;       /* no divide by ~0   */

        /* loop gain MUST stay strictly below unity or the lossless mix
         * diverges to infinity.  0.99 hard ceiling. */
        if (gain < 0.0)  gain = 0.0;
        if (gain > 0.99) gain = 0.99;

        base = sr / x->x_freq;

        /* read + damp each delay line */
        for (i = 0; i < n; i++) {
            double L = base * x->x_ratio[i];
            int d;
            double rp, a, s0, s1, y;
            if (L < 2.0) L = 2.0;
            if (L > DLEN - 2) L = DLEN - 2;      /* clamp delay to buffer     */
            x->x_len[i] = L;
            rp = (double)x->x_wr[i] - L;
            while (rp < 0.0) rp += DLEN;
            d = (int)rp; a = rp - d;
            s0 = x->x_buf[i][d & (DLEN-1)];
            s1 = x->x_buf[i][(d+1) & (DLEN-1)];
            y = s0 + a * (s1 - s0);
            x->x_lp[i] += damp * (y - x->x_lp[i]);        /* HF loss */
            x->x_lp[i] = sanitise(x->x_lp[i]);            /* flush/clamp state */
            f[i] = x->x_lp[i];
            sum += f[i];
            o += f[i];
        }

        /* Householder lossless mix, scaled by loop gain, then saturate */
        for (i = 0; i < n; i++) {
            double v = gain * (f[i] - (2.0 / n) * sum);
            v = tanh(nl * v);                              /* saturate near +/-1 */
            v += 1.5e-5 * frnd(x) + x->x_burst * frnd(x);  /* seed / kick */
            v = sanitise(v);                               /* no NaN into buffer */
            x->x_buf[i][x->x_wr[i]] = v;
            x->x_wr[i] = (x->x_wr[i] + 1) & (DLEN - 1);
        }
        x->x_burst *= 0.9995;

        o /= (double)n;
        o = softclip(o * (1.0 + mdrive * 4.0) * 1.3);
        if (is_bad(o)) o = 0.0;
        if (o >  1.5) o =  1.5;                            /* final safety net  */
        if (o < -1.5) o = -1.5;
        *out++ = (t_sample)(o * 0.696);

        x->x_energy += 0.0005 * (fabs(o) - x->x_energy);
        if (is_bad(x->x_energy)) x->x_energy = 0.0;
        *eout++ = (t_sample)x->x_energy;
    }
    return (w + 7);
}

static void alien_feedback_dsp(t_alien_feedback *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    dsp_add(alien_feedback_perform, 6, x,
            sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, sp[3]->s_vec,
            (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_feedback_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_feedback *x = (t_alien_feedback *)pd_new(alien_feedback_tilde_class);
    int i, j;
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    for (i = 0; i < MAXN; i++) {
        for (j = 0; j < DLEN; j++) x->x_buf[i][j] = 0.0;
        x->x_wr[i] = 0; x->x_lp[i] = 0.0; x->x_len[i] = 100.0;
    }
    x->x_n = 6; x->x_freq = 120.0; x->x_energy = 0.0; x->x_burst = 0.3;
    x->x_rng = 99137u;
    x->x_circ = -1;
    fb_set_circuit(x, 3);

    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);   /* macro */
    outlet_new(&x->x_obj, &s_signal);
    x->x_eout = outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_feedback_tilde_setup(void)
{
    alien_feedback_tilde_class = class_new(gensym("alien_feedback~"),
        (t_newmethod)alien_feedback_new, 0, sizeof(t_alien_feedback),
        CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_feedback_tilde_class, t_alien_feedback, x_f);
    class_addmethod(alien_feedback_tilde_class, (t_method)alien_feedback_dsp,
                    gensym("dsp"), A_CANT, 0);
}
