/* alien_vector~.c  --  vector / group-additive wavestacking synthesis
 *
 * A Prophet-VS style vector synth.  Four source spectra sit at the corners
 * of a square:
 *
 *      A (top-left)      warm sine organ      -- soft, few low harmonics
 *      B (top-right)     bright saw / brass   -- full harmonic series, tilted
 *      C (bottom-right)  hollow square        -- odd harmonics only
 *      D (bottom-left)   inharmonic bell/metal-- stretched, clangorous ratios
 *
 * A vector position (x,y) in the unit square bilinearly crossfades the four
 * sources into one evolving hybrid timbre.  Rather than sum hundreds of
 * independent partials, this is GROUP ADDITIVE over a FIXED UNION of partials:
 * every distinct ratio used by any source gets one permanent partial (with one
 * permanent phase accumulator), and each block only its blended AMPLITUDE
 * changes.  Because phases are never reassigned and the per-partial gains are
 * smoothed per-sample toward their target, a fast-moving macro (a wobble LFO)
 * morphs the spectrum continuously WITHOUT the clicks you'd get from partials
 * snapping in/out or phases being remapped between blocks.
 *
 * The MACRO drives the vector position as an angle+radius spiral around the
 * square, so 0..100 continuously morphs A -> B -> C -> D -> A.  A slow
 * al_wander wobble nudges (x,y) so the timbre breathes and never sits still.
 *
 *   inlet 0   signal/float   fundamental in Hz
 *   inlet 1   signal/float   MACRO 0..100 (wrapped) -- traverses the vector
 *                            square, morphing warm organ -> bright brass ->
 *                            hollow square -> inharmonic bell and back.
 *
 *   outlet 0  signal
 *
 * build:
 *   gcc -shared -fPIC -O2 -o alien_vector~.pd_linux alien_vector~.c -lm
 *   (macOS: -bundle -undefined dynamic_lookup -o alien_vector~.pd_darwin)
 */

#include "m_pd.h"
#include "alien.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* capacity for the union of all distinct partials across the four sources */
#define NPART 48

static t_class *alien_vector_tilde_class;

typedef struct _alien_vector {
    t_object x_obj;
    t_float  x_f;

    double   x_sr;

    /* ---- fixed union of partials (built once at construction) ---- */
    int      x_nu;                 /* number of union partials              */
    double   x_uratio[NPART];      /* each partial's ratio to f0            */
    double   x_usrc[NPART][4];     /* its baked amplitude in sources A/B/C/D */

    /* ---- persistent per-partial state ---- */
    double   x_ph[NPART];          /* phase 0..1 (NEVER reassigned)         */
    double   x_gain[NPART];        /* smoothed gain actually summed         */

    /* ---- per-block targets ---- */
    double   x_tgain[NPART];       /* target (normalised) gain              */
    double   x_inc[NPART];         /* phase increment fr/sr                 */
    double   x_gc;                 /* per-sample gain smoothing coefficient */

    al_rng   x_rng;
    al_wander x_wan;               /* slow autonomous drift of vector pos   */
    al_dc    x_dc;
} t_alien_vector;

/* ------------------------------------------------------------------ */
/* The four source spectra as (ratio, amplitude) pairs.  Ratios are f0
 * multipliers; the bell's are inharmonic. */

static const double SRC_A_R[] = {1, 2, 3, 4};
static const double SRC_A_A[] = {1.0, 0.45, 0.18, 0.08};
#define SRC_A_N 4

static const double SRC_B_R[] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
#define SRC_B_N 16

static const double SRC_C_R[] = {1,3,5,7,9,11,13,15,17,19};
#define SRC_C_N 10

static const double SRC_D_R[] = {1, 2.76, 5.40, 8.93, 13.34, 1.68, 3.43, 6.79};
static const double SRC_D_A[] = {1.0, 0.72, 0.55, 0.38, 0.22, 0.60, 0.40, 0.28};
#define SRC_D_N 8

