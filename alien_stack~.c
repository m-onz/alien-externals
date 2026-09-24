/* alien_stack~.c  --  additive synthesis whose partials come from a pattern
 *
 * Also answers to [alien_partials~], since you already have a control
 * object called alien_stack.
 *
 * Ordinary additive synthesis uses the harmonic series: 1, 2, 3, 4...
 * This one builds a stack of partials and then transforms the whole set at
 * once with the classic additive performance controls -- stretch (raise
 * every ratio to a power, spreading the partials into a bell), tilt
 * (spectral slope), shimmer (slow independent per-partial detune so it
 * doesn't sound dead), odd (fade the even partials) and drive.  Partials
 * above Nyquist are dropped rather than folded, so it never aliases however
 * far stretch is pushed.
 *
 *   inlet 0   signal/float   fundamental in Hz
 *   inlet 1   signal/float   MACRO 0..100 (wrapped mod 100) -- one knob that
 *                            morphs the whole voice: a soft, small harmonic
 *                            organ at 0, opening through more partials and a
 *                            brighter tilt, into a wide inharmonic bell that
 *                            shimmers, thins to odd partials and drives at
 *                            100.  It sweeps partial count / stretch / tilt /
 *                            shimmer / odd / drive together along a
 *                            hand-tuned trajectory.
 *
 *   outlet 0  signal
 *
 * build:
 *   gcc -shared -fPIC -O2 -o alien_stack~.pd_linux alien_stack~.c -lm
 */

#include "m_pd.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAXP 48

/* the macro can request at most this many partials; MUST be <= MAXP so the
 * additive loop can never index past the phase / ratio arrays */
#define MAXN 32

static t_class *alien_stack_tilde_class;

typedef struct _alien_stack {
    t_object x_obj;
    t_float  x_f;

    double   x_sr;
    double   x_ph[MAXP];
    double   x_shph[MAXP];      /* shimmer LFO phases */
    double   x_ratio[MAXP];
    int      x_lastmode;        /* which ratio-set is currently loaded */

    /* per-block cache: derived ONCE per block from the first sample's macro
     * (and f0), not rebuilt per sample. */
    int      x_n;               /* active partial count, clamped [1,MAXN]   */
    double   x_amp[MAXP];       /* per-partial amplitude (tilt/odd applied)  */
    double   x_finc[MAXP];      /* per-partial phase increment fr/sr         */
    double   x_active[MAXP];    /* 1.0 if below Nyquist, else 0.0 (band-lim) */
    double   x_shinc[MAXP];     /* per-partial shimmer LFO increment         */
    double   x_shdepth;         /* shimmer depth (0 = off)                   */
    double   x_norm;            /* 1/sum(active amps) so peak stays ~unity   */
    double   x_drive;           /* cached drive amount                       */
    double   x_lastmac;         /* last macro value, to detect change        */
    double   x_lastf0;          /* last f0, to detect change                 */
    int      x_haveblock;       /* 0 until the first block has been built    */
} t_alien_stack;

static inline double softclip(double v)
{
    if (v >  3.0) return 1.0;
    if (v < -3.0) return -1.0;
    return v * (27.0 + v*v) / (27.0 + 9.0*v*v);
}

/* ------------------------------------------------------------------ */

/* the ratio sets the macro morphs between, in order of increasing
 * inharmonicity: a harmonic organ, then stacked fifths, then a bell. */
#define NMODE 3
static const double MODE_RATIO[NMODE][MAXP] = {
    /* 0 harmonic: 1 2 3 4 ... filled below at load time */
    {0},
    /* 1 fifths */
    {1,1.5,2.25,3.375,5.0625,7.594,11.39,17.09,25.63,38.44,57.67,86.5},
    /* 2 bell */
    {1,2.00,3.01,4.15,5.43,6.79,8.21,9.33,10.9,12.4,14.1,15.9}
};

/* load the ratio set for a given discrete mode into x_ratio */
static void load_mode(t_alien_stack *x, int mode)
{
    int i;
    if (mode == 0) {
        for (i = 0; i < MAXP; i++) x->x_ratio[i] = i + 1;
    } else {
        for (i = 0; i < MAXP; i++) x->x_ratio[i] = MODE_RATIO[mode][i];
    }
    x->x_lastmode = mode;
}

