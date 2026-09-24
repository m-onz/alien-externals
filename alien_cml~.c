/* alien_cml~.c  --  coupled map lattice (spatiotemporal chaos)
 *
 * A row of cells, each running a chaotic map, each diffusively coupled to its
 * neighbours (Kaneko's coupled map lattice).  The row is read as a wavetable
 * at audio rate, so you hear the lattice think.  Where [alien_ca~] is a binary
 * rule and [alien_chaos~] is one chaotic point, this is a whole FIELD of
 * chaos that talks to itself: at low coupling every cell does its own thing
 * (noise); raise it and the cells synchronise into smooth travelling waves;
 * in between is turbulence -- domains forming, defects drifting, patterns
 * dissolving -- an evolving alien texture that never settles and never
 * repeats.
 *
 *   inlet 0   signal/float   fundamental in Hz (the wavetable read = pitch)
 *   inlet 1   signal/float   MACRO 0..100 (wrapped mod 100) -- one knob that
 *                            morphs the whole voice: at 0 a slow, mellow,
 *                            strongly-coupled synchronised wave (near sine),
 *                            climbing through turbulent travelling patterns
 *                            and swapping the underlying chaotic map, into a
 *                            fast, weakly-coupled, driven incoherent roar at
 *                            100. It sweeps map/param/coupling/rate/drive
 *                            together along a hand-tuned trajectory.
 *
 *   outlet 0  signal
 *   outlet 1  list           cell states, one list per lattice generation, for
 *                            visuals
 *
 * build:
 *   gcc -shared -fPIC -O2 -o alien_cml~.pd_linux alien_cml~.c -lm
 *   (macOS: -bundle -undefined dynamic_lookup -o alien_cml~.pd_darwin)
 */

#include "m_pd.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TWOPI (2.0 * M_PI)
#define MAXC  256                 /* physical capacity of every lattice array  */
#define EMITMAX 256               /* cap on atoms emitted per list (<= MAXC)   */
#define CELL_LO (-4.0)            /* hard clamp band for a lattice cell        */
#define CELL_HI ( 4.0)

static t_class *alien_cml_tilde_class;

typedef struct _alien_cml {
    t_object x_obj;
    t_float  x_f;

    double   x_sr;
    double   x_cell[MAXC];
    double   x_fx[MAXC];       /* map(cell) scratch */
    int      x_ncell, x_map, x_lastmap;

    double   x_ph;             /* wavetable read phase */
    double   x_gph;            /* generation clock phase */
    double   x_dc0, x_dc1;

    int      x_interp, x_wrap;
    double   x_param, x_coupling, x_rate, x_drive;

    int      x_lastmacq;       /* last per-block discrete macro quantum (-1=none) */
    int      x_dirty;          /* lattice changed since last list emission?    */

    unsigned x_rng;
    t_outlet *x_list;
    t_atom    x_atoms[MAXC];
} t_alien_cml;

static unsigned rn(t_alien_cml *x)
{
    x->x_rng ^= x->x_rng << 13; x->x_rng ^= x->x_rng >> 17;
    x->x_rng ^= x->x_rng << 5;  return x->x_rng;
}
static double frnd(t_alien_cml *x){ return (double)(rn(x)&0xffffff)/(double)0x1000000; }

static inline double softclip(double v)
{
    if (v >  3.0) return 1.0;
    if (v < -3.0) return -1.0;
    return v * (27.0 + v*v) / (27.0 + 9.0*v*v);
}

static void cml_seed(t_alien_cml *x)
{
    int i, n = x->x_ncell;
    if (n < 1)    n = 1;
    if (n > MAXC) n = MAXC;
    x->x_ncell = n;
    for (i = 0; i < n; i++) x->x_cell[i] = frnd(x);
    x->x_dirty = 1;
}

static double cml_map(t_alien_cml *x, double v)
{
    double p = x->x_param;
    switch (x->x_map) {
        case 1: { double a = 0.7 + p*0.3; double r = a*sin(M_PI*v);
                  return r < 0 ? 0 : (r > 1 ? 1 : r); }
        case 2: { double mu = 1.2 + p*0.799; double r = mu*(v<0.5?v:1.0-v);
                  return r < 0 ? 0 : (r > 1 ? 1 : r); }
        case 3: { double K = p*6.0; double r = v + 0.3 - (K/TWOPI)*sin(TWOPI*v);
                  return r - floor(r); }
        default:{ double rr = 3.5 + p*0.499; double r = rr*v*(1.0-v);
                  return r < 0 ? 0 : (r > 1 ? 1 : r); }
    }
}

