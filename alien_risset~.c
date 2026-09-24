/* alien_risset~.c  --  Shepard-Risset endless glissando drone
 *
 * Jean-Claude Risset's auditory illusion: a stack of sine partials spaced
 * an OCTAVE apart, all sitting under a single FIXED bell-shaped spectral
 * envelope over log-frequency (loud in the middle, fading at the extremes).
 * Slide every partial up (or down) together in log-frequency and the pitch
 * seems to rise (or fall) FOREVER -- as a partial climbs past the top of
 * the bell it fades out while a fresh one fades in at the bottom, so the
 * spectrum never actually changes. The "impossible" endlessly-glissing tone.
 *
 * Because the envelope is anchored and only the partials move, the overall
 * timbre is constant while the pitch drifts without bound. Quantising the
 * slide to semitones up top turns the smooth glide into a stepped endless
 * ARPEGGIO -- a Risset rhythm/melody that seems to climb or fall endlessly.
 *
 *   inlet 0   signal/float   fundamental / reference Hz (the bell centre)
 *   inlet 1   signal/float   MACRO 0..100 -- glide RATE + DIRECTION:
 *                            50 = frozen chord, <50 descending, >50 ascending,
 *                            distance from 50 = speed. It also opens the bell
 *                            (wider/brighter) and, up top, quantises the slide
 *                            into a stepped endless arpeggio.
 *
 *   outlet 0  signal
 *
 * build:
 *   gcc -bundle -undefined dynamic_lookup -o alien_risset~.pd_darwin alien_risset~.c -lm
 */

#include "m_pd.h"
#include "alien.h"
#include <math.h>

#define RISSET_NPART 10          /* octave-spaced partials in the stack */

static t_class *alien_risset_tilde_class;

typedef struct _alien_risset {
    t_object x_obj;
    t_float  x_f;

    double   x_sr;
    double   x_phase[RISSET_NPART]; /* per-partial sine phase, radians    */
    double   x_slide;               /* shared log-freq offset, in octaves */
    al_rng   x_rng;
    al_wander x_wander;             /* slow shimmer brain                 */
    al_dc    x_dc;
} t_alien_risset;

/* ------------------------------------------------------------------ */

static void *alien_risset_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_risset *x = (t_alien_risset *)pd_new(alien_risset_tilde_class);
    int i;
    (void)s; (void)argc; (void)argv;

    x->x_sr = sys_getsr(); if (x->x_sr < 1000.0) x->x_sr = 44100.0;
    for (i = 0; i < RISSET_NPART; i++)
        x->x_phase[i] = (double)i * 0.137;   /* decorrelate starting phases */
    x->x_slide = 0.0;
    al_rng_seed(&x->x_rng, 0x1CE7u);
    al_wander_init(&x->x_wander, &x->x_rng, 4);
    al_dc_init(&x->x_dc);

    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

