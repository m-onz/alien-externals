/* alien_bow~.c  --  bowed inharmonic metal (resonant band-pass bank)
 *
 * A rich source (a sawtooth plus bow-noise) poured through a bank of high-Q
 * band-pass resonators tuned to INHARMONIC ratios.  Where [alien_impact~] is
 * struck and rings out, this is driven continuously and SUSTAINS -- a singing
 * bowl, a bowed vibraphone, a musical saw, glass harmonica, or metals that do
 * not exist.  A lead, not a hit.
 *
 * The resonators are inharmonic, so the tone is metallic; their Q (the ring)
 * runs from a soft breathy bow to a screaming ring.  Bow force adds friction
 * noise (air, scratch) and brightness, so it sweeps the dynamics.  A resonant
 * feedback path growls the metal when pushed.
 *
 * Each band-pass has 0 dB peak gain, so the bank is always bounded and stable
 * however high the Q.  Two extra controls push it past a physical model into
 * alien territory: structure stretches the mode ratios into imaginary alloys,
 * and cross feeds the bank's mean field back into every mode so the modes
 * trade energy -> multiphonics.
 *
 *   inlet 0   signal/float   pitch in Hz  (use [mtof] to play MIDI)
 *   inlet 1   signal/float   MACRO 0..100 (wrapped mod 100) -- one knob that
 *                            morphs the whole voice.  At 0 a soft, few-moded,
 *                            gently-bowed metallic bowl (real ratios, mellow,
 *                            long ring).  Sweeping up it steps through the
 *                            materials (bar->bowl->glass->tube->string->alien),
 *                            adds modes, force, feedback growl, inharmonic
 *                            stretch, cross-coupling multiphonics and drive,
 *                            until at 100 a screaming, cross-coupled, driven
 *                            alien-alloy lead.
 *
 *   outlet 0  signal
 *   outlet 1  signal         energy / brightness readout (visuals, mod)
 *
 * build:
 *   gcc -shared -fPIC -O2 -o alien_bow~.pd_linux alien_bow~.c -lm
 *   (macOS: -bundle -undefined dynamic_lookup -o alien_bow~.pd_darwin)
 */

#include "m_pd.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TWOPI    (2.0 * M_PI)
#define MAXMODES 24
#define NMAT     6

static t_class *alien_bow_tilde_class;

typedef struct _alien_bow {
    t_object x_obj;
    t_float  x_f;

    double   x_sr;
    double   x_ratio[MAXMODES];   /* base inharmonic ratios */
    double   x_reff[MAXMODES];    /* stretched ratios */
    double   x_g[MAXMODES];       /* mode gains (bow position) */
    double   x_q[MAXMODES];       /* per-mode Q from decay/tilt */
    double   x_b0[MAXMODES];      /* cached biquad coeffs (per block) */
    double   x_a1[MAXMODES], x_a2[MAXMODES];
    double   x_x1[MAXMODES], x_x2[MAXMODES];
    double   x_y1[MAXMODES], x_y2[MAXMODES];
    int      x_n, x_mat;

    double   x_freq, x_exph;
    double   x_prev, x_energy, x_lp, x_gsum;
    double   x_burst;             /* attack noise-burst envelope */

    /* current morph-derived parameters (recompute triggers on change) */
    double   x_decay, x_tilt, x_position, x_structure;
    double   x_last_freq;         /* freq used to build cached coeffs */

    unsigned x_rng;
    t_outlet *x_eout;
} t_alien_bow;

static unsigned rn(t_alien_bow *x)
{
    x->x_rng ^= x->x_rng << 13; x->x_rng ^= x->x_rng >> 17;
    x->x_rng ^= x->x_rng << 5;  return x->x_rng;
}
static double frnd(t_alien_bow *x){ return (double)(rn(x)&0xffffff)/(double)0x800000 - 1.0; }

static inline double softclip(double v)
{
    if (v >  3.0) return 1.0;
    if (v < -3.0) return -1.0;
    return v * (27.0 + v*v) / (27.0 + 9.0*v*v);
}

/* flush denormals and clamp a state variable to a finite, sane range */
static inline double clampstate(double v)
{
    if (!isfinite(v)) return 0.0;
    if (v >  1.0e6) return 1.0e6;
    if (v < -1.0e6) return -1.0e6;
    if (v > -1.0e-20 && v < 1.0e-20) return 0.0;   /* denormal flush */
    return v;
}