/* map the 0..1 macro onto the whole parameter set (curated morph) */
static inline void stack_macro(double t, int *n, double *stretch,
                               double *tilt, double *shimmer,
                               double *shimrate, double *odd,
                               double *drive, int *mode)
{
    /* discrete ratio set: harmonic for the lower third, fifths in the
     * middle, bell up top -- increasing inharmonicity */
    *mode = (t < 0.34) ? 0 : (t < 0.67 ? 1 : 2);

    *n        = (int)(4.0 + 28.0 * t);            /* few -> many partials      */
    if (*n < 1)    *n = 1;
    if (*n > MAXN) *n = MAXN;                      /* hard cap: never past MAXP */
    *stretch  = 1.0 + 0.9 * t * t;                /* neutral -> spread bell    */
    *tilt     = -1.2 + 3.2 * t;                   /* dark rolloff -> thin buzz */
    *shimmer  = (t < 0.4) ? 0.0 : (t - 0.4) / 0.6;/* dead -> shimmering        */
    *shimrate = 0.3 + 1.7 * t;                    /* slow -> quicker detune    */
    *odd      = (t < 0.75) ? 0.0 : (t - 0.75) / 0.25; /* full -> odd only      */
    *drive    = (t < 0.7) ? 0.0 : (t - 0.7) / 0.3;    /* grit only up top      */
}

/* Rebuild the per-partial tables from a single macro value + f0.  Called
 * once per block (and only when the macro / f0 actually change), NOT per
 * sample -- this is what keeps the CPU sane: all pow()/ratio/tilt work
 * happens here, the per-sample loop is just phase + sin + sum. */
static void build_block(t_alien_stack *x, double mac, double f0)
{
    double sr = x->x_sr, nyq = sr * 0.48;
    double t, stretch, tilt, shimmer, shimrate, odd, drive;
    double sum = 0.0;
    int n, mode, i;

    /* wrap the macro into [0,100) then normalise to [0,1) */
    if (!(mac == mac)) mac = 0.0;                 /* NaN guard (mac!=mac)     */
    t = mac - 100.0 * floor(mac * 0.01);
    if (!(t >= 0.0 && t <= 100.0)) t = 0.0;       /* NaN/inf guard            */
    t *= 0.01;

    stack_macro(t, &n, &stretch, &tilt, &shimmer, &shimrate,
                &odd, &drive, &mode);

    /* discrete ratio-set select, per block */
    if (mode != x->x_lastmode) load_mode(x, mode);

    /* hard clamp: guarantees the additive loop never reads past MAXP */
    if (n < 1)    n = 1;
    if (n > MAXN) n = MAXN;

    /* clamp fundamental: no divide oddities, keep partials in audible range */
    if (!(f0 == f0)) f0 = 0.01;                   /* NaN guard                */
    if (f0 < 0.01)   f0 = 0.01;
    if (f0 > 8000.0) f0 = 8000.0;

    x->x_n       = n;
    x->x_drive   = drive;
    x->x_shdepth = (shimmer > 0.0) ? shimmer * 0.012 : 0.0;

    for (i = 0; i < n; i++) {
        double r = x->x_ratio[i];
        double fr, a;

        if (r <= 0.0) {                            /* undefined ratio slot     */
            x->x_active[i] = 0.0;
            x->x_amp[i]    = 0.0;
            x->x_finc[i]   = 0.0;
            x->x_shinc[i]  = 0.0;
            continue;
        }

        /* stretch: ratio^s.  At s = 1 nothing changes. */
        fr = f0 * pow(r, stretch);

        /* drop rather than fold: never aliases, however far stretch pushes */
        if (fr > nyq || fr < 1.0) {
            x->x_active[i] = 0.0;
            x->x_amp[i]    = 0.0;
            x->x_finc[i]   = 0.0;
            x->x_shinc[i]  = 0.0;
            continue;
        }

        a = 1.0 / (double)(i + 1);
        if (tilt != 0.0) a *= pow(r, -tilt);
        if (odd > 0.0 && (i & 1)) a *= 1.0 - odd;
        if (!(a == a) || a < 0.0) a = 0.0;         /* NaN/neg guard            */

        x->x_active[i] = 1.0;
        x->x_amp[i]    = a;
        x->x_finc[i]   = fr / sr;
        x->x_shinc[i]  = (x->x_shdepth > 0.0)
                       ? shimrate * (0.6 + 0.4 * (double)i / n) / sr
                       : 0.0;
        sum += a;
    }

    /* normalise so the summed partials peak near unity, not full-scale */
    x->x_norm = (sum > 1e-6) ? (1.0 / sum) : 1.0;

    x->x_lastmac = mac;
    x->x_lastf0  = f0;
    x->x_haveblock = 1;
}

