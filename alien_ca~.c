/* alien_ca~.c  --  cellular automaton as a living wavetable
 *
 * A row of cells evolves by an elementary CA rule (0-255) at a rate you
 * set, while a phase accumulator reads the row as a waveform at audio
 * rate.  The rule number is the timbre control, and it is genuinely
 * discontinuous: rule 30 is noise, rule 90 is a fractal (Sierpinski),
 * rule 110 is structured and Turing-complete, rule 150 is dense.
 *
 * The cell state also comes out of the right outlet as a list, so the
 * same array can be drawn in GEM.  The picture on screen is literally
 * the waveform you are hearing, not a reaction to it.
 *
 *   inlet 0   signal/float   frequency in Hz
 *   inlet 1   signal/float   MACRO 0..100 (wrapped mod 100) -- one knob that
 *                            morphs the whole voice along a hand-tuned
 *                            trajectory: at 0 a slow, mellow, tonal drone
 *                            (short smoothed table on the calm Sierpinski
 *                            rule 90); climbing through rule 150 and 110 it
 *                            grows longer, faster-evolving and richer; at 100
 *                            a fast, dense, aliased chaotic buzz (long table
 *                            on the noise rule 30, driven hard). It sweeps
 *                            rule / size / rate / mode / density / drive
 *                            together.
 *
 *   outlet 0  signal
 *   outlet 1  list           cell states, on each step, for visuals
 *
 * build:
 *   gcc -shared -fPIC -O2 -o alien_ca~.pd_linux alien_ca~.c -lm
 */

#include "m_pd.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAXCELL 512

static t_class *alien_ca_tilde_class;

typedef struct _alien_ca {
    t_object  x_obj;
    t_float   x_f;
    t_outlet *x_cells_out;

    unsigned char x_cell[MAXCELL];
    unsigned char x_next[MAXCELL];
    float         x_tab[MAXCELL + 1];   /* +1 guard point for interp */

    double   x_sr, x_ph, x_evoph;
    int      x_size, x_rule, x_mode;
    t_float  x_rate, x_density;
    unsigned x_rng;
    int      x_pending;                 /* cells changed, ask for a dump */
    int      x_lastrule, x_lastsize;    /* re-seed the row when these change */
} t_alien_ca;

/* ------------------------------------------------------------------ */

static unsigned rn(t_alien_ca *x)
{
    x->x_rng ^= x->x_rng << 13; x->x_rng ^= x->x_rng >> 17;
    x->x_rng ^= x->x_rng << 5;  return x->x_rng;
}

/* build the audio table from the current cell state */
static void ca_build(t_alien_ca *x)
{
    int n = x->x_size, i;
    if (n < 1)       n = 1;
    if (n > MAXCELL) n = MAXCELL;
    x->x_size = n;

    if (x->x_mode == 1) {
        /* groups of 8 cells read as an unsigned byte: many more distinct
         * levels than +/-1, so the waveform has real shape */
        for (i = 0; i < n; i++) {
            int b = 0, j;
            for (j = 0; j < 8; j++)
                b = (b << 1) | x->x_cell[(i + j) % n];
            x->x_tab[i] = (float)(b - 128) / 128.f;
        }
    } else if (x->x_mode == 2) {
        /* local density over a 7-cell neighbourhood: smoother, more
         * tonal, much less harsh at high cell counts */
        for (i = 0; i < n; i++) {
            int s = 0, j;
            for (j = -3; j <= 3; j++) s += x->x_cell[((i + j) % n + n) % n];
            x->x_tab[i] = (float)s / 3.5f - 1.f;
        }
    } else {
        for (i = 0; i < n; i++) x->x_tab[i] = x->x_cell[i] ? 1.f : -1.f;
    }
    x->x_tab[n] = x->x_tab[0];          /* wrap point */
}

static void ca_evolve(t_alien_ca *x, int rule)
{
    int n = x->x_size, i;
    if (n < 1)       n = 1;
    if (n > MAXCELL) n = MAXCELL;
    x->x_size = n;
    for (i = 0; i < n; i++) {
        int l, c, r, idx;
        /* circular boundary (formerly the [wrap( option, now always on) */
        l = x->x_cell[(i - 1 + n) % n];
        r = x->x_cell[(i + 1) % n];
        c = x->x_cell[i];
        idx = (l << 2) | (c << 1) | r;   /* 0..7 */
        x->x_next[i] = (rule >> idx) & 1;
    }
    memcpy(x->x_cell, x->x_next, (size_t)n);
    ca_build(x);
    x->x_pending = 1;
}

