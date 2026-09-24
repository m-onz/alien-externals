/* alien_chaos~.c  --  deterministic chaos as an oscillator
 *
 * Three classic strange attractors -- Chua's circuit, Lorenz, Rossler --
 * integrated at audio rate and read out as sound.  Each system moves
 * through its own bifurcation: at one end a clean periodic tone, then
 * period-doubling into windows of order and chaos, then the full
 * broadband attractor.  It is a noise source you can tune all the way
 * down to a pitch.
 *
 * Unlike the emergent objects that fuse many voices (kuramoto, ca), this
 * is one low-dimensional nonlinear system.  Its whole state is three
 * numbers, so the second outlet gives you another axis of the SAME
 * trajectory -- a phase-locked modulation source, and an honest x/y plot
 * of the attractor for visuals.
 *
 *   inlet 0   signal/float   pitch in Hz -- scales the integration rate
 *   inlet 1   signal/float   MACRO 0..100 (wrapped mod 100) -- one knob that
 *                            morphs the whole voice.  It walks the three
 *                            attractors in turn (chua, then lorenz, then
 *                            rossler) and inside each sweeps the regime from
 *                            periodic to chaotic while bringing drive up
 *                            toward the top: at 0 a clean mellow Chua tone,
 *                            at 100 a driven broadband Rossler roar.
 *
 *   outlet 0  signal         one axis of the attractor
 *   outlet 1  signal         a second axis (modulation / visuals)
 *
 * The integrator is oversampled and the state is clamped, so it cannot
 * blow up; a DC blocker sits on each output because attractors are not
 * centred on zero.  The state is re-seeded whenever the macro crosses into
 * a new attractor, since a fresh system can otherwise sit on a fixed point.
 *
 * build:
 *   gcc -shared -fPIC -O2 -o alien_chaos~.pd_linux alien_chaos~.c -lm
 *   (macOS: -bundle -undefined dynamic_lookup -o alien_chaos~.pd_darwin)
 */

#include "m_pd.h"
#include <math.h>
#include <string.h>

#define OS 8            /* integration oversampling per audio sample */

enum { SYS_CHUA = 0, SYS_LORENZ, SYS_ROSSLER };

static t_class *alien_chaos_tilde_class;

typedef struct _alien_chaos {
    t_object x_obj;
    t_float  x_f;

    double   x_sr;
    double   x_x, x_y, x_z;      /* state */
    double   x_dc0a, x_dc0b;     /* DC-blocker history (in) */
    double   x_dc1a, x_dc1b;     /* DC-blocker history (out) */

    int      x_sys;              /* last-selected attractor */
    unsigned x_rng;
} t_alien_chaos;

static unsigned rn(t_alien_chaos *x)
{
    x->x_rng ^= x->x_rng << 13; x->x_rng ^= x->x_rng >> 17;
    x->x_rng ^= x->x_rng << 5;  return x->x_rng;
}
static double frnd(t_alien_chaos *x)
{
    return (double)(rn(x) & 0xffffff) / (double)0x800000 - 1.0;
}

static inline double softclip(double v)
{
    if (v >  3.0) return 1.0;
    if (v < -3.0) return -1.0;
    return v * (27.0 + v*v) / (27.0 + 9.0*v*v);
}
static inline double clampd(double v, double a)
{
    if (v >  a) return a;
    if (v < -a) return -a;
    return v;
}

static void chaos_reset(t_alien_chaos *x)
{
    switch (x->x_sys) {
        case SYS_LORENZ:  x->x_x = 0.1; x->x_y = 0.0; x->x_z = 20.0; break;
        case SYS_ROSSLER: x->x_x = 0.1; x->x_y = 0.0; x->x_z = 0.0;  break;
        default:          x->x_x = 0.2 + 0.01 * frnd(x); x->x_y = 0.0; x->x_z = 0.0;
    }
}

