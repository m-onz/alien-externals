/* alien_spectra~.c  --  spectral-frame synthesis (build sound in the spectrum)
 *
 * Every other object here builds a waveform sample by sample.  This one builds
 * a SPECTRUM -- a magnitude and phase for each frequency bin -- and inverse-
 * FFTs it, frame by frame, with overlap-add.  Working in the frequency domain
 * makes sounds the time domain cannot: frozen shimmer, spectra that mutate
 * like a cellular automaton, and whispering clouds where the magnitudes are
 * pitched but the phases are random.
 *
 * The pitch inlet places the spectral energy (harmonics of f0); the MACRO
 * decides how that spectrum behaves from frame to frame.  A self-contained
 * radix-2 FFT, 1024-point, 4x overlap, Hann-windowed.
 *
 *   inlet 0   signal/float   pitch in Hz (where the spectral energy sits)
 *   inlet 1   signal/float   MACRO 0..100 (wrapped mod 100) -- one knob that
 *                            morphs the whole voice.  It walks through the five
 *                            spectral behaviours in order -- harmonic clean tone,
 *                            metallic inharmonic partials, a slowly mutating
 *                            spectrum, a whispering choir of random phases, and
 *                            sparse glass bells -- while at the same time it
 *                            tilts the spectrum from dark to bright, adds
 *                            partials, smear, drift and drive.  At 0 it is a
 *                            dark, few-partial harmonic tone; at 100 it is a
 *                            bright, dense, fast-mutating driven spray of bells.
 *
 *   outlet 0  signal
 *
 * build:
 *   gcc -shared -fPIC -O2 -o alien_spectra~.pd_linux alien_spectra~.c -lm
 *   (macOS: -bundle -undefined dynamic_lookup -o alien_spectra~.pd_darwin)
 */

#include "m_pd.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TWOPI (2.0 * M_PI)
#define NFFT  1024
#define NHOP  256
#define NBIN  (NFFT/2 + 1)
#define NMODE 5

static t_class *alien_spectra_tilde_class;

typedef struct _alien_spectra {
    t_object x_obj;
    t_float  x_f;

    double   x_sr;
    double   x_re[NFFT], x_im[NFFT];
    double   x_mag[NBIN], x_phase[NBIN];
    double   x_win[NFFT];
    double   x_ola[NFFT];
    int      x_olapos;
    int      x_count;
    int      x_mode;
    int      x_seeded;

    double   x_freq;
    double   x_morph, x_tilt, x_spread, x_stretch, x_drive;
    int      x_partials;

    unsigned x_rng;
} t_alien_spectra;

static unsigned rn(t_alien_spectra *x)
{
    x->x_rng ^= x->x_rng << 13; x->x_rng ^= x->x_rng >> 17;
    x->x_rng ^= x->x_rng << 5;  return x->x_rng;
}
static double frnd(t_alien_spectra *x){ return (double)(rn(x)&0xffffff)/(double)0x1000000; }

static inline double softclip(double v)
{
    if (v >  3.0) return 1.0;
    if (v < -3.0) return -1.0;
    return v * (27.0 + v*v) / (27.0 + 9.0*v*v);
}

/* iterative radix-2 FFT; dir=-1 forward, +1 inverse (unnormalised) */
static void fft(double *re, double *im, int n, int dir)
{
    int i, j, k, m, step;
    double c, s, tr, ti, ang, wr, wi, wtr;
    for (i = 1, j = 0; i < n; i++) {          /* bit reversal */
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { tr=re[i];re[i]=re[j];re[j]=tr; ti=im[i];im[i]=im[j];im[j]=ti; }
    }
    for (step = 1; step < n; step <<= 1) {     /* butterflies */
        m = step << 1;
        ang = dir * M_PI / step;
        wr = cos(ang); wi = sin(ang);
        for (k = 0; k < n; k += m) {
            c = 1.0; s = 0.0;
            for (j = 0; j < step; j++) {
                int a = k + j, b = k + j + step;
                tr = c*re[b] - s*im[b];
                ti = c*im[b] + s*re[b];
                re[b] = re[a] - tr; im[b] = im[a] - ti;
                re[a] += tr;        im[a] += ti;
                wtr = c*wr - s*wi; s = c*wi + s*wr; c = wtr;
            }
        }
    }
}