/* add ratio r with source-`src` amplitude a into the fixed union (build-time) */
static void uni_add(t_alien_vector *x, double r, int src, double a)
{
    int j;
    for (j = 0; j < x->x_nu; j++)
        if (fabs(x->x_uratio[j] - r) < 1e-9) break;
    if (j == x->x_nu) {
        if (x->x_nu >= NPART) return;
        x->x_uratio[j] = r;
        x->x_usrc[j][0] = x->x_usrc[j][1] = x->x_usrc[j][2] = x->x_usrc[j][3] = 0.0;
        x->x_nu++;
    }
    x->x_usrc[j][src] = a;   /* tilt already baked in */
}

static void build_union(t_alien_vector *x)
{
    int i;
    x->x_nu = 0;
    for (i = 0; i < SRC_A_N; i++)                       /* A: tilt 0        */
        uni_add(x, SRC_A_R[i], 0, SRC_A_A[i]);
    for (i = 0; i < SRC_B_N; i++)                       /* B: 1/n, tilt 0.7 */
        uni_add(x, SRC_B_R[i], 1, (1.0/(double)(i+1)) * pow(SRC_B_R[i], -0.7));
    for (i = 0; i < SRC_C_N; i++)                       /* C: 1/n, tilt 0.6 */
        uni_add(x, SRC_C_R[i], 2, (1.0/(double)(i+1)) * pow(SRC_C_R[i], -0.6));
    for (i = 0; i < SRC_D_N; i++)                       /* D: tilt 0        */
        uni_add(x, SRC_D_R[i], 3, SRC_D_A[i]);
}

/* macro (0..1) -> vector position (x,y) in the unit square, with drift */
static inline void macro_to_xy(double t, double dx, double dy,
                               double *ox, double *oy)
{
    double ang = (t * AL_2PI) - 0.75 * AL_2PI;
    double rad = 0.72 + 0.28 * cos(t * AL_2PI * 2.0);
    double cx = 0.5 + 0.5 * rad * cos(ang) + 0.16 * dx;
    double cy = 0.5 + 0.5 * rad * sin(ang) + 0.16 * dy;
    *ox = al_clamp(cx, 0.0, 1.0);
    *oy = al_clamp(cy, 0.0, 1.0);
}

/* per block: recompute each fixed partial's TARGET gain + phase increment.
 * No slot churn, no phase reassignment -- only smooth targets move. */
static void build_block(t_alien_vector *x, double mac, double f0)
{
    double sr = x->x_sr, nyq = sr * 0.45;
    double t, vx, vy, wA, wB, wC, wD, dx, dy, sum = 0.0, inv;
    int u, nu = x->x_nu;

    if (!(mac == mac)) mac = 0.0;
    t = mac - 100.0 * floor(mac * 0.01);
    if (!(t >= 0.0 && t <= 100.0)) t = 0.0;
    t *= 0.01;

    if (!(f0 == f0)) f0 = 8.0;
    if (f0 < 8.0)       f0 = 8.0;
    if (f0 > 0.45 * sr) f0 = 0.45 * sr;

    dx = al_wander_bip(&x->x_wan, 0);
    dy = al_wander_bip(&x->x_wan, 1);
    macro_to_xy(t, dx, dy, &vx, &vy);

    /* bilinear corner weights: A(0,0) B(1,0) C(1,1) D(0,1) */
    wA = (1.0 - vx) * (1.0 - vy);
    wB = vx         * (1.0 - vy);
    wC = vx         * vy;
    wD = (1.0 - vx) * vy;

    for (u = 0; u < nu; u++) {
        double r  = x->x_uratio[u];
        double fr = r * f0;
        double g  = wA*x->x_usrc[u][0] + wB*x->x_usrc[u][1]
                  + wC*x->x_usrc[u][2] + wD*x->x_usrc[u][3];
        if (!(g == g) || g < 0.0) g = 0.0;
        /* smooth Nyquist rolloff: fade the top octave out rather than a hard
         * drop, so partials near Nyquist don't click on/off */
        if (fr >= nyq) g = 0.0;
        else if (fr > nyq * 0.5) g *= (nyq - fr) / (nyq * 0.5);
        if (fr < 1.0) g = 0.0;
        x->x_inc[u]   = fr / sr;      /* phase always advances (coherent)   */
        x->x_tgain[u] = g;
        sum += g;
    }
    inv = (sum > 1e-6) ? (1.0 / sum) : 1.0;
    for (u = 0; u < nu; u++) x->x_tgain[u] *= inv;   /* normalised target   */
}

