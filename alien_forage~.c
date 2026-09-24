/* alien_forage~.c  --  a split-band bass for techy drum & bass
 *
 *   low band   : phase-locked sine sub (owns the deep end)
 *   upper band : procedural wavetable -> phase distortion -> wavefolder
 *                -> detuned unison -> state-variable filter -> drive
 *
 * The upper waveform is generated from a small continuous "genome" -- a set
 * of spectral knobs (tilt, odd/even, formant position/width/peak) rendered
 * to a single-cycle table.  One macro knob walks that genome along a
 * hand-tuned trajectory and sweeps the whole voice chain with it, from a
 * clean harmonic sub-tone up to a bright detuned wavefolded growl.
 *
 *   inlet 0   signal/float   fundamental in Hz  (use [mtof] to play MIDI)
 *   inlet 1   signal/float   MACRO 0..100 (wrapped mod 100) -- one knob that
 *                            morphs the whole voice: at 0 a soft harmonic
 *                            sub sine, climbing through saw/square/reese
 *                            timbres into a bright, detuned, wavefolded and
 *                            driven metallic growl at 100. It sweeps the
 *                            genome, detune, morph, fold, cutoff, res and
 *                            drive together along one curated path.
 *
 *   outlet 0  signal         the bass
 *   outlet 1  float          spectral brightness (0..1) of the current timbre
 *
 * Amplitude is continuous -- gate it with a drum trigger / [*~], the
 * drum-and-bass way.
 *
 * build:
 *   gcc -shared -fPIC -O2 -o alien_forage~.pd_linux alien_forage~.c -lm
 *   (macOS: -bundle -undefined dynamic_lookup -o alien_forage~.pd_darwin)
 */

#include "m_pd.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TWOPI   (2.0 * M_PI)
#define TLEN    2048            /* single-cycle table length (power of 2) */
#define NPART   32             /* harmonics used to build the table */
#define NVOICE  3              /* unison voices in the upper band */
#define OS      4              /* oversampling of the nonlinear chain */
#define NCHAR   8              /* number of home characters */
#define NG      5              /* genome dimensions */

static t_class *alien_forage_tilde_class;

typedef struct _alien_forage {
    t_object x_obj;
    t_float  x_f;

    double   x_sr;

    /* two single-cycle tables + a crossfade between them (the wavescan) */
    double   x_table[TLEN];
    double   x_target[TLEN];
    double   x_xfade, x_xfinc;

    int      x_char;            /* last discrete timbre, for retable-on-change */
    double   x_gene[NG];        /* current genome */

    /* voice state */
    double   x_ph[NVOICE];
    double   x_subph;
    double   x_freq;
    double   x_ic1, x_ic2;
    double   x_hp_x1, x_hp_y1;
    double   x_dec;

    t_outlet *x_nvout;
} t_alien_forage;

static inline double softclip(double v)
{
    if (v >  3.0) return 1.0;
    if (v < -3.0) return -1.0;
    return v * (27.0 + v*v) / (27.0 + 9.0*v*v);
}

/* -ffast-math can defeat isfinite(); test via a magnitude bound too. */
static inline int bad_double(double v)
{
    if (v != v) return 1;                 /* NaN: self-compare survives -ffast-math */
    if (v > 1e30 || v < -1e30) return 1;  /* inf / runaway */
    return 0;
}

/* flush denormals / non-finite state to zero */
static inline double flushd(double v)
{
    if (bad_double(v)) return 0.0;
    if (v > -1e-15 && v < 1e-15) return 0.0;
    return v;
}

/* ---- genome -> harmonic spectrum -> table ---------------------------- */

static void gene_spectrum(const double *g, double *amp) /* amp[1..NPART] */
{
    double tilt   = 0.2 + g[0] * 2.3;                 /* 0.2 bright .. 2.5 dark */
    double oe     = g[1];                             /* 0 all .. 1 odd only */
    double center = 1.0 + g[2] * (NPART - 1);         /* formant harmonic */
    double width  = 0.5 + g[3] * (NPART * 0.3);
    double peak   = g[4] * 3.0;
    int k;
    for (k = 1; k <= NPART; k++) {
        double a = pow((double)k, -tilt);
        if ((k & 1) == 0) a *= (1.0 - oe);
        if (peak > 0.0) {
            double d = (double)k - center;
            a += peak * exp(-(d * d) / (2.0 * width * width)) * pow((double)k, -0.3);
        }
        amp[k] = a < 0.0 ? 0.0 : a;
    }
}