static const double MAT[NMAT][8] = {
 {1,2.756,5.404,8.933,13.34,18.64,24.82,31.87}, /* 0 bar */
 {1,2.32,4.25,6.63,9.38,12.44,15.80,19.43},     /* 1 bowl */
 {1,2.40,4.72,7.70,11.30,15.44,20.10,25.25},    /* 2 glass */
 {1,2.76,5.40,8.93,13.34,18.64,24.82,31.87},    /* 3 tube */
 {1,2.01,3.00,4.02,5.01,6.03,7.00,8.04},        /* 4 string */
 {1,1.71,3.14,4.28,6.83,8.11,10.7,13.9}         /* 5 alien */
};

/* set the base ratios for a material index */
static void bow_set_material(t_alien_bow *x, int m)
{
    int i;
    m = ((m % NMAT) + NMAT) % NMAT;
    x->x_mat = m;
    for (i = 0; i < MAXMODES; i++)
        x->x_ratio[i] = (i < 8) ? MAT[m][i] : MAT[m][7] * (double)(i - 6);
}

/* gains, Q, stretched ratios from decay/tilt/position/structure/n */
static void bow_recalc(t_alien_bow *x)
{
    int i, n = x->x_n;
    double pos = x->x_position, st = x->x_structure, gs = 0.0;
    for (i = 0; i < n; i++) {
        double dsec = 0.001 * x->x_decay * pow(x->x_ratio[i], -x->x_tilt);
        if (dsec < 0.002) dsec = 0.002;
        x->x_reff[i] = pow(x->x_ratio[i], st);
        /* Q chosen so the ring time is ~decay (Q ~ pi f T); f folded in per
         * sample, so store the pi*T part and multiply by f there */
        x->x_q[i] = M_PI * dsec;               /* multiply by fc at run time */
        x->x_g[i] = sin(M_PI * (double)(i + 1) * pos);
        gs += fabs(x->x_g[i]);
    }
    x->x_gsum = (gs > 1e-6) ? gs : 1.0;
}

/* Build the per-mode band-pass coefficients ONCE per block from the current
 * pitch.  All the trig / divides live here, out of the per-sample loop. */
static void bow_coeffs(t_alien_bow *x)
{
    int i, n = x->x_n;
    double sr = x->x_sr;
    double f0 = x->x_freq;
    if (f0 < 0.01) f0 = 0.01;                 /* forbid divide-by-zero pitch */
    for (i = 0; i < n; i++) {
        double fc = f0 * x->x_reff[i];
        double w0, cw, sw, alpha, denom;
        double Q;
        if (fc < 0.01)        fc = 0.01;
        if (fc > sr * 0.49)   fc = sr * 0.49;
        Q = x->x_q[i] * fc;                   /* Q ~ pi f T */
        if (Q < 0.5)   Q = 0.5;
        if (Q > 800.0) Q = 800.0;
        w0    = TWOPI * fc / sr;
        cw    = cos(w0); sw = sin(w0);
        alpha = sw / (2.0 * Q);
        denom = 1.0 + alpha;
        if (denom < 1.0e-9) denom = 1.0e-9;   /* clamp denominator */
        x->x_b0[i] = alpha / denom;           /* 0 dB peak BPF */
        x->x_a1[i] = -2.0 * cw / denom;
        x->x_a2[i] = (1.0 - alpha) / denom;
    }
    x->x_last_freq = x->x_freq;
}

/* ------------------------------------------------------------------ */

/* map the 0..1 macro onto the whole parameter set (curated morph).
 * Discrete mode: material index steps across NMAT sub-ranges.
 * Continuous params sweep from mellow/simple to bright/aggressive. */
