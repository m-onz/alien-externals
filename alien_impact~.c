/* alien_impact~.c  --  modal percussion
 *
 * A bank of tuned resonators struck by a short exciter.  This is how
 * real struck objects work: the strike puts broadband energy in, and
 * the object's modes decide what survives and for how long.  Get the
 * mode RATIOS right and it sounds like the material; get the decay
 * RATES right and it sounds like the size.
 *
 * There is no bang: the bank is re-struck automatically once per cycle
 * of the fundamental, so the pitch inlet plays it like a tone whose
 * timbre is the material.  Feed it a slow f0 for a sparse struck
 * pattern, an audio-rate f0 for a droning resonant buzz.
 *
 *   inlet 0   signal/float   fundamental in Hz (also the re-strike rate)
 *   inlet 1   signal/float   MACRO 0..100 (wrapped mod 100) -- one knob that
 *                            morphs the whole voice: a soft mallet on a
 *                            dark, short, damped wooden body at 0, sweeping
 *                            through skin and glass, into a hard bright
 *                            stick ringing a long inharmonic metal bell,
 *                            driven and detuned, at 100.  It sweeps
 *                            material/decay/tilt/position/hardness/spread/
 *                            drive together along a hand-tuned trajectory.
 *
 *   outlet 0  signal
 *
 * build:
 *   gcc -shared -fPIC -O2 -o alien_impact~.pd_linux alien_impact~.c -lm
 */

#include "m_pd.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAXMODE 24

static t_class *alien_impact_tilde_class;

typedef struct _alien_impact {
    t_object x_obj;
    t_float  x_f;

    double   x_y1[MAXMODE], x_y2[MAXMODE];
    double   x_ratio[MAXMODE], x_gain[MAXMODE];
    int      x_n;
    int      x_mat;             /* current material index (discrete morph) */

    /* per-block coefficient cache -- recomputed once per block, on change,
     * NOT per sample.  keyed on the block's first macro/f0 value. */
    double   x_a1[MAXMODE], x_a2[MAXMODE], x_sw[MAXMODE];
    int      x_active[MAXMODE];  /* 1 if mode below Nyquist this block */
    double   x_c_t;              /* macro value the cache was built for */
    double   x_c_f0;             /* f0 the cache was built for           */
    int      x_c_valid;
    double   x_ynorm;            /* 1/sqrt(active modes)                 */
    double   x_drive;            /* cached drive for this block          */
    double   x_bcoef, x_bbright; /* cached exciter envelope coeffs       */

    double   x_sr;
    double   x_burst;           /* exciter envelope state */
    double   x_bstate;          /* exciter one-pole */
    double   x_phase;           /* fundamental phase for auto re-strike */
    double   x_restrike_hold;   /* samples until next restrike allowed  */
    unsigned x_rng;
} t_alien_impact;

/* mode ratios.  bell and plate are inharmonic on purpose -- that is
 * what makes metal sound like metal rather than like a filtered saw.
 * skin uses the Bessel zeros of a circular membrane. */
static const double MAT_BELL[]   = {1,2.00,3.01,4.15,5.43,6.79,8.21,9.33,10.75,12.4};
static const double MAT_GLASS[]  = {1,2.32,4.25,6.63,9.38,12.4,15.8,19.5};
static const double MAT_WOOD[]   = {1,1.58,2.63,3.72,5.11,6.80,8.42};
static const double MAT_SKIN[]   = {1,1.593,2.135,2.295,2.917,3.500,3.599,4.230};
static const double MAT_BAR[]    = {1,2.756,5.404,8.933,13.34,18.64};
static const double MAT_PLATE[]  = {1,1.41,1.73,2.00,2.24,2.45,2.65,2.83,3.00,3.16,3.32,3.46};
static const double MAT_STRING[] = {1,2,3,4,5,6,7,8,9,10,11,12};

#define NMAT 7

/* load one material's ratio set into the bank.  decay/tilt come from
 * the macro, not the material, so the sweep controls ring length. */
static void imp_load_material(t_alien_impact *x, int idx)
{
    const double *m; int n, i;
    idx = (idx % NMAT + NMAT) % NMAT;
    switch (idx) {
        case 0: m = MAT_WOOD;   n = 7;  break;
        case 1: m = MAT_SKIN;   n = 8;  break;
        case 2: m = MAT_STRING; n = 12; break;
        case 3: m = MAT_GLASS;  n = 8;  break;
        case 4: m = MAT_PLATE;  n = 12; break;
        case 5: m = MAT_BAR;    n = 6;  break;
        default: m = MAT_BELL;  n = 10; break;
    }
    x->x_n = n;
    for (i = 0; i < n; i++) x->x_ratio[i] = m[i];
    x->x_mat = idx;
}

static inline double softclip(double v)
{
    if (v >  3.0) return 1.0;
    if (v < -3.0) return -1.0;
    return v * (27.0 + v*v) / (27.0 + 9.0*v*v);
}

/* ------------------------------------------------------------------ */