/* one derivative evaluation for the selected system */
static void deriv(t_alien_chaos *x, double reg,
                  double px, double py, double pz,
                  double *dx, double *dy, double *dz)
{
    if (x->x_sys == SYS_LORENZ) {
        double sigma = 10.0, beta = 8.0/3.0;
        double rho = 20.0 + reg * 28.0;          /* 20..48, chaos ~>24.7 */
        *dx = sigma * (py - px);
        *dy = px * (rho - pz) - py;
        *dz = px * py - beta * pz;
    } else if (x->x_sys == SYS_ROSSLER) {
        double a = 0.2, b = 0.2;
        double c = 2.5 + reg * 8.0;               /* 2.5..10.5 */
        *dx = -py - pz;
        *dy = px + a * py;
        *dz = b + pz * (px - c);
    } else { /* Chua */
        /* canonical bounded double-scroll parameters (beta = 28).  Floor at
         * alpha 10.5 so even reg=0 sits PAST the Hopf bifurcation on an
         * audible single-scroll limit cycle (below ~10.4 it decays onto a
         * stable focus and the DC blocker mutes it -> dead silence); top at
         * ~15.5 reaches the double-scroll chaos and stays bounded. */
        double alpha = 10.5 + reg * 5.0;          /* 10.5..15.5 (above Hopf) */
        double beta = 28.0, m0 = -1.143, m1 = -0.714;
        double h = m1 * px + 0.5 * (m0 - m1) * (fabs(px + 1.0) - fabs(px - 1.0));
        *dx = alpha * (py - px - h);
        *dy = px - py + pz;
        *dz = -beta * py;
    }
}

/* per-system readout scaling so all three land around unity */
static double out_scale(int sys) {
    switch (sys) {
        case SYS_LORENZ:  return 0.032;
        case SYS_ROSSLER: return 0.060;
        default:          return 0.32;   /* chua */
    }
}
/* base timestep so f0 maps to a musical pitch for each system */
static double base_dt(int sys) {
    switch (sys) {
        case SYS_LORENZ:  return 0.0022;
        case SYS_ROSSLER: return 0.028;
        default:          return 0.010;  /* chua */
    }
}

/* ------------------------------------------------------------------ */

/* map the 0..1 macro onto system + regime + drive (curated morph).
 * The lower third is Chua, the middle Lorenz, the top Rossler; inside
 * each third the regime sweeps periodic -> chaotic, and drive comes up
 * across the whole range so the brightest voice is also the grittiest. */
static inline void chaos_macro(double t, int *sys, double *reg, double *drive)
{
    double u;                                     /* position within a third */
    if (t < 1.0/3.0)       { *sys = SYS_CHUA;    u = t * 3.0; }
    else if (t < 2.0/3.0)  { *sys = SYS_LORENZ;  u = (t - 1.0/3.0) * 3.0; }
    else                   { *sys = SYS_ROSSLER; u = (t - 2.0/3.0) * 3.0; }
    if (u > 1.0) u = 1.0;
    *reg   = u;                                    /* periodic -> chaotic */
    *drive = (t < 0.6) ? 0.0 : (t - 0.6) / 0.4;    /* grit only up top */
}

