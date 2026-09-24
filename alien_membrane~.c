/* alien_membrane~.c  --  a 2D membrane, solved directly
 *
 * The wave equation on a grid, stepped once per audio sample:
 *
 *   u'[i][j] = 2u[i][j] - u''[i][j]
 *            + c^2 (u[i+1][j] + u[i-1][j] + u[i][j+1] + u[i][j-1] - 4u[i][j])
 *
 * Strike position and pickup position are both timbre controls, because
 * where you hit a drum decides which modes get energy and where you
 * listen decides which ones you hear.  Hit the centre and you get the
 * fundamental; hit the rim and you get the inharmonic ones.  That is
 * one object covering tom, steel pan, gong and sheet metal.
 *
 * The grid state comes out of the right outlet as a list on [dump(, so
 * the same simulation can be drawn in GEM.
 *
 *   inlet 0   signal/float   excitation, added at the strike point.  Send
 *                            noise bursts, clicks, or a whole other
 *                            instrument to drive the membrane.
 *   inlet 1   signal/float   MACRO 0..100 (wrapped mod 100) -- one knob that
 *                            morphs the whole membrane along a hand-tuned
 *                            trajectory: at 0 a small, tightly-damped,
 *                            centre-struck tom on a clamped head; at 100 a
 *                            large, high-tension, barely-damped free plate
 *                            struck near the rim -- a bright, inharmonic,
 *                            long-ringing gong.  It sweeps grid size,
 *                            tension, damping, HF loss, boundary and the
 *                            strike/pickup geometry together.
 *
 *   outlet 0  signal
 *   outlet 1  list     grid state on [dump(, row-major
 *
 * Cost is size^2 per audio sample: 16x16 is about 11 million cell
 * updates a second, which is fine.  32x32 is four times that.
 *
 * build:
 *   gcc -shared -fPIC -O2 -o alien_membrane~.pd_linux alien_membrane~.c -lm
 *   (macOS: -bundle -undefined dynamic_lookup -o alien_membrane~.pd_darwin)
 */

#include "m_pd.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Grid limits.  The macro maps t in [0,1] onto g = 8..32, so 32 is the
 * real ceiling; MAXG carries headroom and the arrays are sized to MAXG^2
 * so g*g can never exceed NCELL no matter what the macro produces. */
#define MAXG  32
#define NCELL (MAXG * MAXG)          /* 32*32 = 1024, fits g_max^2 exactly */

/* The build uses -ffast-math, under which isfinite()/isnan() may be folded
 * away by the optimiser.  Test the raw IEEE-754 bits instead so the guard
 * survives: exponent all-ones => Inf or NaN. */
static inline int mb_bad(float v)
{
    union { float f; unsigned int u; } p;
    p.f = v;
    return ((p.u & 0x7f800000u) == 0x7f800000u);
}

/* Flush denormals to zero (cheap, keeps the mesh from grinding the FPU). */
static inline float mb_denorm(float v)
{
    return (fabsf(v) < 1e-20f) ? 0.f : v;
}

/* Same bit test for doubles (the one-pole accumulator), fast-math safe. */
static inline int mb_bad_d(double v)
{
    union { double d; unsigned long long u; } p;
    p.d = v;
    return ((p.u & 0x7ff0000000000000ull) == 0x7ff0000000000000ull);
}

static t_class *alien_membrane_tilde_class;

typedef struct _alien_membrane {
    t_object  x_obj;
    t_float   x_f;

    float    x_u[NCELL], x_v[NCELL];    /* current, previous */
    int      x_g;
    int      x_lastg;                   /* last grid size, to reset on change */
    double   x_lp;
    double   x_dc_x1, x_dc_y1;          /* output DC-blocker (high-pass)       */
    double   x_env;                     /* running level for output AGC        */
    double   x_phase;                   /* re-strike phase (freq on inlet 0)   */
    double   x_sr;                      /* sample rate                         */
} t_alien_membrane;