static void gene_table(const double *g, double *table)
{
    double amp[NPART + 1];
    int i, k;
    double norm = 0.0;
    gene_spectrum(g, amp);
    for (i = 0; i < TLEN; i++) {
        double s = 0.0, ph = (double)i / (double)TLEN;
        for (k = 1; k <= NPART; k++)
            if (amp[k] != 0.0) s += amp[k] * sin(TWOPI * k * ph);
        table[i] = s;
        if (fabs(s) > norm) norm = fabs(s);
    }
    if (norm > 1e-9) for (i = 0; i < TLEN; i++) table[i] /= norm;
}

/* ---- the home characters the macro walks between --------------------- */

static const double HOME[NCHAR][NG] = {
    {0.90, 0.00, 0.00, 0.20, 0.00}, /* 0 sine */
    {0.35, 0.00, 0.00, 0.20, 0.00}, /* 1 saw */
    {0.35, 1.00, 0.00, 0.20, 0.00}, /* 2 square */
    {0.15, 0.00, 0.20, 0.50, 0.20}, /* 3 pulse */
    {0.50, 0.30, 0.10, 0.20, 0.10}, /* 4 reese */
    {0.40, 0.20, 0.35, 0.25, 0.50}, /* 5 growl */
    {0.50, 0.10, 0.40, 0.12, 1.00}, /* 6 vocal */
    {0.15, 0.40, 0.75, 0.30, 0.60}  /* 7 metal */
};

/* ---- the macro: map 0..1 onto genome + whole voice chain (curated) --- */

typedef struct _voiceset {
    double gene[NG];
    double sub, detune, morph, fold, cross;
    double cutoff, res, drive;
    int    ch;                 /* which home character we are nearest */
} t_voiceset;

static inline void forage_macro(double t, t_voiceset *vs)
{
    /* discrete home timbres, walked in a musical order and crossfaded:
     * sine -> saw -> square -> reese -> growl -> metal.  We interpolate
     * the genome between adjacent homes so the table scans, not jumps. */
    static const int order[6] = { 0, 1, 2, 4, 5, 7 };
    int seg;
    double u, lo, hi, frac;
    int d;

    u   = t * 5.0;                 /* 0..5 across 6 waypoints */
    seg = (int)u;
    if (seg > 4) seg = 4;
    frac = u - seg;                /* 0..1 within the segment */
    lo = order[seg];
    hi = order[seg + 1];
    for (d = 0; d < NG; d++)
        vs->gene[d] = HOME[(int)lo][d] + frac * (HOME[(int)hi][d] - HOME[(int)lo][d]);

    /* the discrete "mode" we snap the retable to: nearest waypoint */
    vs->ch = (frac < 0.5) ? (int)lo : (int)hi;

    /* the rest of the chain climbs monotonically with brightness/energy */
    vs->sub    = 0.75 - 0.55 * t;              /* thick sub -> thin, upper takes over */
    vs->detune = 0.0 + 22.0 * t * t;           /* unison spread widens (the reese)   */
    vs->morph  = 0.05 + 0.75 * t;              /* phase distortion / brightness       */
    vs->fold   = (t < 0.35) ? 0.0 : (t - 0.35) / 0.65 * 0.7;  /* wavefolder up top   */
    vs->cross  = 80.0 + 140.0 * t;             /* sub owns less as it opens up         */
    vs->cutoff = 500.0 + 5500.0 * t * t;       /* filter opens                         */
    vs->res    = 0.10 + 0.45 * t;              /* resonance climbs                     */
    vs->drive  = (t < 0.5) ? 0.0 : (t - 0.5) / 0.5;  /* saturation only in the top half */
}

/* ------------------------------------------------------------------ */

static inline double table_read(const double *t, double phase)
{
    double f = phase - floor(phase);
    double xi = f * TLEN;
    int i0 = (int)xi;
    double frac = xi - i0;
    int i1 = (i0 + 1) & (TLEN - 1);
    i0 &= (TLEN - 1);
    return t[i0] + frac * (t[i1] - t[i0]);
}