static inline void bow_macro(double t,
                             int *mat, int *n,
                             double *decay, double *tilt, double *position,
                             double *bowforce, double *feedback,
                             double *structure, double *cross,
                             double *bright, double *drive)
{
    *mat = (int)(t * (double)NMAT);           /* bar..alien across the sweep */
    if (*mat >= NMAT) *mat = NMAT - 1;

    *n        = (int)(4.0 + 18.0 * t);        /* few -> many modes           */
    if (*n < 1)        *n = 1;                /* never a dead/empty bank     */
    if (*n > MAXMODES) *n = MAXMODES;         /* clamp to capacity           */
    *decay    = 2500.0 - 2100.0 * t;          /* long ring -> short/tighter  */
    *tilt     = 0.2 + 1.2 * t;                /* highs ring less as it opens */
    *position = 0.35 - 0.22 * t;              /* fuller -> brighter bow point*/
    *bowforce = 0.15 + 1.55 * t;              /* breathy -> scratchy         */
    *feedback = (t < 0.45) ? 0.0
                : (t - 0.45) / 0.55 * 0.85;   /* growl only up top           */
    if (*feedback > 0.9) *feedback = 0.9;     /* keep loop gain < 1          */
    *structure = 1.0 + (t < 0.5 ? -0.4 * (0.5 - t) * 2.0   /* 0.6..1.0 */
                                : 0.9 * (t - 0.5) * 2.0);   /* 1.0..1.9 */
    *cross    = (t < 0.55) ? 0.0
                : (t - 0.55) / 0.45 * 0.9;    /* multiphonics only up top    */
    *bright   = 0.35 + 0.6 * t;               /* darker -> open lowpass      */
    *drive    = (t < 0.6) ? 0.0
                : (t - 0.6) / 0.4;            /* grit only up top            */
}

static t_int *alien_bow_perform(t_int *w)
{
    t_alien_bow *x = (t_alien_bow *)(w[1]);
    t_sample *fin  = (t_sample *)(w[2]);
    t_sample *min  = (t_sample *)(w[3]);
    t_sample *out  = (t_sample *)(w[4]);
    t_sample *eout = (t_sample *)(w[5]);
    int nsamp = (int)(w[6]);

    double sr = x->x_sr;

    /* ---- ONCE-PER-BLOCK work, driven by the macro's FIRST sample -------- *
     * The macro is a signal but its musical meaning is block-rate: reading
     * it per sample and rebuilding pow/sin coefficient tables every sample
     * is what stutters and (via runaway feedback) crashes Pd. */
    double mac = (nsamp > 0) ? (double)min[0] : 0.0;
    double ftarget0 = (nsamp > 0) ? (double)fin[0] : x->x_freq;
    double t, bowforce, feedback, cross, bright, drive, structure, position;
    double tilt, decay, brco;
    int mat, nreq, i, n;

    /* wrap the macro into [0,100) then normalise to [0,1) */
    t = mac - 100.0 * floor(mac * 0.01);
    if (!(t >= 0.0 && t <= 100.0)) t = 0.0;       /* NaN/inf guard */
    t *= 0.01;

    bow_macro(t, &mat, &nreq, &decay, &tilt, &position,
              &bowforce, &feedback, &structure, &cross, &bright, &drive);

    /* smooth the pitch once per block (block ~1.5ms: musically immediate,
     * cheap, and lets us cache coefficients) */
    if (!(ftarget0 >= -1e9 && ftarget0 <= 1e9)) ftarget0 = x->x_freq;
    if (ftarget0 < 1.0)    ftarget0 = 1.0;
    if (ftarget0 > 5000.0) ftarget0 = 5000.0;
    x->x_freq += 0.25 * (ftarget0 - x->x_freq);
    if (x->x_freq < 0.01) x->x_freq = 0.01;       /* forbid divide-by-zero  */

    /* DISCRETE quantities: fire only on an ACTUAL integer change ---------- */
    if (mat != x->x_mat) { bow_set_material(x, mat); x->x_decay = -1.0; }
    if (nreq != x->x_n)  { x->x_n = nreq; x->x_decay = -1.0; }
    if (x->x_n < 1)        x->x_n = 1;             /* belt & braces          */
    if (x->x_n > MAXMODES) x->x_n = MAXMODES;

    /* recompute gains/Q/stretch only when a continuous morph param changed */
    if (decay != x->x_decay || tilt != x->x_tilt ||
        position != x->x_position || structure != x->x_structure) {
        x->x_decay = decay; x->x_tilt = tilt;
        x->x_position = position; x->x_structure = structure;
        bow_recalc(x);
    }

    /* rebuild the cached biquad coefficients once per block (or when the
     * gliding pitch has moved) */
    bow_coeffs(x);

    n = x->x_n;
    brco = 0.02 + 0.98 * bright;
    if (brco < 0.001) brco = 0.001;
    if (brco > 1.0)   brco = 1.0;

    {
        double airg = 0.25 + 0.6 * bowforce;      /* bow-air gain */
        double fbg  = feedback * 0.9;             /* bounded < 1  */
        double invn = 1.0 / (double)n;
        double invgsum = 1.0 / x->x_gsum;

        /* ---- CHEAP per-sample loop: cached coeffs, no trig/pow -------- */
        while (nsamp--) {
            double src, saw, o, disp = 0.0, ymean = 0.0;

            fin++;                                /* pitch consumed at block rate */
            min++;                                /* macro consumed at block rate */

            /* source: sawtooth + bow noise + attack chiff + growl feedback */
            x->x_exph += x->x_freq / sr;
            if (x->x_exph >= 1.0) x->x_exph -= 1.0;
            saw = 2.0 * x->x_exph - 1.0;
            src = 0.6 * saw
                + airg * frnd(x)                  /* bow air */
                + x->x_burst * frnd(x)            /* attack chiff */
                + fbg * tanh(2.0 * x->x_prev);    /* growl feedback (bounded) */
            x->x_burst *= 0.999;
            if (!isfinite(src)) src = 0.0;

            for (i = 0; i < n; i++) ymean += x->x_y1[i];
            ymean *= invn;

            for (i = 0; i < n; i++) {
                double y, xin;
                xin = src + cross * ymean;
                y = x->x_b0[i] * (xin - x->x_x2[i])
                    - x->x_a1[i] * x->x_y1[i]
                    - x->x_a2[i] * x->x_y2[i];
                y = clampstate(y);                /* finite + denormal flush */
                x->x_x2[i] = x->x_x1[i]; x->x_x1[i] = xin;
                x->x_y2[i] = x->x_y1[i]; x->x_y1[i] = y;
                disp += y * x->x_g[i];
            }
            disp *= invgsum;

            x->x_lp += brco * (disp - x->x_lp);
            x->x_lp = clampstate(x->x_lp);
            o = x->x_lp * 5.0;
            if (drive > 0.0)
                o = softclip(o * (1.0 + drive * 6.0)) / (1.0 + drive * 1.5);
            else
                o = softclip(o);
            if (!isfinite(o)) o = 0.0;
            if (o >  1.5) o =  1.5;               /* final soft bound */
            if (o < -1.5) o = -1.5;
            x->x_prev = o;
            *out++ = (t_sample)(o * 1.016);

            x->x_energy += 0.001 * (fabs(o) - x->x_energy);
            if (!isfinite(x->x_energy)) x->x_energy = 0.0;
            *eout++ = (t_sample)x->x_energy;
        }
    }
    return (w + 7);
}

