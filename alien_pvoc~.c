/* alien_pvoc~.c  --  streaming phase-vocoder spectral synthesis (DRONE voice)
 *
 * Most spectral objects here BUILD a spectrum from scratch.  This one runs a
 * true STREAMING short-time Fourier transform of an internal rich source and
 * manipulates the *analysed* spectrum before resynthesis -- the phase-vocoder
 * avenue proper.  An internal band-limited saw/pulse (plus a whisper of noise)
 * at f0 feeds an input ring; every hop we window the last N samples, forward
 * FFT, and reshape the live spectrum:
 *
 *     FREEZE     capture a frame and crossfade the live magnitudes/phases
 *                toward it, so the sound sustains as a spectral drone
 *     BLUR       one-pole temporal smoothing of the magnitude spectrum (smear)
 *     SHIFT/     remap bins by a factor -- spectral pitch/formant/inharmonic
 *     STRETCH    shift that a normal oscillator cannot make
 *     TILT/GATE  brightness tilt and a soft magnitude gate
 *     PHASE      proper phase-vocoder phase advance for frozen/held partials,
 *                blended toward randomised phase for glassy shimmer
 *
 * then inverse FFT, window again, overlap-add into an output ring.  A slow
 * al_wander drifts the shift and blur so a held note keeps evolving.
 *
 *   inlet 0   signal/float   fundamental in Hz (drives the internal source)
 *   inlet 1   signal/float   MACRO 0..100 (wrapped mod 100) -- one knob that
 *                            morphs the voice: 0 = near-clean rich tone,
 *                            rising through blurred / shifted spectral wash,
 *                            to a frozen glassy inharmonic pad at 100.
 *
 *   outlet 0  signal
 *
 * build:
 *   gcc -shared -fPIC -O2 -o alien_pvoc~.pd_linux alien_pvoc~.c -lm
 *   (macOS: -bundle -undefined dynamic_lookup -o alien_pvoc~.pd_darwin)
 */

#include "m_pd.h"
#include "alien.h"
#include <math.h>
#include <string.h>

#define NFFT  1024
#define NHOP  256
#define NBIN  (NFFT/2 + 1)

static t_class *alien_pvoc_tilde_class;

typedef struct _alien_pvoc {
    t_object x_obj;
    t_float  x_f;

    double   x_sr;

    /* internal source */
    al_phasor x_src;        /* saw/pulse phasor at f0            */
    al_rng    x_rng;        /* noise + phase shimmer             */
    al_wander x_wander;     /* slow life for shift / blur        */

    /* streaming STFT rings */
    double   x_in[NFFT];    /* input ring (internal source)      */
    int      x_inpos;
    double   x_ola[NFFT];   /* output overlap-add ring           */
    int      x_olapos;
    int      x_count;       /* hop countdown                     */

    /* work + windows */
    double   x_re[NFFT], x_im[NFFT];
    double   x_win[NFFT];   /* Hann (analysis == synthesis)      */

    /* spectral state carried across hops */
    double   x_smag[NBIN];  /* blurred (smoothed) magnitude      */
    double   x_ph[NBIN];    /* running resynthesis phase (PV)    */
    double   x_frzmag[NBIN];/* frozen magnitude snapshot         */
    double   x_frzph[NBIN]; /* frozen phase snapshot             */
    int      x_haveFrz;

    double   x_olanorm;     /* overlap-add + FFT normalisation   */

    al_dc    x_dc;
} t_alien_pvoc;

/* ------------------------------------------------------------------ */

/* map the 0..1 macro onto the whole spectral parameter set (curated).
 * Everything sweeps monotonically so one knob goes clean -> wash -> frozen. */
typedef struct {
    double blur;      /* 0 = live, 1 = fully smeared          */
    double freeze;    /* 0 = live, 1 = fully frozen           */
    double shift;     /* bin remap factor (1 = identity)      */
    double tilt;      /* < 0 dark, > 0 bright                 */
    double gate;      /* magnitude gate threshold             */
    double shimmer;   /* 0 = coherent PV phase, 1 = random    */
    double drive;     /* only up top                          */
} pvoc_params;