static void spectra_build(t_alien_spectra *x, double morph)
{
    int k, h, np = x->x_partials, bin;
    double sr = x->x_sr, f0 = x->x_freq;
    double phinc;

    /* Hard bound on partial count: never index or iterate beyond the array.
     * A given harmonic can only land in a real bin, so capping partials to
     * NBIN also caps the synthesis work per frame (CPU + bounds safety). */
    if (np < 1)      np = 1;
    if (np > NBIN-1) np = NBIN-1;
    if (sr < 1.0)    sr = 44100.0;                 /* div-by-zero guard */
    if (f0 < 0.01)   f0 = 0.01;

    if (x->x_mode == 2) {                      /* mutate: drift existing mags */
        if (!x->x_seeded) {
            for (k = 0; k < NBIN; k++) x->x_mag[k] = 0.0;
            for (h = 1; h <= np; h++) {
                bin = (int)(h * f0 * NFFT / sr + 0.5);
                if (bin > 0 && bin < NBIN) x->x_mag[bin] += pow(1.0/h, x->x_tilt);
            }
            x->x_seeded = 1;
        }
        for (k = 1; k < NBIN; k++) {
            x->x_mag[k] += morph * 0.3 * (frnd(x) - 0.5) * (0.02 + x->x_mag[k]);
            if (x->x_mag[k] < 0) x->x_mag[k] = 0;
            if ((rn(x) & 0x3ff) < (unsigned)(morph * 20)) /* occasional new bin */
                x->x_mag[k] += 0.3 * frnd(x);
        }
    } else if (x->x_mode == 4) {               /* sparse: random bins */
        int base = (int)(f0 * NFFT / sr);
        for (k = 0; k < NBIN; k++) x->x_mag[k] *= 0.9;      /* let old ring out */
        for (h = 0; h < np; h++) {
            if (frnd(x) < 0.3 + 0.7 * morph) {
                bin = base + (int)(frnd(x) * (NBIN - 4));
                if (NBIN > 2) bin = (bin % (NBIN - 2)) + 1; /* -> [1, NBIN-2]  */
                else          bin = 1;
                if (bin < 1)      bin = 1;                  /* belt & braces   */
                if (bin > NBIN-1) bin = NBIN-1;
                x->x_mag[bin] += 0.5 + 0.5 * frnd(x);
            }
        }
    } else {                                   /* harmonic / inharmonic / whisper */
        double st = (x->x_mode == 1) ? x->x_stretch : 1.0;
        int sp = 1 + (int)(x->x_spread * 4.0);
        if (sp < 1)  sp = 1;
        if (sp > 16) sp = 16;
        for (k = 0; k < NBIN; k++) x->x_mag[k] = 0.0;
        for (h = 1; h <= np; h++) {
            double fr = f0 * pow((double)h, st);
            double a = pow(1.0/h, x->x_tilt);
            int c = (int)(fr * NFFT / sr + 0.5), d;
            if (c <= 0 || c >= NBIN) continue;
            for (d = -sp+1; d < sp; d++) {                  /* smear over bins */
                int bb = c + d;
                if (bb > 0 && bb < NBIN)
                    x->x_mag[bb] += a * (1.0 - fabs((double)d)/sp);
            }
        }
    }

    /* clamp spectral magnitudes: mutate mode accumulates without a ceiling and
     * would otherwise ramp toward Inf and blow up the inverse FFT.  Also flush
     * denormals / NaN so the state can never poison the frame. */
    for (k = 0; k < NBIN; k++) {
        double m = x->x_mag[k];
        if (!isfinite(m) || fabs(m) < 1e-20) m = 0.0;
        if (m < 0.0)  m = 0.0;
        if (m > 32.0) m = 32.0;
        x->x_mag[k] = m;
    }

    /* phases: coherent (advance per hop) except whisper = random.
     * fmod keeps the running phase bounded no matter how large phinc*k is. */
    phinc = TWOPI * (double)NHOP / (double)NFFT;
    for (k = 0; k < NBIN; k++) {
        if (x->x_mode == 3) x->x_phase[k] = frnd(x) * TWOPI;   /* whisper */
        else {
            double ph = x->x_phase[k] + phinc * k;
            ph = fmod(ph, TWOPI);
            if (!isfinite(ph)) ph = 0.0;
            x->x_phase[k] = ph;
        }
    }
}