/* emit the cell state.  the atom buffer is exactly MAXCELL, so n is hard
 * clamped to it here regardless of anything upstream -- this is the loop
 * that would otherwise overrun and crash Pd. */
static void ca_dump(t_alien_ca *x)
{
    t_atom a[MAXCELL];
    int i, n = x->x_size;
    if (n < 0)       n = 0;
    if (n > MAXCELL) n = MAXCELL;
    for (i = 0; i < n; i++) SETFLOAT(a + i, x->x_cell[i]);
    outlet_list(x->x_cells_out, &s_list, n, a);
}

/* seed the row: idx 0 single, 1 random, 2 alternate, 3 edge */
static void ca_seed(t_alien_ca *x, int idx)
{
    int i, n = x->x_size;
    if (n < 1)       n = 1;
    if (n > MAXCELL) n = MAXCELL;
    x->x_size = n;
    memset(x->x_cell, 0, sizeof(x->x_cell));
    switch (((idx % 4) + 4) % 4) {
        case 0: x->x_cell[n/2] = 1; break;                 /* single      */
        case 1:                                             /* random      */
            for (i = 0; i < n; i++)
                x->x_cell[i] = ((rn(x) & 0xffff) / 65535.f < x->x_density);
            break;
        case 2:                                             /* alternate   */
            for (i = 0; i < n; i++) x->x_cell[i] = (i & 1);
            break;
        default: x->x_cell[0] = 1; break;                   /* edge        */
    }
    ca_build(x);
    x->x_pending = 1;
}

/* ------------------------------------------------------------------ */

/* map the 0..1 macro onto the whole parameter set (curated morph).
 * rule is discrete: walk a curated set from calm/tonal to chaotic/noisy.
 * everything continuous (size, rate, mode, density, drive) sweeps with t. */
static void ca_macro(double t, int *rule, int *size, double *rate,
                     int *mode, double *density, double *drive)
{
    /* five rules ordered simple/tonal -> chaotic/noisy */
    static const int rules[5] = { 90, 150, 110, 45, 30 };
    int ri = (int)(t * 5.0);
    if (ri > 4) ri = 4;
    *rule = rules[ri];

    *size = (int)(32.0 + 288.0 * t);          /* short -> long table       */
    if (*size < 8)       *size = 8;
    if (*size > MAXCELL) *size = MAXCELL;

    *rate = 2.0 + 200.0 * t * t;              /* slow -> fast evolution    */

    /* mode: smoothed/tonal low, +/-1 mid, bytes/harsh high */
    *mode = (t < 0.33) ? 2 : (t < 0.66 ? 0 : 1);

    *density = 0.2 + 0.6 * t;                 /* sparse -> busy random fill */

    *drive = (t < 0.7) ? 0.0 : (t - 0.7) / 0.3;   /* grit only up top       */
}

static inline double softclip(double v)
{
    if (v >  3.0) return 1.0;
    if (v < -3.0) return -1.0;
    return v * (27.0 + v*v) / (27.0 + 9.0*v*v);
}