#define IDX(x_, i, j) ((i) * (x_)->x_g + (j))

/* ------------------------------------------------------------------ */

/* map the 0..1 macro onto the whole parameter set (curated morph):
 * at 0 a small, damped, clamped, centre-struck tom;
 * at 100 a large, tense, ringing, free-boundary gong struck near the rim. */
static inline void mb_macro(double t, int *g, double *tension, double *damp,
                            double *loss, int *bound,
                            double *sx, double *sy, double *px, double *py)
{
    *g       = (int)(8.0 + 24.0 * t + 0.5);     /* 8x8 -> 32x32              */
    if (*g < 4)    *g = 4;
    if (*g > MAXG) *g = MAXG;                    /* g*g <= NCELL guaranteed  */
    /* Wave speed^2 must stay strictly below the 2D explicit-FD CFL limit
     * (0.5).  Cap at 0.47 so the mesh never sits exactly on the edge and
     * creeps unstable over a long ring. */
    *tension = 0.12 + 0.34 * t;                 /* slack -> tense (< CFL)    */
    if (*tension > 0.47) *tension = 0.47;
    if (*tension < 0.0)  *tension = 0.0;
    /* damp is a per-step energy-loss knob: high => short tom, low => gong.
     * It is scaled into damp_step below.  Keep it strictly positive so the
     * mesh always bleeds a little energy and can never sit lossless. */
    *damp    = 0.9 - 0.87 * t;                   /* damped -> long ringing    */
    if (*damp < 0.03) *damp = 0.03;
    *loss    = 0.75 - 0.72 * t;                  /* dull -> bright            */
    if (*loss < 0.0)  *loss = 0.0;
    if (*loss > 1.0)  *loss = 1.0;
    *bound   = (t >= 0.6);                       /* clamped head -> free plate*/

    /* strike centre (fundamental) -> near rim (inharmonic modes) */
    *sx = 0.5 - 0.35 * t;
    *sy = 0.5 - 0.35 * t;
    /* pickup drifts off-centre toward the rim for more partials */
    *px = 0.5 + 0.35 * t;
    *py = 0.35 + 0.30 * t;
}