static t_int *alien_chaos_perform(t_int *w)
{
    t_alien_chaos *x = (t_alien_chaos *)(w[1]);
    t_sample *fin  = (t_sample *)(w[2]);
    t_sample *min  = (t_sample *)(w[3]);
    t_sample *out  = (t_sample *)(w[4]);
    t_sample *out2 = (t_sample *)(w[5]);
    int nsamp = (int)(w[6]);

    /* -------- once per block: derive the integer system from the macro's
     * FIRST sample only.  The attractor index (and any chaos_reset) is a
     * block-rate decision -- deriving it per sample and reseeding whenever a
     * swept/dithered macro crosses a third boundary is what stutters and
     * starves the integrator.  reg/drive still track per sample below, but
     * those are cheap continuous parameters that never reseed. */
    {
        double t0 = min[0];
        int sys0; double reg0, drive0;
        t0 = t0 - 100.0 * floor(t0 * 0.01);
        if (!(t0 >= 0.0 && t0 <= 100.0)) t0 = 0.0;   /* NaN/inf guard */
        t0 *= 0.01;
        chaos_macro(t0, &sys0, &reg0, &drive0);
        if (sys0 != x->x_sys) {                      /* real integer change */
            x->x_sys = sys0;
            chaos_reset(x);
        }
    }

    {
        double sc  = out_scale(x->x_sys);
        double bdt = base_dt(x->x_sys);

        while (nsamp--) {
            double f0 = *fin++, mac = *min++;
            double t, reg, drive, dt, ya, yb, o;
            int sys, k;

            /* wrap the macro into [0,100) then normalise to [0,1).  Only the
             * continuous reg/drive from here are used per sample; the integer
             * `sys` is IGNORED (already fixed for this block above). */
            t = mac - 100.0 * floor(mac * 0.01);
            if (!(t >= 0.0 && t <= 100.0)) t = 0.0;   /* NaN/inf guard */
            t *= 0.01;
            chaos_macro(t, &sys, &reg, &drive);

            if (!(f0 == f0)) f0 = 110.0;              /* NaN pitch guard */
            if (f0 < 0.01)   f0 = 0.01;               /* keep dt bounded */
            if (f0 > 6000.0) f0 = 6000.0;

            /* dt scales with pitch; divided across the oversampling steps */
            dt = bdt * (f0 / 110.0) / (double)OS;
            if (dt > 0.1) dt = 0.1;

            for (k = 0; k < OS; k++) {
                double k1x,k1y,k1z, k2x,k2y,k2z;
                double mx,my,mz;
                deriv(x, reg, x->x_x, x->x_y, x->x_z, &k1x,&k1y,&k1z);
                mx = x->x_x + 0.5*dt*k1x;
                my = x->x_y + 0.5*dt*k1y;
                mz = x->x_z + 0.5*dt*k1z;
                deriv(x, reg, mx, my, mz, &k2x,&k2y,&k2z);   /* RK2 midpoint */
                x->x_x += dt * k2x;
                x->x_y += dt * k2y;
                x->x_z += dt * k2z;

                /* blow-up guard: if the integrator diverges to NaN/Inf,
                 * re-seed this block's system and bail out of oversampling */
                if (!(isfinite(x->x_x) && isfinite(x->x_y) && isfinite(x->x_z))) {
                    chaos_reset(x);
                    break;
                }
                /* hard safety clamp -- attractors stay bounded, but pitch
                 * sweeps can transiently overshoot the integrator.  flush
                 * denormals so the state can't stall in a slow-math trap. */
                x->x_x = clampd(x->x_x, 60.0)  + 1e-30 - 1e-30;
                x->x_y = clampd(x->x_y, 60.0)  + 1e-30 - 1e-30;
                x->x_z = clampd(x->x_z, 120.0) + 1e-30 - 1e-30;
            }

            ya = x->x_x * sc;
            yb = x->x_y * sc;

            /* DC block outlet 0 (one-pole highpass, R=0.999), then soft
             * saturate rather than hard clamp so overshoots stay musical */
            o = ya - x->x_dc0a + 0.999 * x->x_dc1a;
            x->x_dc0a = ya; x->x_dc1a = o;
            if (!isfinite(o)) { o = 0.0; x->x_dc1a = 0.0; }
            if (drive > 0.0)
                o = o * (1.0 + drive * 6.0) / (1.0 + drive * 2.0);
            o = softclip(o);
            if (!isfinite(o)) o = 0.0;
            o = clampd(o, 1.5);                       /* final output safety */
            *out++ = (t_sample)(o * 0.73);

            /* DC block outlet 1 */
            o = yb - x->x_dc0b + 0.999 * x->x_dc1b;
            x->x_dc0b = yb; x->x_dc1b = o;
            if (!isfinite(o)) { o = 0.0; x->x_dc1b = 0.0; }
            o = softclip(o);
            if (!isfinite(o)) o = 0.0;
            o = clampd(o, 1.5);
            *out2++ = (t_sample)(o * 0.73);
        }
    }
    return (w + 7);
}

static void alien_chaos_dsp(t_alien_chaos *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    dsp_add(alien_chaos_perform, 6, x,
            sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, sp[3]->s_vec,
            (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_chaos_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_chaos *x = (t_alien_chaos *)pd_new(alien_chaos_tilde_class);
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    x->x_sys = SYS_CHUA;
    x->x_rng = 77773u;
    x->x_dc0a = x->x_dc1a = x->x_dc0b = x->x_dc1b = 0.0;
    chaos_reset(x);

    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_chaos_tilde_setup(void)
{
    alien_chaos_tilde_class = class_new(gensym("alien_chaos~"),
        (t_newmethod)alien_chaos_new, 0, sizeof(t_alien_chaos),
        CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_chaos_tilde_class, t_alien_chaos, x_f);
    class_addmethod(alien_chaos_tilde_class, (t_method)alien_chaos_dsp,
                    gensym("dsp"), A_CANT, 0);
}