/* map the 0..1 macro onto the whole parameter set (curated morph).
 * material walks a continuum from soft/dark/short to hard/bright/long:
 *   wood -> skin -> string -> glass -> plate -> bar -> bell
 * decay grows, tilt eases so highs sustain, the strike hardens and
 * moves toward an edge, and drive/detune arrive only up top. */
static inline void imp_macro(double t, int *mat, double *decay, double *tilt,
                             double *pos, double *hard, double *spread,
                             double *drive)
{
    *mat    = (int)(t * NMAT);                 /* 7 materials across the sweep */
    if (*mat >= NMAT) *mat = NMAT - 1;
    *decay  = 120.0 * pow(20.0, t);            /* 120ms damped -> ~2.4s ring   */
    *tilt   = 1.5 - 0.8 * t;                   /* highs die fast -> highs ring  */
    *pos    = 0.5 - 0.35 * t;                  /* centred/round -> near an edge */
    *hard   = 0.1 + 0.85 * t;                  /* soft mallet -> hard stick     */
    *spread = 1.0 + 0.6 * t * t;               /* in tune -> stretched/alien    */
    *drive  = (t < 0.6) ? 0.0 : (t - 0.6) / 0.4;   /* grit only up top          */
}

/* small helper: sanitise one macro sample into t in [0,1). */
static inline double imp_wrap_macro(double mac)
{
    double t;
    if (!isfinite(mac)) return 0.0;
    t = mac - 100.0 * floor(mac * 0.01);       /* wrap into [0,100) */
    if (!(t >= 0.0 && t <= 100.0)) t = 0.0;
    return t * 0.01;
}

static inline double imp_clamp_f0(double f0)
{
    if (!isfinite(f0)) return 1.0;
    if (f0 < 0.01)    f0 = 0.01;               /* never divide by ~0    */
    if (f0 < 1.0)     f0 = 1.0;                /* keep re-strike sane    */
    if (f0 > 6000.0)  f0 = 6000.0;
    return f0;
}

/* Rebuild the whole per-block coefficient cache from ONE macro/f0 value.
 * All the transcendentals (pow/exp/sin/cos) and the material select live
 * here, so the per-sample loop is a handful of multiply-adds. */
static void imp_rebuild(t_alien_impact *x, double t, double f0)
{
    double decay, tilt, pos, hard, spread, drive, blen;
    double sr = x->x_sr;
    int mat, n, i, nactive = 0;

    imp_macro(t, &mat, &decay, &tilt, &pos, &hard, &spread, &drive);

    /* discrete material: reload ratios only when it changes */
    if (mat != x->x_mat) imp_load_material(x, mat);
    n = x->x_n;
    if (n < 1) n = 1;
    if (n > MAXMODE) n = MAXMODE;               /* hard capacity clamp   */

    /* strike position gains (physical rule sin(pi*i*pos)) */
    for (i = 0; i < n; i++) {
        double g = sin(M_PI * (double)(i + 1) * pos);
        x->x_gain[i] = fabs(g) * 0.6 + 0.4;
    }

    for (i = 0; i < n; i++) {
        double fm, r, dec, wn;

        fm = f0 * pow(x->x_ratio[i], 1.0 + (spread - 1.0) * 0.5);
        if (!(fm > 0.0) || fm > sr * 0.47) {    /* above Nyquist / bad    */
            x->x_active[i] = 0;
            x->x_a1[i] = x->x_a2[i] = x->x_sw[i] = 0.0;
            x->x_y1[i] = x->x_y2[i] = 0.0;      /* silence dead mode      */
            continue;
        }

        /* higher modes decay faster, as they do in real objects */
        dec = decay * 0.001 / pow(x->x_ratio[i], tilt);
        if (dec < 0.0005) dec = 0.0005;
        r = exp(-1.0 / (dec * sr));
        if (r >= 0.99995) r = 0.99995;          /* strictly stable (<1)   */
        if (!(r > 0.0))   r = 0.0;

        wn = 2.0 * M_PI * fm / sr;
        x->x_sw[i] = sin(wn);
        x->x_a1[i] = 2.0 * r * cos(wn);
        x->x_a2[i] = -r * r;
        x->x_active[i] = 1;
        nactive++;
    }

    x->x_n = n;
    x->x_ynorm = 1.0 / sqrt((double)(nactive > 0 ? nactive : 1));
    x->x_drive = drive;

    /* exciter envelope: hard strike = short burst = more high modes */
    blen  = 0.0004 + (1.0 - hard) * 0.010;
    x->x_bcoef  = exp(-1.0 / (blen * sr));
    x->x_bbright = 0.15 + hard * 0.8;

    x->x_c_t   = t;
    x->x_c_f0  = f0;
    x->x_c_valid = 1;
}