static inline void pvoc_macro(double t, pvoc_params *p)
{
    /* blur comes in first, freeze latches in over the top half */
    p->blur    = t * t;                              /* live -> smeared      */
    p->freeze  = (t < 0.45) ? 0.0 : (t - 0.45) / 0.55;
    if (p->freeze > 1.0) p->freeze = 1.0;
    /* spectral shift: gently below then above unity for inharmonic drift    */
    p->shift   = 1.0 + 0.55 * (t - 0.35);            /* ~0.81 .. 1.36        */
    p->tilt    = 0.2 - 1.3 * t;                      /* mellow -> darker     */
    p->gate    = 0.02 + 0.18 * t;                    /* thin the wash up top */
    p->shimmer = (t < 0.5) ? 0.0 : (t - 0.5) / 0.5;  /* glassy at the top    */
    p->drive   = (t < 0.7) ? 0.0 : (t - 0.7) / 0.3;
}

/* finite-guard helper */
static inline double fin(double s) {
    if (!(s > -1e6 && s < 1e6)) return 0.0;
    return s;
}

/* one hop: window input ring -> FFT -> reshape -> IFFT -> OLA */
static void pvoc_hop(t_alien_pvoc *x, const pvoc_params *p)
{
    int k, i;
    /* live drift from the wander brain -- keeps a held note breathing */
    double drift  = al_wander_bip(&x->x_wander, 0);   /* -1..1 */
    double drift2 = al_wander_bip(&x->x_wander, 1);
    double drift3 = al_wander_bip(&x->x_wander, 2);
    double shift  = p->shift * (1.0 + 0.12 * drift);  /* spectral shift wobble */
    double blur   = al_clamp(p->blur + 0.15 * drift2, 0.0, 0.995);
    double tiltmod = p->tilt + 0.9 * drift3;          /* brightness breathes   */
    double expinc = AL_2PI * (double)NHOP / (double)NFFT; /* per-hop, per-bin */

    if (shift < 0.25) shift = 0.25;
    if (shift > 4.0)  shift = 4.0;

    /* --- gather the last N windowed samples from the input ring --- */
    {
        int rp = x->x_inpos;                 /* oldest sample position */
        for (i = 0; i < NFFT; i++) {
            int idx = (rp + i) & (NFFT - 1);
            x->x_re[i] = x->x_in[idx] * x->x_win[i];
            x->x_im[i] = 0.0;
        }
    }

    al_fft(x->x_re, x->x_im, NFFT, -1);      /* forward, unnormalised */

    /* --- reshape the spectrum, bin by bin, into a remapped target --- */
    {
        static double tmag[NBIN], tph[NBIN];
        for (k = 0; k < NBIN; k++) {
            /* magnitude/phase of the LIVE analysed bin */
            double re = x->x_re[k], im = x->x_im[k];
            double mag = sqrt(re*re + im*im);
            double phs = atan2(im, re);
            if (!(mag >= 0.0 && mag < 1e9)) mag = 0.0;
            if (!isfinite(phs)) phs = 0.0;

            /* temporal blur (one-pole smear across hops) */
            x->x_smag[k] = blur * x->x_smag[k] + (1.0 - blur) * mag;
            if (!(x->x_smag[k] >= 0.0 && x->x_smag[k] < 1e9)) x->x_smag[k] = 0.0;

            /* brightness tilt: dark rolloff or bright boost */
            {
                double bn = (double)k / (double)(NBIN - 1);
                double g = pow(2.0, tiltmod * (bn - 0.35));  /* ~unity mid */
                if (!(g > 0.0 && g < 32.0)) g = 1.0;
                tmag[k] = x->x_smag[k] * g;
            }
            tph[k] = phs;
        }

        /* soft magnitude gate (thins the pad, keeps it glassy) */
        {
            double thr = p->gate;
            for (k = 0; k < NBIN; k++) {
                double m = tmag[k];
                if (m < thr) m *= (m / (thr + 1e-9));  /* soft knee */
                tmag[k] = m;
            }
        }

        /* --- spectral SHIFT / STRETCH: remap source bin -> target bin --- */
        {
            static double rmag[NBIN], rph[NBIN];
            for (k = 0; k < NBIN; k++) { rmag[k] = 0.0; rph[k] = 0.0; }
            for (k = 0; k < NBIN; k++) {
                double srcf = (double)k / shift;      /* where target k reads from */
                int s0 = (int)srcf;
                double fr = srcf - s0;
                int s1 = s0 + 1;
                if (s0 < 0) s0 = 0; if (s0 > NBIN - 1) s0 = NBIN - 1;
                if (s1 < 0) s1 = 0; if (s1 > NBIN - 1) s1 = NBIN - 1;
                rmag[k] = al_lerp(tmag[s0], tmag[s1], fr);
                rph[k]  = tph[s0];
                if (!(rmag[k] >= 0.0 && rmag[k] < 1e9)) rmag[k] = 0.0;
            }
            memcpy(tmag, rmag, sizeof(tmag));
            memcpy(tph,  rph,  sizeof(tph));
        }

        /* --- FREEZE: capture once, crossfade toward the held snapshot --- */
        if (p->freeze > 0.0 && !x->x_haveFrz) {
            memcpy(x->x_frzmag, tmag, sizeof(tmag));
            memcpy(x->x_frzph,  tph,  sizeof(tph));
            for (k = 0; k < NBIN; k++) x->x_ph[k] = tph[k];
            x->x_haveFrz = 1;
        }
        if (p->freeze <= 0.0) x->x_haveFrz = 0;   /* re-arm when released */

        {
            double fz = al_clamp(p->freeze, 0.0, 1.0);
            for (k = 0; k < NBIN; k++) {
                /* slowly leak the drifting live spectrum into the frozen
                 * snapshot so even a fully frozen pad keeps evolving */
                x->x_frzmag[k] += 0.02 * (tmag[k] - x->x_frzmag[k]);
                {
                    double m = al_lerp(tmag[k], x->x_frzmag[k], fz);
                    if (!(m >= 0.0 && m < 1e9)) m = 0.0;
                    if (m > 64.0) m = 64.0;
                    tmag[k] = m;
                }
            }
        }

        /* --- PHASE: phase-vocoder advance blended with live/random ---
         * For the frozen/held part we drive a proper per-hop phase advance
         * (expected increment for the bin) so there is no buzzy framing;
         * we crossfade that toward the live analysed phase (freeze = 0) and
         * toward randomised phase (shimmer) for glass. */
        {
            double fz = al_clamp(p->freeze, 0.0, 1.0);
            double shim = al_clamp(p->shimmer, 0.0, 1.0);
            for (k = 0; k < NBIN; k++) {
                double adv = x->x_ph[k] + expinc * (double)k;
                double rndp = al_rand(&x->x_rng) * AL_2PI;
                double liveph = tph[k];
                /* held phase = coherent PV advance; released = live analysis */
                double basep = al_lerp(liveph, adv, fz);
                /* shimmer nudges toward random for a glassy top */
                double outp = al_lerp(basep, rndp, shim * 0.6 * fz);
                outp = fmod(outp, AL_2PI);
                if (!isfinite(outp)) outp = 0.0;
                x->x_ph[k] = fmod(adv, AL_2PI);   /* keep PV accumulator alive */
                if (!isfinite(x->x_ph[k])) x->x_ph[k] = 0.0;

                /* assemble the complex spectrum with hermitian symmetry */
                {
                    double m = tmag[k];
                    x->x_re[k] = m * cos(outp);
                    x->x_im[k] = m * sin(outp);
                    if (k > 0 && k < NFFT/2) {
                        x->x_re[NFFT-k] =  x->x_re[k];
                        x->x_im[NFFT-k] = -x->x_im[k];
                    }
                }
            }
        }
    }

    al_fft(x->x_re, x->x_im, NFFT, +1);      /* inverse, unnormalised */

    /* window again + overlap-add into the output ring, normalised */
    {
        int p0 = x->x_olapos;
        double g = x->x_olanorm;
        for (i = 0; i < NFFT; i++) {
            double v = fin(x->x_re[i] * x->x_win[i] * g);
            x->x_ola[(p0 + i) & (NFFT - 1)] += v;
        }
    }
}