static t_int *alien_forage_perform(t_int *w)
{
    t_alien_forage *x = (t_alien_forage *)(w[1]);
    t_sample *fin  = (t_sample *)(w[2]);
    t_sample *macin = (t_sample *)(w[3]);
    t_sample *out  = (t_sample *)(w[4]);
    int nsamp = (int)(w[5]);

    double sr = x->x_sr, sr_os = sr * OS;

    double t, tblock;
    double morph, fold, sub, cross, cutoff, res, drive;
    double hpa, detr, foldg;
    double a1, a2, a3;
    double g, k, fc;
    t_voiceset vs;
    int d;

    /* -------- ONCE PER BLOCK: read the macro's first sample only -------- */
    tblock = macin[0];
    /* wrap the macro into [0,100) then normalise to [0,1) */
    t = tblock - 100.0 * floor(tblock * 0.01);
    if (!(t >= 0.0 && t <= 100.0)) t = 0.0;   /* NaN/inf guard */
    t *= 0.01;
    if (t < 0.0) t = 0.0; else if (t > 0.999999) t = 0.999999;
    forage_macro(t, &vs);

    for (d = 0; d < NG; d++) x->x_gene[d] = vs.gene[d];

    /* Re-render the target table ONLY when the discrete mode changes, and
     * begin a short crossfade toward it (the wavescan).  This is the heavy
     * setup (TLEN*NPART sines) and MUST NOT run per-sample. */
    if (vs.ch != x->x_char) {
        x->x_char = vs.ch;
        gene_table(x->x_gene, x->x_target);
        x->x_xfade = 0.0;
        x->x_xfinc = 1.0 / (0.001 * 40.0 * sr);   /* 40 ms scan */
        if (!(x->x_xfinc > 0.0) || bad_double(x->x_xfinc)) x->x_xfinc = 0.01;
    }

    sub    = vs.sub;
    morph  = vs.morph;
    fold   = vs.fold;
    cross  = vs.cross;
    cutoff = vs.cutoff;
    res    = vs.res;
    drive  = vs.drive;
    if (drive < 0.0) drive = 0.0;
    foldg  = 1.0 + fold * 5.0;

    hpa  = exp(-TWOPI * (cross < 10.0 ? 10.0 : cross) / sr);
    detr = pow(2.0, vs.detune / 1200.0);
    if (bad_double(detr) || detr < 0.5) detr = 1.0;

    /* SVF coefficients: clamp cutoff into a valid range below Nyquist and
     * keep resonance below self-destruction so the filter cannot explode. */
    fc = cutoff;
    if (bad_double(fc)) fc = 500.0;
    if (fc < 20.0) fc = 20.0;
    if (fc > sr_os * 0.45) fc = sr_os * 0.45;
    if (!(res >= 0.0)) res = 0.0;
    if (res > 0.9) res = 0.9;                 /* headroom before self-osc  */
    g = tan(M_PI * fc / sr_os);
    if (bad_double(g) || g <= 0.0) g = 0.01;
    if (g > 8.0) g = 8.0;
    k = 2.0 - 2.0 * res;                      /* damping, k >= 0.2         */
    a1 = 1.0 / (1.0 + g * (g + k));
    a2 = g * a1;
    a3 = g * a2;

    /* -------------------------- PER SAMPLE ------------------------------ */
    while (nsamp--) {
        double ftarget = *fin++;
        double y, subout, decsum = 0.0;
        double xf;
        int os, v;

        (void)macin;

        if (bad_double(ftarget)) ftarget = 55.0;
        if (ftarget < 0.01)   ftarget = 0.01;     /* forbid divide-by-zero */
        if (ftarget > 8000.0) ftarget = 8000.0;
        x->x_freq = ftarget;                /* immediate pitch tracking */

        x->x_subph += x->x_freq / sr;
        if (x->x_subph >= 1.0) x->x_subph -= 1.0;
        subout = sin(TWOPI * x->x_subph);

        xf = x->x_xfade;
        for (os = 0; os < OS; os++) {
            double u = 0.0, warp, s, hp, v1, v2, v3;
            for (v = 0; v < NVOICE; v++) {
                double det = (v == 0) ? 1.0 : (v == 1 ? detr : 1.0 / detr);
                double a, b, rd;
                x->x_ph[v] += x->x_freq * det / sr_os;
                if (x->x_ph[v] >= 1.0) x->x_ph[v] -= 1.0;
                else if (x->x_ph[v] < 0.0) x->x_ph[v] = 0.0;
                warp = x->x_ph[v] + morph * 0.5 * sin(TWOPI * x->x_ph[v]);
                a = table_read(x->x_table, warp);
                if (xf < 1.0) {                 /* wavescan crossfade */
                    b = table_read(x->x_target, warp);
                    rd = a + xf * (b - a);
                } else rd = a;
                u += rd;
            }
            u /= NVOICE;

            /* clamp the fold input so the wavefolder cannot explode */
            if (u >  4.0) u =  4.0;
            else if (u < -4.0) u = -4.0;
            s = sin(0.5 * M_PI * foldg * u);              /* wavefolder */

            hp = hpa * (x->x_hp_y1 + s - x->x_hp_x1);     /* crossover HP */
            x->x_hp_x1 = s; x->x_hp_y1 = flushd(hp);

            v3 = hp - x->x_ic2;                            /* SVF lowpass */
            v1 = a1 * x->x_ic1 + a2 * v3;
            v2 = x->x_ic2 + a2 * x->x_ic1 + a3 * v3;
            x->x_ic1 = 2.0 * v1 - x->x_ic1;
            x->x_ic2 = 2.0 * v2 - x->x_ic2;
            /* clamp + flush filter state every sample: no blow-up */
            x->x_ic1 = flushd(x->x_ic1);
            x->x_ic2 = flushd(x->x_ic2);
            if (x->x_ic1 >  4.0) x->x_ic1 =  4.0; else if (x->x_ic1 < -4.0) x->x_ic1 = -4.0;
            if (x->x_ic2 >  4.0) x->x_ic2 =  4.0; else if (x->x_ic2 < -4.0) x->x_ic2 = -4.0;

            x->x_dec += 0.5 * (v2 - x->x_dec);
            x->x_dec = flushd(x->x_dec);
            decsum += x->x_dec;

            if (xf < 1.0) { xf += x->x_xfinc; if (xf >= 1.0) xf = 1.0; }
        }

        /* commit the crossfade once complete: target becomes current */
        if (x->x_xfade < 1.0 && xf >= 1.0)
            memcpy(x->x_table, x->x_target, sizeof(x->x_table));
        x->x_xfade = xf;

        y = decsum / OS + sub * subout;
        if (drive > 0.0)
            y = softclip(y * (1.0 + drive * 6.0)) / (1.0 + drive * 1.5);
        else
            y = softclip(y);
        y *= (0.8 * 0.70);
        /* final finite + bounded guarantee */
        if (bad_double(y)) y = 0.0;
        if (y >  1.5) y =  1.5;
        else if (y < -1.5) y = -1.5;
        *out++ = (t_sample)y;
    }

    /* brightness readout for visuals: throttled to ONCE PER BLOCK */
    outlet_float(x->x_nvout, (t_float)t);
    return (w + 6);
}