static t_int *alien_membrane_perform(t_int *w)
{
    t_alien_membrane *x = (t_alien_membrane *)(w[1]);
    t_sample *in   = (t_sample *)(w[2]);
    t_sample *min  = (t_sample *)(w[3]);
    t_sample *out  = (t_sample *)(w[4]);
    int nsamp = (int)(w[5]);

    int g, i, j;
    int si, sj, pi, pj, bound;
    float damp_step;
    double tension, damp, loss, sx, sy, px, py;

    /* macro is control-rate here: read the first sample of the block */
    {
        double mac = *min, t;
        t = mac - 100.0 * floor(mac * 0.01);
        if (!(t >= 0.0 && t <= 100.0)) t = 0.0;   /* NaN/inf guard */
        t *= 0.01;
        mb_macro(t, &g, &tension, &damp, &loss, &bound, &sx, &sy, &px, &py);
    }

    /* grid resize resets the mesh (mode count changes).  The output-domain
     * state (low-pass, DC blocker, level envelope) is deliberately kept: if
     * the macro is swept at audio rate the grid can change every few samples,
     * and zeroing the AGC envelope there would let its gain slam to the cap
     * and spike the output.  Keeping it means the level normaliser stays
     * settled through the churn. */
    if (g != x->x_lastg) {
        memset(x->x_u, 0, sizeof(x->x_u));
        memset(x->x_v, 0, sizeof(x->x_v));
        x->x_lp = 0.0;
        x->x_lastg = g;
    }
    x->x_g = g;

    si = (int)(sy * (g - 1)); sj = (int)(sx * (g - 1));
    pi = (int)(py * (g - 1)); pj = (int)(px * (g - 1));
    /* Per-step multiplier, always in (0,1): short tom ~0.996, gong ~0.9998. */
    damp_step = 1.f - (float)damp * 0.005f;
    if (damp_step > 0.99995f) damp_step = 0.99995f;
    if (damp_step < 0.90f)    damp_step = 0.90f;

    if (si < 0) si = 0;
    if (si >= g) si = g - 1;
    if (sj < 0) sj = 0;
    if (sj >= g) sj = g - 1;
    if (pi < 0) pi = 0;
    if (pi >= g) pi = g - 1;
    if (pj < 0) pj = 0;
    if (pj >= g) pj = g - 1;

    while (nsamp--) {
        float freq = *in++;
        float c2, y;
        double inc;
        min++;

        /* Inlet 0 is a FREQUENCY (Hz): a phase accumulator re-strikes the head
         * at that rate, the way a drummer's roll drives a membrane.  A held
         * frequency therefore gives a sustained, pitched sequence of strikes;
         * between strikes the mesh rings and decays on its own.  This is what
         * keeps the head oscillating around zero instead of sitting on the
         * static deflection a constant push would build (the old DC + rail). */
        if (mb_bad(freq)) freq = 0.f;
        if (freq <  0.f)    freq = -freq;   /* negative freq -> same rate      */
        if (freq > 20000.f) freq = 20000.f; /* bound the re-strike rate        */

        c2 = (float)tension;
        if (c2 > 0.47f) c2 = 0.47f;     /* strictly below CFL (0.5) for 2D FD */
        if (c2 < 0.f)   c2 = 0.f;

        inc = (double)freq / x->x_sr;
        if (!(inc >= 0.0 && inc < 1.0)) inc = 0.0;   /* NaN/oversize guard      */
        x->x_phase += inc;
        if (x->x_phase >= 1.0) {
            x->x_phase -= floor(x->x_phase);
            /* fixed-energy strike: bipolar impulse into the strike cell so the
             * excitation carries no DC of its own. */
            x->x_u[IDX(x, si, sj)] += 0.7f;
            x->x_v[IDX(x, si, sj)] -= 0.7f;
        }

        for (i = 0; i < g; i++) {
            for (j = 0; j < g; j++) {
                int id = IDX(x, i, j);
                float up, dn, lf, rt, lap, nv;

                if (i == 0 || i == g-1 || j == 0 || j == g-1) {
                    if (!bound) { x->x_v[id] = 0.f; continue; }
                }
                up = (i > 0)     ? x->x_u[IDX(x, i-1, j)] : x->x_u[id];
                dn = (i < g-1)   ? x->x_u[IDX(x, i+1, j)] : x->x_u[id];
                lf = (j > 0)     ? x->x_u[IDX(x, i, j-1)] : x->x_u[id];
                rt = (j < g-1)   ? x->x_u[IDX(x, i, j+1)] : x->x_u[id];

                lap = up + dn + lf + rt - 4.f * x->x_u[id];
                nv  = (2.f * x->x_u[id] - x->x_v[id] + c2 * lap) * damp_step;

                if (mb_bad(nv)) nv = 0.f;       /* NaN/Inf -> silence cell */
                if (nv >  8.f) nv =  8.f;
                if (nv < -8.f) nv = -8.f;
                x->x_v[id] = mb_denorm(nv);
            }
        }
        /* v now holds the new state; swap by copying (grids are small) */
        {
            int nc = g * g;
            for (i = 0; i < nc; i++) {
                float tt = x->x_u[i];
                x->x_u[i] = x->x_v[i];
                x->x_v[i] = tt;
            }
        }

        y = x->x_u[IDX(x, pi, pj)];
        if (mb_bad(y)) y = 0.f;

        /* one-pole low-pass to take the edge off the grid's own top octave */
        x->x_lp += (1.0 - loss * 0.9) * (y - x->x_lp);
        if (mb_bad_d(x->x_lp)) x->x_lp = 0.0;       /* NaN/Inf -> reset */
        if (fabs(x->x_lp) < 1e-20) x->x_lp = 0.0;   /* denormal flush */

        /* One-pole DC blocker (high-pass): y[n] = x[n] - x[n-1] + R*y[n-1].
         * The mesh, especially a free plate, holds a static displacement
         * offset that the low-pass above passes straight through; this
         * removes it so the pickup rides around zero, not a DC pedestal. */
        {
            double dcin = x->x_lp;
            double dcout = dcin - x->x_dc_x1 + 0.995 * x->x_dc_y1;
            if (mb_bad_d(dcout)) dcout = 0.0;
            if (fabs(dcout) < 1e-20) dcout = 0.0;
            x->x_dc_x1 = dcin;
            x->x_dc_y1 = dcout;
            y = (float)dcout;
        }
        if (mb_bad(y)) y = 0.f;

        /* Output AGC.  The pickup level swings wildly across the morph -- a
         * small, hard-damped tom is quiet; a big, barely-damped gong rings
         * far above unity and would otherwise pin the soft clip at full
         * scale.  Track a slow envelope of |y| and divide by it (with a
         * floor) so the mesh always lands near a fixed nominal level instead
         * of blasting.  The envelope is slow, so real strikes and the ring
         * tail keep their natural dynamics; only the gross size/damping trend
         * is normalised out. */
        {
            double a = fabs((double)y);
            /* Both time constants are slow (a fraction of a second) so the
             * envelope follows only the gross size/damping trend, not the
             * per-strike attack.  That leaves the tom->gong morph and each
             * hit's natural dynamics intact while still pulling a very loud
             * gong back down toward the target. */
            if (a > x->x_env) x->x_env += 0.0006 * (a - x->x_env);  /* attack */
            else              x->x_env += 0.0001 * (a - x->x_env);  /* release*/
            if (mb_bad_d(x->x_env) || x->x_env < 0.0) x->x_env = 0.0;
        }
        {
            double gain = 0.286 / (x->x_env + 0.06); /* target level, floored */
            if (gain > 1.5) gain = 1.5;              /* don't amplify silence  */
            y = (float)((double)y * gain);
        }

        /* final safety: kill non-finite, then a bounded monotonic cubic
         * soft clip.  1.5x - 0.5x^3 has unity slope at 0 and maps |x|<=1
         * onto |y|<=1; outside that we rail.  Output is guaranteed within
         * +/-1 (well inside the [-1.5,1.5] budget). */
        if (mb_bad(y)) y = 0.f;
        if      (y >  1.f) y =  1.f;
        else if (y < -1.f) y = -1.f;
        else    y = 1.5f * y - 0.5f * y * y * y;
        *out++ = (t_sample)y;
    }
    return (w + 6);
}

