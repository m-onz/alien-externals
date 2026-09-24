/* alien_terrain~.c  --  wave-terrain synthesis
 *
 * A waveform read off a landscape.  Two oscillators, x and y, trace an ORBIT
 * across a 3-D surface z = f(x,y); the height under the orbit is the sound.
 * Nothing about the timbre is a spectrum you dialled -- it emerges from the
 * shape of the surface and the path taken over it, so small moves give large,
 * organic, un-programmable changes.
 *
 * The orbit's frequency ratio decides the path (integer = closed/harmonic,
 * irrational = precessing/metallic), its radius decides how far it reaches
 * (small = gentle valley, near sine; large = the steep folded edges), and a
 * skew reshapes it into a Lissajous.  Six terrains -- classic, ripple, saddle,
 * braid, fold, chaos -- are each a different landscape and so a different
 * instrument.  The two outlets read the surface from mirrored orbits, so a
 * single note is already a stereo pair.
 *
 *   inlet 0   signal/float   fundamental in Hz
 *   inlet 1   signal/float   MACRO 0..100 (wrapped mod 100) -- one knob that
 *                            morphs the whole voice: at 0 a small, harmonic,
 *                            near-sine orbit on the gentle classic terrain;
 *                            climbing through the six landscapes in turn while
 *                            the orbit widens, the ratio goes inharmonic, the
 *                            skew opens and the drive bites, into a torn,
 *                            metallic, chaotic spray at 100.  It sweeps
 *                            terrain/ratio/radius/skew/drive together along a
 *                            hand-tuned trajectory.
 *
 *   outlet 0  signal
 *   outlet 1  signal         mirrored-orbit read (stereo partner)
 *
 * Runs 2x oversampled -- the terrains are polynomials of the oscillators and
 * generate high harmonics.
 *
 * build:
 *   gcc -shared -fPIC -O2 -o alien_terrain~.pd_linux alien_terrain~.c -lm
 *   (macOS: -bundle -undefined dynamic_lookup -o alien_terrain~.pd_darwin)
 */

#include "m_pd.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TWOPI (2.0 * M_PI)
#define OS    2

static t_class *alien_terrain_tilde_class;

typedef struct _alien_terrain {
    t_object x_obj;
    t_float  x_f;

    double   x_sr;
    double   x_phx, x_phy;
    double   x_dc0a, x_dc1a, x_dc0b, x_dc1b;   /* DC blockers */
    int      x_ter;                            /* per-block terrain select    */
} t_alien_terrain;

/* finite check that survives -ffast-math (isfinite() may be optimised away
 * when the compiler is told to assume no NaN/Inf).  Inspect the raw bits. */
static inline int dbl_finite(double v)
{
    union { double d; unsigned long long u; } b;
    b.d = v;
    return ((b.u >> 52) & 0x7ffULL) != 0x7ffULL;   /* exponent all-ones => Inf/NaN */
}

static inline double softclip(double v)
{
    if (!dbl_finite(v)) return 0.0;
    if (v >  3.0) return 1.0;
    if (v < -3.0) return -1.0;
    return v * (27.0 + v*v) / (27.0 + 9.0*v*v);
}

/* the landscapes, x,y in roughly [-radius,radius]; return ~[-1,1].
 * Polynomial terrains are normalised so the whole family stays bounded near
 * unity across the orbit radius range (<= ~1.55), keeping drive predictable
 * and the DC blocker well behaved. */
static inline double terrain(int t, double x, double y)
{
    double z;
    switch (t) {
        case 1:  z = 0.7 * (sin(3.3 * x + 2.1 * y) + 0.5 * sin(5.0 * x * y)); break;
        case 2:  z = 0.42 * (x * x - y * y); break;                 /* saddle    */
        case 3:  z = 0.13 * x * y * (x + y); break;                 /* braid     */
        case 4:  z = sin(M_PI * (x + y)); break;                    /* fold      */
        case 5:  z = sin(5.0 * x * y); break;                       /* chaos     */
        default: /* 0 classic Mitsuhashi */
                 z = 0.35 * (x - y) * (x - 1.0) * (x + 1.0) * (y - 1.0) * (y + 1.0);
                 break;
    }
    /* hard bound before it can feed the recursive DC blocker */
    if (!dbl_finite(z)) return 0.0;
    if (z >  4.0) z =  4.0;
    if (z < -4.0) z = -4.0;
    return z;
}

/* ------------------------------------------------------------------ */

/* normalise a raw macro sample into t in [0,1): wrap mod 100, guard NaN/Inf. */
static inline double macro_norm(double mac)
{
    double t;
    if (!dbl_finite(mac)) return 0.0;
    t = mac - 100.0 * floor(mac * 0.01);
    if (!(t >= 0.0 && t <= 100.0)) t = 0.0;
    return t * 0.01;
}

/* discrete terrain select from t (once per block). */
static inline int terrain_select(double t)
{
    int ter = (int)(t * 6.0);
    if (ter < 0) ter = 0;
    if (ter > 5) ter = 5;
    return ter;
}

/* map the 0..1 macro onto the continuous parameter set (curated morph).
 * The continuous controls are cheap enough to run per sample so the sweep
 * stays smooth; the DISCRETE terrain select is derived once per block. */