/* ------------------------------------------------------------------ */

static t_int *alien_pvoc_perform(t_int *w)
{
    t_alien_pvoc *x = (t_alien_pvoc *)(w[1]);
    t_sample *fin_in = (t_sample *)(w[2]);
    t_sample *min = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    int nsamp = (int)(w[5]);

    double sr = x->x_sr;
    pvoc_params p;

    al_denorm_off();

    /* macro + f0 read once per block (control rate) */
    {
        double mac = (double)min[0];
        double t;
        if (!isfinite(mac)) mac = 0.0;
        t = mac - 100.0 * floor(mac * 0.01);
        if (!(t >= 0.0 && t <= 100.0)) t = 0.0;
        t *= 0.01;
        pvoc_macro(t, &p);
    }

    {
        double f0 = (double)fin_in[0];
        if (!isfinite(f0)) f0 = 110.0;
        if (f0 < 8.0)          f0 = 8.0;
        if (f0 > 0.45 * sr)    f0 = 0.45 * sr;
        x->x_f = (t_float)f0;   /* stash the clamped fundamental */
    }

    while (nsamp--) {
        double f0 = (double)x->x_f;
        double dt = f0 / sr;
        double ph, saw, pul, src, o;

        /* --- internal rich source: band-limited saw + pulse + a little noise --- */
        ph  = al_phasor_tick(&x->x_src, f0, sr);
        saw = al_saw_bl(ph, dt);
        pul = al_squ_bl(ph, dt);
        src = 0.7 * saw + 0.3 * pul + 0.02 * al_randb(&x->x_rng);
        src = al_clamp(src, -1.5, 1.5);

        /* write source into input ring */
        x->x_in[x->x_inpos] = src;
        x->x_inpos = (x->x_inpos + 1) & (NFFT - 1);

        /* read (and clear) the output ring */
        o = x->x_ola[x->x_olapos];
        x->x_ola[x->x_olapos] = 0.0;
        x->x_olapos = (x->x_olapos + 1) & (NFFT - 1);

        /* every hop, run one STFT frame */
        if (--x->x_count <= 0) {
            x->x_count = NHOP;
            al_wander_tick(&x->x_wander, &x->x_rng, 2.5, sr);
            pvoc_hop(x, &p);
        }

        /* --- output: DC-block, drive, softclip, bound --- */
        o = fin(o);
        o = al_dc_tick(&x->x_dc, o);
        if (p.drive > 0.0)
            o = al_softclip(o * (1.0 + p.drive * 4.0)) / (1.0 + p.drive * 1.5);
        else
            o = al_softclip(o);
        o *= 1.45;                       /* ~ -12 dBFS trim */
        if (!isfinite(o)) o = 0.0;
        if (o >  1.5) o =  1.5;
        if (o < -1.5) o = -1.5;
        *out++ = (t_sample)o;
    }
    return (w + 6);
}