static void cml_stepgen(t_alien_cml *x, double eps)
{
    int i, n = x->x_ncell;
    int bad = 0;
    if (n < 1)    n = 1;
    if (n > MAXC) n = MAXC;             /* never index past the arrays        */
    x->x_ncell = n;
    if (eps < 0.0) eps = 0.0;
    if (eps > 1.0) eps = 1.0;
    for (i = 0; i < n; i++) {
        double m = cml_map(x, x->x_cell[i]);
        if (!isfinite(m)) { m = 0.0; bad = 1; }
        x->x_fx[i] = m;
    }
    for (i = 0; i < n; i++) {
        int l = i - 1, r = i + 1;
        double nl, nr, v;
        if (x->x_wrap) { if (l < 0) l = n-1; if (r >= n) r = 0; nl = x->x_fx[l]; nr = x->x_fx[r]; }
        else { nl = (l < 0) ? x->x_fx[i] : x->x_fx[l]; nr = (r >= n) ? x->x_fx[i] : x->x_fx[r]; }
        v = (1.0 - eps) * x->x_fx[i] + eps * 0.5 * (nl + nr);
        if (!isfinite(v)) { v = 0.0; bad = 1; }
        else {
            if (v < CELL_LO) v = CELL_LO;       /* keep cells in a finite band */
            else if (v > CELL_HI) v = CELL_HI;
            if (fabs(v) < 1e-15) v = 0.0;        /* flush denormals            */
        }
        x->x_cell[i] = v;
    }
    if (bad) cml_seed(x);                        /* diverged -> restart clean  */
    x->x_dirty = 1;
}

static void cml_dump(t_alien_cml *x)
{
    int i, n = x->x_ncell;
    if (n < 1) n = 1;
    if (n > EMITMAX) n = EMITMAX;      /* never exceed the atom buffer         */
    if (n > MAXC)    n = MAXC;
    for (i = 0; i < n; i++) {
        double v = x->x_cell[i];
        if (!isfinite(v)) v = 0.0;
        SETFLOAT(x->x_atoms + i, (t_float)v);
    }
    outlet_list(x->x_list, &s_list, n, x->x_atoms);
    x->x_dirty = 0;
}

/* ------------------------------------------------------------------ */

/* map the 0..1 macro onto the whole parameter set (curated morph).
 * t = 0 : darkest / mellow  -- strong coupling (synchronised smooth wave),
 *                              gentle chaos, slow evolution, no drive, map 0.
 * t = 1 : brightest / harsh -- weak coupling (incoherent field), full chaos,
 *                              fast evolution, hard drive; the map switches
 *                              logistic -> sine -> tent -> circle up the sweep. */
static inline void cml_macro(double t, int *map, double *param,
                             double *coupling, double *rate, double *drive)
{
    /* discrete map: split the sweep into four sub-ranges                     */
    if      (t < 0.25) *map = 0;   /* logistic */
    else if (t < 0.50) *map = 1;   /* sine     */
    else if (t < 0.75) *map = 2;   /* tent     */
    else               *map = 3;   /* circle   */

    *param    = 0.20 + 0.80 * t;              /* gentle -> full chaos          */
    *coupling = 0.90 - 0.85 * t;              /* synchronised -> incoherent    */
    if (*coupling < 0.0) *coupling = 0.0;
    *rate     = 8.0 + 240.0 * t * t;          /* slow -> fast evolution        */
    *drive    = (t < 0.6) ? 0.0 : (t - 0.6) / 0.4;  /* grit only up top        */
}