static inline void terrain_macro(double t, double *ratio,
                                 double *radius, double *skew, double *drive)
{
    *ratio  = 1.0 + 5.0 * t * t;               /* harmonic -> inharmonic      */
    *radius = 0.3 + 1.25 * t;                  /* gentle valley -> folded edge*/
    *skew   = 0.5 * t;                         /* orbit opens into Lissajous  */
    *drive  = (t < 0.55) ? 0.0 : (t - 0.55) / 0.45;   /* grit only up top     */
}

static t_int *alien_terrain_perform(t_int *w)
{
    t_alien_terrain *x = (t_alien_terrain *)(w[1]);
    t_sample *fin = (t_sample *)(w[2]);
    t_sample *min = (t_sample *)(w[3]);
    t_sample *outa= (t_sample *)(w[4]);
    t_sample *outb= (t_sample *)(w[5]);
    int nsamp = (int)(w[6]);

    double sr = x->x_sr, sr_os = sr * OS;
    int ter;

    /* --- once per block: discrete terrain select from the macro's first
     * sample.  Cheap, and it stops the audio-rate landscape switching that
     * caused the stutter. Held stable across the block. */
    if (nsamp > 0) {
        ter = terrain_select(macro_norm((double)min[0]));
        x->x_ter = ter;
    } else {
        ter = x->x_ter;
    }

    while (nsamp--) {
        double f0 = *fin++, mac = *min++;
        double t, ratio, radius, skew, drive;
        double za = 0.0, zb = 0.0, oa, ob;
        int os;

        t = macro_norm(mac);
        terrain_macro(t, &ratio, &radius, &skew, &drive);

        if (!dbl_finite(f0)) f0 = 0.01;
        if (f0 < 0.01)   f0 = 0.01;           /* forbid divide-by-zero paths */
        if (f0 > 8000.0) f0 = 8000.0;

        for (os = 0; os < OS; os++) {
            double xv, yv;
            x->x_phx += f0 / sr_os;
            x->x_phy += f0 * ratio / sr_os;
            /* wrap (increments are always < 1 here, but stay safe) */
            x->x_phx -= floor(x->x_phx);
            x->x_phy -= floor(x->x_phy);
            xv = radius * sin(TWOPI * x->x_phx);
            yv = radius * sin(TWOPI * (x->x_phy + skew));
            za += terrain(ter, xv, yv);
            zb += terrain(ter, yv, xv);          /* mirrored orbit */
        }
        za /= OS; zb /= OS;

        /* DC block both (terrains are not zero-mean) */
        oa = za - x->x_dc0a + 0.999 * x->x_dc1a; x->x_dc0a = za; x->x_dc1a = oa;
        ob = zb - x->x_dc0b + 0.999 * x->x_dc1b; x->x_dc0b = zb; x->x_dc1b = ob;

        if (drive > 0.0) {
            oa = softclip(oa * (1.0 + drive * 6.0)) / (1.0 + drive * 1.5);
            ob = softclip(ob * (1.0 + drive * 6.0)) / (1.0 + drive * 1.5);
        } else { oa = softclip(oa); ob = softclip(ob); }

        oa *= 0.8; ob *= 0.8;

        /* final finite + bound guard (soft clip already keeps |.| <= 1) */
        if (!dbl_finite(oa)) oa = 0.0;
        if (!dbl_finite(ob)) ob = 0.0;
        if (oa >  1.5) oa =  1.5; else if (oa < -1.5) oa = -1.5;
        if (ob >  1.5) ob =  1.5; else if (ob < -1.5) ob = -1.5;

        *outa++ = (t_sample)oa;
        *outb++ = (t_sample)ob;
    }

    /* if the DC-blocker state ever went non-finite, reset it so the next
     * block starts clean instead of propagating NaN forever */
    if (!dbl_finite(x->x_dc0a) || !dbl_finite(x->x_dc1a)) x->x_dc0a = x->x_dc1a = 0.0;
    if (!dbl_finite(x->x_dc0b) || !dbl_finite(x->x_dc1b)) x->x_dc0b = x->x_dc1b = 0.0;
    if (!dbl_finite(x->x_phx)) x->x_phx = 0.0;
    if (!dbl_finite(x->x_phy)) x->x_phy = 0.0;

    return (w + 7);
}

static void alien_terrain_dsp(t_alien_terrain *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    dsp_add(alien_terrain_perform, 6, x,
            sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, sp[3]->s_vec,
            (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_terrain_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_terrain *x = (t_alien_terrain *)pd_new(alien_terrain_tilde_class);
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    x->x_phx = 0.0; x->x_phy = 0.25;
    x->x_dc0a = x->x_dc1a = x->x_dc0b = x->x_dc1b = 0.0;
    x->x_ter = 0;

    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);   /* macro */
    outlet_new(&x->x_obj, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_terrain_tilde_setup(void)
{
    alien_terrain_tilde_class = class_new(gensym("alien_terrain~"),
        (t_newmethod)alien_terrain_new, 0, sizeof(t_alien_terrain),
        CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_terrain_tilde_class, t_alien_terrain, x_f);
    class_addmethod(alien_terrain_tilde_class, (t_method)alien_terrain_dsp, gensym("dsp"), A_CANT, 0);
}