static t_int *alien_stack_perform(t_int *w)
{
    t_alien_stack *x = (t_alien_stack *)(w[1]);
    t_sample *fin = (t_sample *)(w[2]);
    t_sample *min = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    int nsamp = (int)(w[5]);

    double mac0 = min[0], f00 = fin[0];
    double norm, shdepth, drive;
    int n, i;

    /* build the per-block tables from the first sample of this block, but
     * only when something actually changed (or on the very first block) */
    if (!x->x_haveblock || mac0 != x->x_lastmac || f00 != x->x_lastf0)
        build_block(x, mac0, f00);

    n       = x->x_n;
    norm    = x->x_norm;
    shdepth = x->x_shdepth;
    drive   = x->x_drive;

    while (nsamp--) {
        double y = 0.0;
        fin++;                                     /* f0 read per block only   */
        min++;                                     /* macro read per block only*/

        for (i = 0; i < n; i++) {
            double det = 1.0;

            if (x->x_active[i] == 0.0) continue;

            if (shdepth > 0.0) {
                x->x_shph[i] += x->x_shinc[i];
                if (x->x_shph[i] >= 1.0) x->x_shph[i] -= 1.0;
                det = 1.0 + shdepth * sin(x->x_shph[i] * 2.0 * M_PI);
            }

            x->x_ph[i] += x->x_finc[i] * det;
            if (x->x_ph[i] >= 1.0) x->x_ph[i] -= 1.0;
            else if (x->x_ph[i] < 0.0) x->x_ph[i] += 1.0;

            y += x->x_amp[i] * sin(x->x_ph[i] * 2.0 * M_PI);
        }

        y *= norm;
        if (drive > 0.0)
            y = softclip(y * (1.0 + drive * 8.0)) / (1.0 + drive * 2.0);

        /* keep the output finite and bounded no matter what */
        if (!(y > -1.5 && y < 1.5)) {              /* catches NaN/Inf too      */
            y = softclip(y);
            if (!(y > -1.5 && y < 1.5)) y = 0.0;
        }

        *out++ = (t_sample)(y * (0.9 * 0.74));
    }
    return (w + 6);
}

static void alien_stack_dsp(t_alien_stack *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    dsp_add(alien_stack_perform, 5, x,
            sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_stack_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_stack *x = (t_alien_stack *)pd_new(alien_stack_tilde_class);
    int i;
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    for (i = 0; i < MAXP; i++) {
        x->x_ph[i] = (double)i * 0.0137;
        x->x_shph[i] = (double)i / (double)MAXP;
    }
    x->x_lastmode = -1;
    load_mode(x, 0);
    x->x_haveblock = 0;
    x->x_lastmac = -1e30;
    x->x_lastf0  = -1e30;
    x->x_n = 0;
    x->x_norm = 1.0;
    x->x_shdepth = 0.0;
    x->x_drive = 0.0;
    for (i = 0; i < MAXP; i++) {
        x->x_amp[i] = 0.0; x->x_finc[i] = 0.0;
        x->x_active[i] = 0.0; x->x_shinc[i] = 0.0;
    }

    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_stack_tilde_setup(void)
{
    alien_stack_tilde_class = class_new(gensym("alien_stack~"),
        (t_newmethod)alien_stack_new, 0, sizeof(t_alien_stack),
        CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_stack_tilde_class, t_alien_stack, x_f);
    class_addmethod(alien_stack_tilde_class, (t_method)alien_stack_dsp,
                    gensym("dsp"), A_CANT, 0);
}