static void alien_forage_dsp(t_alien_forage *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    dsp_add(alien_forage_perform, 5, x,
            sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_forage_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_forage *x = (t_alien_forage *)pd_new(alien_forage_tilde_class);
    int v, d;
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    for (v = 0; v < NVOICE; v++) x->x_ph[v] = (double)v * 0.0137;
    x->x_subph = 0.0; x->x_freq = 55.0;
    x->x_ic1 = x->x_ic2 = 0.0;
    x->x_hp_x1 = x->x_hp_y1 = x->x_dec = 0.0;
    x->x_xfade = 1.0; x->x_xfinc = 0.01;
    x->x_char = -1;                      /* force a table render on first block */

    /* prime the genome/table at the dark end of the macro */
    for (d = 0; d < NG; d++) x->x_gene[d] = HOME[0][d];
    gene_table(x->x_gene, x->x_table);
    memcpy(x->x_target, x->x_table, sizeof(x->x_table));

    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);   /* macro */
    outlet_new(&x->x_obj, &s_signal);
    x->x_nvout = outlet_new(&x->x_obj, &s_float);
    return (void *)x;
}

void alien_forage_tilde_setup(void)
{
    alien_forage_tilde_class = class_new(gensym("alien_forage~"),
        (t_newmethod)alien_forage_new, 0, sizeof(t_alien_forage),
        CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_forage_tilde_class, t_alien_forage, x_f);
    class_addmethod(alien_forage_tilde_class, (t_method)alien_forage_dsp,
                    gensym("dsp"), A_CANT, 0);
}