static t_int *alien_impact_perform(t_int *w)
{
    t_alien_impact *x = (t_alien_impact *)(w[1]);
    t_sample *fin = (t_sample *)(w[2]);
    t_sample *min = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    int nsamp = (int)(w[5]);

    double sr = x->x_sr;
    double f0, t, phinc, drive, ynorm, bcoef, bbright;
    /* minimum samples between re-strikes: cap restrike rate so a high f0
     * does not pile a fresh burst on every few samples (which stutters and
     * accumulates energy).  ~130 Hz ceiling -> above that it's a sustained
     * drone from a single decaying burst, not a machine-gun of impacts. */
    double min_hold = sr / 130.0;
    int n = x->x_n, i;

    /* --- per-BLOCK work: read macro/f0 from the FIRST sample only, and
     * rebuild coefficients only when they actually changed. --- */
    t  = imp_wrap_macro((double)min[0]);
    f0 = imp_clamp_f0((double)fin[0]);
    if (!x->x_c_valid ||
        fabs(t - x->x_c_t) > 1e-9 || fabs(f0 - x->x_c_f0) > 1e-9) {
        imp_rebuild(x, t, f0);
    }
    n       = x->x_n;
    drive   = x->x_drive;
    ynorm   = x->x_ynorm;
    bcoef   = x->x_bcoef;
    bbright = x->x_bbright;
    phinc   = f0 / sr;
    (void)fin; (void)min;  /* macro/f0 consumed once per block, above */

    while (nsamp--) {
        double exc, y = 0.0;

        /* re-strike once per fundamental cycle, rate-limited */
        x->x_phase += phinc;
        if (x->x_restrike_hold > 0.0) x->x_restrike_hold -= 1.0;
        if (x->x_phase >= 1.0) {
            x->x_phase -= 1.0;
            if (x->x_phase >= 1.0) x->x_phase = 0.0;  /* audio-rate f0 guard */
            if (x->x_restrike_hold <= 0.0) {
                x->x_burst = 1.0;                     /* clean re-excite (set, not add) */
                x->x_restrike_hold = min_hold;
            }
        }

        /* noise burst, lowpassed by hardness */
        x->x_rng ^= x->x_rng << 13; x->x_rng ^= x->x_rng >> 17; x->x_rng ^= x->x_rng << 5;
        exc = ((double)(x->x_rng & 0xffffff) / (double)0x800000 - 1.0) * x->x_burst;
        x->x_burst *= bcoef;
        x->x_bstate += bbright * (exc - x->x_bstate);
        exc = x->x_bstate;

        for (i = 0; i < n; i++) {
            double v;
            if (!x->x_active[i]) continue;
            v = x->x_a1[i] * x->x_y1[i] + x->x_a2[i] * x->x_y2[i]
                + exc * x->x_gain[i];
            /* clamp mode state so a stray transient can never run to inf,
             * and flush denormals (tiny tails burn CPU on x86) */
            if (!isfinite(v)) v = 0.0;
            else if (v >  1e4) v =  1e4;
            else if (v < -1e4) v = -1e4;
            else if (v > -1e-15 && v < 1e-15) v = 0.0;
            x->x_y2[i] = x->x_y1[i];
            x->x_y1[i] = v;
            /* the impulse response peaks at 1/sin(w), so sin(w) normalises */
            y += v * x->x_sw[i];
        }

        y *= ynorm;
        if (drive > 0.0)
            y = softclip(y * (1.0 + drive * 20.0)) / (1.0 + drive * 3.0);
        y *= 0.08;

        /* final safety: finite + bounded output near unity */
        if (!isfinite(y)) y = 0.0;
        if (y >  1.5) y =  1.5;
        else if (y < -1.5) y = -1.5;
        *out++ = (t_sample)(y * 0.51);
    }
    return (w + 6);
}

static void alien_impact_dsp(t_alien_impact *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    if (x->x_sr < 1000) x->x_sr = 44100;
    x->x_c_valid = 0;              /* sr may have changed: force coeff rebuild */
    dsp_add(alien_impact_perform, 5, x,
            sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_impact_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_impact *x = (t_alien_impact *)pd_new(alien_impact_tilde_class);
    int i;
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    for (i = 0; i < MAXMODE; i++) { x->x_y1[i] = x->x_y2[i] = 0.0; }
    x->x_burst = 0.0; x->x_bstate = 0.0; x->x_phase = 0.0; x->x_rng = 5489u;
    x->x_restrike_hold = 0.0;
    x->x_c_valid = 0; x->x_c_t = -1.0; x->x_c_f0 = -1.0;
    x->x_ynorm = 1.0; x->x_drive = 0.0; x->x_bcoef = 0.0; x->x_bbright = 0.5;
    for (i = 0; i < MAXMODE; i++) {
        x->x_a1[i] = x->x_a2[i] = x->x_sw[i] = 0.0;
        x->x_active[i] = 0; x->x_gain[i] = 0.5;
    }
    imp_load_material(x, 0);       /* sensible default bank before dsp runs */
    x->x_mat = -1;                 /* force material reload from macro in perform */

    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_impact_tilde_setup(void)
{
    alien_impact_tilde_class = class_new(gensym("alien_impact~"),
        (t_newmethod)alien_impact_new, 0, sizeof(t_alien_impact),
        CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_impact_tilde_class, t_alien_impact, x_f);
    class_addmethod(alien_impact_tilde_class, (t_method)alien_impact_dsp,
                    gensym("dsp"), A_CANT, 0);
}