static void alien_membrane_dsp(t_alien_membrane *x, t_signal **sp)
{
    x->x_sr = (sp[0]->s_sr > 0) ? (double)sp[0]->s_sr : 44100.0;
    dsp_add(alien_membrane_perform, 5, x,
            sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */

static void *alien_membrane_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_membrane *x = (t_alien_membrane *)pd_new(alien_membrane_tilde_class);
    (void)s; (void)argc; (void)argv;
    x->x_g = 16;
    x->x_lastg = -1;
    x->x_lp = 0.0;
    x->x_dc_x1 = 0.0;
    x->x_dc_y1 = 0.0;
    x->x_env = 0.0;
    x->x_phase = 0.0;
    x->x_sr = 44100.0;
    memset(x->x_u, 0, sizeof(x->x_u));
    memset(x->x_v, 0, sizeof(x->x_v));

    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_membrane_tilde_setup(void)
{
    alien_membrane_tilde_class = class_new(gensym("alien_membrane~"),
        (t_newmethod)alien_membrane_new, 0, sizeof(t_alien_membrane),
        CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_membrane_tilde_class, t_alien_membrane, x_f);
    class_addmethod(alien_membrane_tilde_class, (t_method)alien_membrane_dsp,
                    gensym("dsp"), A_CANT, 0);
}