static t_int *alien_vector_perform(t_int *w)
{
    t_alien_vector *x = (t_alien_vector *)(w[1]);
    t_sample *fin = (t_sample *)(w[2]);
    t_sample *min = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    int nsamp = (int)(w[5]);

    int nu = x->x_nu, u;
    double gc = x->x_gc;

    al_denorm_off();

    /* slow autonomous drift, then refresh targets every block (cheap: no
     * pow/alloc here) so a held note still breathes and a moving macro is
     * always current -- the per-sample gain smoothing removes any step. */
    al_wander_tick(&x->x_wan, &x->x_rng, 0.6, x->x_sr);
    build_block(x, min[0], fin[0]);

    while (nsamp--) {
        double s = 0.0;
        fin++; min++;                    /* macro/f0 read per block */

        for (u = 0; u < nu; u++) {
            x->x_gain[u] += (x->x_tgain[u] - x->x_gain[u]) * gc;   /* smooth */
            x->x_ph[u]   += x->x_inc[u];
            if (x->x_ph[u] >= 1.0) x->x_ph[u] -= floor(x->x_ph[u]);
            s += x->x_gain[u] * al_sin01(x->x_ph[u]);
        }

        s = al_dc_tick(&x->x_dc, s);
        if (!(s > -1e6 && s < 1e6)) s = 0.0;
        s = al_softclip(s * 1.1);
        if (s >  1.5) s =  1.5;
        if (s < -1.5) s = -1.5;
        *out++ = (t_sample)(s * 0.68);   /* ~ -12 dBFS */
    }
    return (w + 6);
}

static void alien_vector_dsp(t_alien_vector *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    if (x->x_sr < 1000.0) x->x_sr = 44100.0;
    /* ~6 ms per-sample gain smoothing -> no zipper/clicks under a fast macro */
    x->x_gc = 1.0 - exp(-1.0 / (x->x_sr * 0.006));
    dsp_add(alien_vector_perform, 5, x,
            sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_vector_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_vector *x = (t_alien_vector *)pd_new(alien_vector_tilde_class);
    int i;
    (void)s; (void)argc; (void)argv;

    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    x->x_gc = 1.0 - exp(-1.0 / (x->x_sr * 0.006));

    al_rng_seed(&x->x_rng, 0x51ec70a5u);
    al_wander_init(&x->x_wan, &x->x_rng, 2);
    al_dc_init(&x->x_dc);

    build_union(x);
    for (i = 0; i < NPART; i++) {
        x->x_ph[i]   = (double)i * 0.0137;   /* spread start phases */
        x->x_gain[i] = 0.0;
        x->x_tgain[i] = 0.0;
        x->x_inc[i]  = 0.0;
    }

    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_vector_tilde_setup(void)
{
    alien_vector_tilde_class = class_new(gensym("alien_vector~"),
        (t_newmethod)alien_vector_new, 0, sizeof(t_alien_vector),
        CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_vector_tilde_class, t_alien_vector, x_f);
    class_addmethod(alien_vector_tilde_class, (t_method)alien_vector_dsp,
                    gensym("dsp"), A_CANT, 0);
}