static t_int *alien_ca_perform(t_int *w)
{
    t_alien_ca *x = (t_alien_ca *)(w[1]);
    t_sample *fin = (t_sample *)(w[2]);
    t_sample *min = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    int nsamp = (int)(w[5]);

    double sr = x->x_sr, ph = x->x_ph, ev = x->x_evoph;
    double rate, drive;
    int n;

    /* ---- ONCE PER BLOCK: derive the discrete/expensive quantities from
     * the macro's FIRST sample only.  Doing this per-sample re-seeded the
     * whole CA on nearly every sample (size is a continuous function of the
     * macro), which is what stuttered and then crashed Pd. */
    {
        double mac = (nsamp > 0) ? (double)min[0] : 0.0;
        double t;
        int rule, size, mode;
        double density;

        t = mac - 100.0 * floor(mac * 0.01);
        if (!(t >= 0.0 && t <= 100.0)) t = 0.0;   /* NaN/inf guard */
        t *= 0.01;
        ca_macro(t, &rule, &size, &rate, &mode, &density, &drive);

        if (size < 1)       size = 1;
        if (size > MAXCELL) size = MAXCELL;

        x->x_rule    = rule;
        x->x_rate    = (t_float)rate;
        x->x_density = (t_float)density;

        /* Act only when a DISCRETE quantity actually changed vs last block.
         * size change -> re-seed (single); rule change -> re-seed (clean
         * start on the new rule); mode-only change -> just rebuild table. */
        if (size != x->x_lastsize) {
            x->x_size     = size;
            x->x_lastsize = size;
            x->x_mode     = mode;
            x->x_lastrule = rule;
            ca_seed(x, 0);
        } else if (rule != x->x_lastrule) {
            x->x_mode     = mode;
            x->x_lastrule = rule;
            ca_seed(x, 0);
        } else if (mode != x->x_mode) {
            x->x_mode = mode;
            ca_build(x);
        }
    }

    n = x->x_size;
    if (n < 1)       n = 1;
    if (n > MAXCELL) n = MAXCELL;
    x->x_size = n;

    while (nsamp--) {
        double f0 = *fin++;
        double pos, frac, y;
        int i0;
        (void)min;   /* macro already sampled once per block above */

        if (!(f0 >= 0.01)) f0 = 0.01;          /* also catches NaN */
        if (f0 > 8000.0)   f0 = 8000.0;

        /* evolve on its own clock, independent of pitch */
        if (x->x_rate > 0.f) {
            ev += (double)x->x_rate / sr;
            if (ev >= 1.0) {
                ev -= 1.0;
                if (ev >= 1.0) ev = 0.0;       /* guard huge rate */
                ca_evolve(x, x->x_rule);
            }
        }

        ph += f0 / sr;
        if (ph >= 1.0) ph -= floor(ph);
        if (!(ph >= 0.0 && ph < 1.0)) ph = 0.0;

        pos  = ph * (double)n;
        i0   = (int)pos;
        if (i0 < 0)      i0 = 0;
        if (i0 >= n)     i0 = n - 1;
        frac = pos - (double)i0;
        y = x->x_tab[i0] + (x->x_tab[i0+1] - x->x_tab[i0]) * frac;

        if (drive > 0.0)
            y = softclip(y * (1.0 + drive * 4.0)) / (1.0 + drive * 1.5);

        y *= 0.8;

        /* NaN / Inf guard + soft bound to ~[-1.5,1.5].  -ffast-math can
         * defeat isfinite(), so back it with an explicit range test. */
        if (!isfinite(y) || !(y > -1.0e6 && y < 1.0e6)) y = 0.0;
        if (y >  1.5) y =  1.5;
        if (y < -1.5) y = -1.5;

        *out++ = (t_sample)(y * 0.35);
    }
    x->x_ph = ph; x->x_evoph = ev;

    /* Emit the cell state at most once per block, and only when it actually
     * changed.  ca_dump hard-clamps n to the atom buffer size. */
    if (x->x_pending) { x->x_pending = 0; ca_dump(x); }
    return (w + 6);
}

static void alien_ca_dsp(t_alien_ca *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    dsp_add(alien_ca_perform, 5, x,
            sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_ca_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_ca *x = (t_alien_ca *)pd_new(alien_ca_tilde_class);
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    x->x_ph = x->x_evoph = 0.0;
    x->x_size = 128; x->x_rule = 30; x->x_mode = 1;
    x->x_rate = 20.f; x->x_density = 0.5f;
    x->x_rng = 1103515245u; x->x_pending = 0;
    x->x_lastrule = x->x_rule; x->x_lastsize = x->x_size;

    memset(x->x_cell, 0, sizeof(x->x_cell));
    x->x_cell[x->x_size/2] = 1;
    ca_build(x);

    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    x->x_cells_out = outlet_new(&x->x_obj, &s_list);
    return (void *)x;
}

void alien_ca_tilde_setup(void)
{
    alien_ca_tilde_class = class_new(gensym("alien_ca~"),
        (t_newmethod)alien_ca_new, 0, sizeof(t_alien_ca),
        CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_ca_tilde_class, t_alien_ca, x_f);
    class_addmethod(alien_ca_tilde_class, (t_method)alien_ca_dsp,
                    gensym("dsp"), A_CANT, 0);
}