static void spectra_frame(t_alien_spectra *x)   /* build spectrum -> time frame in x_re */
{
    int k;
    double mo = x->x_morph;
    if (!isfinite(mo)) mo = 0.0;
    if (mo < 0.0) mo = 0.0;
    if (mo > 1.0) mo = 1.0;
    spectra_build(x, mo);
    for (k = 0; k < NFFT; k++) { x->x_re[k] = 0.0; x->x_im[k] = 0.0; }
    for (k = 0; k < NBIN; k++) {
        double m = x->x_mag[k];
        x->x_re[k] = m * cos(x->x_phase[k]);
        x->x_im[k] = m * sin(x->x_phase[k]);
        if (k > 0 && k < NFFT/2) {                 /* hermitian symmetry */
            x->x_re[NFFT-k] =  x->x_re[k];
            x->x_im[NFFT-k] = -x->x_im[k];
        }
    }
    fft(x->x_re, x->x_im, NFFT, +1);               /* inverse */

    /* normalise the frame to constant RMS so every mode sits at one level */
    {
        double e = 0.0, g; int i;
        for (i = 0; i < NFFT; i++) e += x->x_re[i] * x->x_re[i];
        e = sqrt(e / NFFT);
        g = (isfinite(e) && e > 1e-9) ? 0.5 / e : 0.0;
        for (i = 0; i < NFFT; i++) {
            double v = x->x_re[i] * g;
            if (!isfinite(v)) v = 0.0;              /* never feed OLA a NaN/Inf */
            x->x_re[i] = v;
        }
    }
}

/* ------------------------------------------------------------------ */

/* map the 0..1 macro onto the whole parameter set (curated morph).
 * t is split into NMODE equal sub-ranges that select the spectral mode;
 * the continuous params sweep monotonically across the FULL 0..1 range so a
 * single knob goes dark/sparse/mellow -> bright/dense/driven. */
static inline void spectra_macro(double t, int *mode, double *morph,
                                 double *tilt, double *spread,
                                 double *stretch, int *partials, double *drive)
{
    int m = (int)(t * NMODE);           /* 0..NMODE-1 */
    if (m < 0) m = 0;
    if (m >= NMODE) m = NMODE - 1;
    *mode = m;

    *tilt     = 1.6 - 1.7 * t;                     /* dark rolloff -> bright/flat */
    *spread   = 0.02 + 0.9 * t;                    /* tight -> smeared            */
    *stretch  = 1.0 + 0.9 * t;                     /* harmonic -> stretched metal */
    *partials = (int)(6.0 + 52.0 * t);             /* few -> many partials        */
    if (*partials < 1)  *partials = 1;
    if (*partials > 64) *partials = 64;
    *morph    = t * t;                             /* still -> fast mutation/regen*/
    *drive    = (t < 0.6) ? 0.0 : (t - 0.6) / 0.4; /* grit only up top            */
}