static t_int *alien_cml_perform(t_int *w)
{
    t_alien_cml *x = (t_alien_cml *)(w[1]);
    t_sample *fin = (t_sample *)(w[2]);
    t_sample *min = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    int nsamp = (int)(w[5]);

    double sr = x->x_sr;
    int n;

    /* ---- ONCE PER BLOCK: derive the macro from the first sample only ----- *
     * The macro is a slow control signal; evaluating the whole curated morph
     * per-sample was the CPU/stutter culprit, and the per-sample re-seed test
     * was the reset-thrash / crash risk.  We read sample 0, wrap/normalise it,
     * set the continuous params for the block, and take the discrete quantities
     * (map select) only when they actually CHANGE.                            */
    {
        double mac0 = (nsamp > 0) ? (double)min[0] : 0.0;
        double t;
        int macq;

        t = mac0 - 100.0 * floor(mac0 * 0.01);        /* wrap into [0,100)     */
        if (!(t >= 0.0 && t <= 100.0)) t = 0.0;        /* NaN/inf guard         */
        t *= 0.01;                                     /* -> [0,1)              */
        if (t < 0.0) t = 0.0; else if (t > 0.999999) t = 0.999999;

        cml_macro(t, &x->x_map, &x->x_param, &x->x_coupling, &x->x_rate,
                  &x->x_drive);

        /* discrete quantum: change of map (only integer that gates a re-seed) */
        macq = x->x_map;
        if (macq != x->x_lastmacq || x->x_map != x->x_lastmap) {
            if (x->x_map != x->x_lastmap) {
                cml_seed(x);                           /* re-seed ONLY on real  *
                                                        * map change            */
                x->x_lastmap = x->x_map;
            }
            x->x_lastmacq = macq;
        }
    }

    n = x->x_ncell;
    if (n < 1)    n = 1;
    if (n > MAXC) n = MAXC;             /* guarantee every index below is safe */
    x->x_ncell = n;

    while (nsamp--) {
        double f0 = *fin++;
        double y, o, idx, frac; int i0, i1;
        (void)*min++;                  /* macro already consumed per block     */

        if (!(f0 == f0)) f0 = 0.01;    /* NaN -> floor                         */
        if (f0 < 0.01)   f0 = 0.01;    /* forbid divide-by-zero / DC read      */
        if (f0 > 8000.0) f0 = 8000.0;

        /* generation clock: evolve the lattice at [rate] Hz */
        if (x->x_rate > 0.0) {
            x->x_gph += x->x_rate / sr;
            if (x->x_gph >= 1.0) {
                x->x_gph -= floor(x->x_gph);           /* stay bounded if rate  *
                                                        * ever > sr             */
                cml_stepgen(x, x->x_coupling);
            }
        }

        /* read the lattice as a wavetable */
        x->x_ph += f0 / sr;
        x->x_ph -= floor(x->x_ph);                     /* wrap phase to [0,1)   */
        idx = x->x_ph * n;
        i0 = (int)idx;
        if (i0 < 0) i0 = 0; else if (i0 >= n) i0 = n - 1;
        frac = idx - i0;
        i1 = i0 + 1; if (i1 >= n) i1 = 0;              /* clamped i0 -> safe i1 */
        y = x->x_interp ? (x->x_cell[i0] + frac * (x->x_cell[i1] - x->x_cell[i0]))
                        : x->x_cell[i0];
        y = 2.0 * y - 1.0;                       /* [0,1] -> [-1,1] */

        o = y - x->x_dc0 + 0.999 * x->x_dc1;      /* DC block */
        x->x_dc0 = y; x->x_dc1 = o;
        if (x->x_drive > 0.0)
            o = softclip(o * (1.0 + x->x_drive * 6.0)) / (1.0 + x->x_drive * 1.5);
        else o = softclip(o);
        o *= 0.8;                                 /* nominal near unity        */
        if (!isfinite(o)) o = 0.0;                /* final non-finite guard    */
        if (o >  1.5) o =  1.5;                   /* hard safety clip          */
        else if (o < -1.5) o = -1.5;
        *out++ = (t_sample)o;
    }

    /* emit the lattice state at most ONCE per block, and only if it changed   */
    if (x->x_dirty) cml_dump(x);

    return (w + 6);
}

static void alien_cml_dsp(t_alien_cml *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    if (x->x_sr < 1000.0) x->x_sr = 44100.0;   /* forbid divide-by-zero      */
    dsp_add(alien_cml_perform, 5, x,
            sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_cml_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_cml *x = (t_alien_cml *)pd_new(alien_cml_tilde_class);
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    x->x_ncell = 64;             /* fixed lattice length; <= MAXC/EMITMAX      */
    if (x->x_ncell > MAXC) x->x_ncell = MAXC;
    x->x_map = 0; x->x_lastmap = 0; x->x_lastmacq = -1;
    x->x_dirty = 0;
    x->x_ph = 0.0; x->x_gph = 0.0;
    x->x_dc0 = x->x_dc1 = 0.0;
    x->x_interp = 1; x->x_wrap = 1;
    x->x_param = 0.2; x->x_coupling = 0.9; x->x_rate = 8.0; x->x_drive = 0.0;
    x->x_rng = 55511u;
    cml_seed(x);

    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    x->x_list = outlet_new(&x->x_obj, &s_list);
    return (void *)x;
}

void alien_cml_tilde_setup(void)
{
    alien_cml_tilde_class = class_new(gensym("alien_cml~"),
        (t_newmethod)alien_cml_new, 0, sizeof(t_alien_cml),
        CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_cml_tilde_class, t_alien_cml, x_f);
    class_addmethod(alien_cml_tilde_class, (t_method)alien_cml_dsp, gensym("dsp"), A_CANT, 0);
}