static void alien_bow_dsp(t_alien_bow *x, t_signal **sp)
{
    if (sp[0]->s_sr >= 1000 && x->x_sr != sp[0]->s_sr) {
        x->x_sr = sp[0]->s_sr; bow_recalc(x); bow_coeffs(x);
    }
    dsp_add(alien_bow_perform, 6, x,
            sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, sp[3]->s_vec,
            (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_bow_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_bow *x = (t_alien_bow *)pd_new(alien_bow_tilde_class);
    int i;
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    for (i = 0; i < MAXMODES; i++) { x->x_x1[i]=x->x_x2[i]=x->x_y1[i]=x->x_y2[i]=0.0; }
    x->x_n = 8; x->x_freq = 110.0; x->x_exph = 0.0;
    x->x_prev = 0.0; x->x_energy = 0.0; x->x_lp = 0.0; x->x_burst = 0.0;
    x->x_decay = 900.0; x->x_tilt = 0.6; x->x_position = 0.27;
    x->x_structure = 1.0;
    x->x_last_freq = 110.0;
    x->x_rng = 8123u;

    bow_set_material(x, 5);          /* default: alien metal */
    bow_recalc(x);
    bow_coeffs(x);

    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);   /* macro */
    outlet_new(&x->x_obj, &s_signal);
    x->x_eout = outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_bow_tilde_setup(void)
{
    alien_bow_tilde_class = class_new(gensym("alien_bow~"),
        (t_newmethod)alien_bow_new, 0, sizeof(t_alien_bow),
        CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_bow_tilde_class, t_alien_bow, x_f);
    class_addmethod(alien_bow_tilde_class, (t_method)alien_bow_dsp, gensym("dsp"), A_CANT, 0);
}