static t_int *alien_spectra_perform(t_int *w)
{
    t_alien_spectra *x = (t_alien_spectra *)(w[1]);
    t_sample *fin = (t_sample *)(w[2]);
    t_sample *min = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    int nsamp = (int)(w[5]);

    /* --- derive the DISCRETE mode + all morph params ONCE PER BLOCK ---------
     * The macro is a control-rate quantity; running the full parameter map and
     * (worse) resetting the mode/seed every sample was the stutter source.  We
     * read the macro's first sample, clamp it, and rebuild only on change. */
    {
        double mac = (double)min[0];
        double t = mac;
        int mode;

        if (!isfinite(t)) t = 0.0;                 /* NaN/inf guard */
        t = t - 100.0 * floor(t * 0.01);           /* wrap into [0,100)         */
        if (!(t >= 0.0 && t <= 100.0)) t = 0.0;
        t *= 0.01;                                 /* normalise to [0,1)        */

        spectra_macro(t, &mode, &x->x_morph, &x->x_tilt, &x->x_spread,
                      &x->x_stretch, &x->x_partials, &x->x_drive);
        if (mode != x->x_mode) { x->x_mode = mode; x->x_seeded = 0; }
    }

    /* pitch: read once per block; clamp away from zero to forbid div-by-zero */
    {
        double f0 = (double)fin[0];
        if (!isfinite(f0)) f0 = 110.0;
        if (f0 < 0.01)    f0 = 0.01;
        if (f0 > 5000.0)  f0 = 5000.0;
        x->x_freq = f0;
    }

    while (nsamp--) {
        double o;

        o = x->x_ola[x->x_olapos];
        x->x_ola[x->x_olapos] = 0.0;
        x->x_olapos = (x->x_olapos + 1) & (NFFT - 1);

        if (--x->x_count <= 0) {
            int i, p;
            x->x_count = NHOP;
            spectra_frame(x);
            p = x->x_olapos;
            for (i = 0; i < NFFT; i++) {
                x->x_ola[(p + i) & (NFFT - 1)] += x->x_re[i] * x->x_win[i];
            }
        }

        o *= 0.6;                                  /* OLA overlap compensation */
        if (x->x_drive > 0.0)
            o = softclip(o * (1.0 + x->x_drive * 6.0)) / (1.0 + x->x_drive * 1.5);
        else o = softclip(o);

        /* final: guarantee finite + bounded output */
        if (!isfinite(o)) o = 0.0;
        o *= (0.8 * 0.70);
        if (o >  1.5) o =  1.5;                     /* hard safety rail         */
        if (o < -1.5) o = -1.5;
        *out++ = (t_sample)o;
    }
    return (w + 6);
}

static void alien_spectra_dsp(t_alien_spectra *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    dsp_add(alien_spectra_perform, 5, x,
            sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_spectra_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_spectra *x = (t_alien_spectra *)pd_new(alien_spectra_tilde_class);
    int i;
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    for (i = 0; i < NFFT; i++) {
        x->x_ola[i] = 0.0;
        x->x_win[i] = 0.5 - 0.5 * cos(TWOPI * i / (NFFT - 1));   /* Hann */
    }
    for (i = 0; i < NBIN; i++) { x->x_mag[i] = 0.0; x->x_phase[i] = 0.0; }
    x->x_olapos = 0; x->x_count = 1; x->x_mode = 0; x->x_seeded = 0;
    x->x_freq = 110.0;
    x->x_morph = 0.2; x->x_tilt = 1.0; x->x_spread = 0.1;
    x->x_stretch = 1.2; x->x_drive = 0.1; x->x_partials = 24;
    x->x_rng = 71777u;

    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);   /* macro */
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_spectra_tilde_setup(void)
{
    alien_spectra_tilde_class = class_new(gensym("alien_spectra~"),
        (t_newmethod)alien_spectra_new, 0, sizeof(t_alien_spectra),
        CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_spectra_tilde_class, t_alien_spectra, x_f);
    class_addmethod(alien_spectra_tilde_class, (t_method)alien_spectra_dsp, gensym("dsp"), A_CANT, 0);
}