static t_int *alien_risset_perform(t_int *w)
{
    t_alien_risset *x = (t_alien_risset *)(w[1]);
    t_sample *fin = (t_sample *)(w[2]);
    t_sample *min = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    int nsamp = (int)(w[5]);

    double sr  = x->x_sr;
    double lo  = 8.0;                 /* partial floor                    */
    double hi  = 0.45 * sr;           /* partial ceiling (below Nyquist)  */
    int i;

    al_denorm_off();

    while (nsamp--) {
        double f0 = *fin++, mac = *min++;
        double t, rate, dir, width, arp, shimmer;
        double s, norm, ln_lo, ln_hi, ln_ctr, spread;

        /* macro -> 0..1, 50 becomes the still point */
        t = al_param100(mac);
        if (!(t >= 0.0 && t <= 1.0)) t = 0.5;

        /* clamp the reference pitch (bell centre) */
        if (f0 < 20.0)   f0 = 20.0;
        if (f0 > 2000.0) f0 = 2000.0;

        /* signed drive: -1..+1 around the 0.5 still point */
        dir  = (t - 0.5) * 2.0;                 /* -1 desc .. +1 asc       */
        rate = dir * (0.9 * fabs(dir));         /* octaves/sec, eased      */

        /* bell width + brightness open with distance from centre */
        width = 1.6 + 2.2 * fabs(dir);          /* std-dev in octaves      */

        /* stepped endless-arpeggio up top (strong drive quantises slide)  */
        arp = (fabs(dir) > 0.66) ? (fabs(dir) - 0.66) / 0.34 : 0.0;

        /* slow shimmer brain */
        al_wander_tick(&x->x_wander, &x->x_rng, 0.6, sr);
        shimmer = 0.06 * al_wander_bip(&x->x_wander, 0);

        /* advance the shared log-frequency slide (in octaves) */
        x->x_slide += rate / sr;
        /* keep it bounded to one octave; octave spacing makes this seamless */
        if (x->x_slide >= 1.0 || x->x_slide < 0.0)
            x->x_slide -= floor(x->x_slide);

        {   /* optionally quantise the *effective* slide to semitones (arp) */
            double eff = x->x_slide;
            if (arp > 0.0) {
                double q = floor(eff * 12.0 + 0.5) / 12.0;
                eff = al_lerp(eff, q, arp);
            }

            /* bell over log-frequency: centre one octave above the lowest
             * placed partial so the stack straddles the reference pitch */
            ln_lo  = log2(lo);
            ln_hi  = log2(hi);
            ln_ctr = log2(f0) + 2.0;            /* bell centre (log2 Hz)   */

            s = 0.0; norm = 0.0;
            for (i = 0; i < RISSET_NPART; i++) {
                /* partial i sits at f0 * 2^(i + slide + shimmer_i) */
                double octpos = (double)i + eff + shimmer;
                double lf     = log2(f0) + octpos;   /* this partial, log2  */
                double freq   = pow(2.0, lf);
                double d, env, ph;

                /* fixed Gaussian bell over log-frequency (envelope stays put) */
                d   = (lf - ln_ctr) / width;
                env = exp(-0.5 * d * d);

                /* fade partials out at the audible extremes */
                if (lf <= ln_lo || lf >= ln_hi) { env = 0.0; }
                if (freq < lo || freq > hi)     { env = 0.0; }

                /* advance this partial's phase and sum */
                ph = x->x_phase[i] + AL_2PI * freq / sr;
                if (ph >= AL_2PI || ph < 0.0) ph -= AL_2PI * floor(ph / AL_2PI);
                x->x_phase[i] = ph;

                s    += env * sin(ph);
                norm += env;
            }

            /* normalise by the summed envelope so loudness stays constant */
            spread = (norm > 1e-6) ? (s / norm) : 0.0;
        }

        /* voice out: DC-block, softclip, safety net */
        s = al_dc_tick(&x->x_dc, spread);
        s = al_softclip(s * 1.6);               /* gentle body            */
        if (!(s > -1e6 && s < 1e6)) s = 0.0;    /* NaN/Inf crash net      */
        if (s >  1.5) s =  1.5;
        if (s < -1.5) s = -1.5;

        *out++ = (t_sample)(s * 0.62);          /* ~ -12 dBFS             */
    }
    return (w + 6);
}

static void alien_risset_dsp(t_alien_risset *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    if (x->x_sr < 1000.0) x->x_sr = 44100.0;
    dsp_add(alien_risset_perform, 5, x,
            sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

void alien_risset_tilde_setup(void)
{
    alien_risset_tilde_class = class_new(gensym("alien_risset~"),
        (t_newmethod)alien_risset_new, 0, sizeof(t_alien_risset),
        CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_risset_tilde_class, t_alien_risset, x_f);
    class_addmethod(alien_risset_tilde_class, (t_method)alien_risset_dsp,
                    gensym("dsp"), A_CANT, 0);
}