static void alien_pvoc_dsp(t_alien_pvoc *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    if (x->x_sr < 1000.0) x->x_sr = 44100.0;
    dsp_add(alien_pvoc_perform, 5, x,
            sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_pvoc_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_pvoc *x = (t_alien_pvoc *)pd_new(alien_pvoc_tilde_class);
    int i;
    (void)s; (void)argc; (void)argv;

    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;

    al_phasor_init(&x->x_src);
    al_rng_seed(&x->x_rng, 0x51a9e001u);
    al_wander_init(&x->x_wander, &x->x_rng, 4);
    al_dc_init(&x->x_dc);

    al_hann(x->x_win, NFFT);

    for (i = 0; i < NFFT; i++) { x->x_in[i] = 0.0; x->x_ola[i] = 0.0; }
    for (i = 0; i < NBIN; i++) {
        x->x_smag[i] = 0.0; x->x_ph[i] = 0.0;
        x->x_frzmag[i] = 0.0; x->x_frzph[i] = 0.0;
    }
    x->x_inpos = 0; x->x_olapos = 0; x->x_count = 1; x->x_haveFrz = 0;

    /* OLA normalisation: al_fft's inverse is unnormalised (multiplies by N),
     * and a Hann^2 window with 75% overlap sums to ~1.5.  Divide by both so
     * the resynthesised level stays ~unity regardless of overlap. */
    x->x_olanorm = 1.0 / ((double)NFFT * 1.5);

    x->x_f = 110.0;

    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);   /* macro */
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_pvoc_tilde_setup(void)
{
    alien_pvoc_tilde_class = class_new(gensym("alien_pvoc~"),
        (t_newmethod)alien_pvoc_new, 0, sizeof(t_alien_pvoc),
        CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_pvoc_tilde_class, t_alien_pvoc, x_f);
    class_addmethod(alien_pvoc_tilde_class, (t_method)alien_pvoc_dsp,
                    gensym("dsp"), A_CANT, 0);
}
