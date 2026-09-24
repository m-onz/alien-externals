/* alien_banded~.c  --  banded waveguide synthesis (Essl & Cook)
 *
 * A small bank of BANDED WAVEGUIDES: each waveguide is a band-pass filter in
 * series with a feedback delay line whose length is sr/mode_freq, so the loop
 * resonates AT one mode of a struck/bowed object and nowhere else.  Tuned to a
 * material's modal ratios (bar: 1, 2.76, 5.4, 8.9...; bowl/glass: near, saw:
 * harmonic) and driven CONTINUOUSLY by a nonlinear BOW (stick-slip friction on
 * bow_velocity - junction_velocity), the bank sings: bowed bars, crystal
 * glasses, singing bowls, musical saws -- continuous, tonal, inharmonic.
 *
 * This is a DRONE voice: a fundamental in Hz and one macro knob, no note list.
 * Per block we read the macro, pick the material/brightness/bow, and per sample
 * we run the friction map into every band and sum the junction velocities.
 *
 *   inlet 0   signal/float   fundamental in Hz  (CLASS_MAINSIGNALIN)
 *   inlet 1   signal/float   MACRO 0..100 (wrapped mod 100) -- one knob that
 *                            morphs the whole voice.  At 0 a soft glassy hum
 *                            (few near-harmonic modes, light bow, dark).  It
 *                            sweeps material (bowl->glass->bar->saw->alien),
 *                            bow force/velocity, bow position, brightness and
 *                            mode count, up to a bright screaming bowed metal.
 *
 *   outlet 0  signal
 *
 * build:
 *   gcc -shared -fPIC -O2 -o alien_banded~.pd_linux alien_banded~.c -lm
 *   (macOS: -bundle -undefined dynamic_lookup -o alien_banded~.pd_darwin)
 */

#include "m_pd.h"
#include "alien.h"
#include <math.h>
#include <string.h>

#define NBANDS   6              /* max banded waveguides */
#define DELMAX   8192           /* per-band delay buffer (pow of two) */
#define DELMASK  (DELMAX - 1)

/* material ratio sets: near-harmonic hum -> inharmonic metal -> harmonic saw */
#define NMAT 5
static const double BMAT[NMAT][NBANDS] = {
 {1.0, 2.00, 3.01, 4.02, 5.00, 6.03},   /* 0 near-harmonic bowl (soft hum) */
 {1.0, 2.40, 4.72, 7.70, 11.30, 15.44}, /* 1 glass */
 {1.0, 2.76, 5.40, 8.93, 13.34, 18.64}, /* 2 bar / tube (bowed vibraphone) */
 {1.0, 2.00, 3.00, 4.00, 5.00, 6.00},   /* 3 saw (harmonic) */
 {1.0, 1.71, 3.14, 4.28, 6.83, 8.11}    /* 4 alien alloy */
};

static t_class *alien_banded_tilde_class;

typedef struct _alien_banded {
    t_object x_obj;
    t_float  x_f;

    double   x_sr;

    /* per band: a bandpass resonator + a feedback delay line */
    al_bq    x_bp[NBANDS];
    double   x_buf[NBANDS][DELMAX];
    int      x_wr[NBANDS];
    double   x_junc[NBANDS];       /* last junction velocity (loop state) */

    /* smoothed / cached morph state */
    double   x_freq;               /* glided fundamental */
    double   x_ratio[NBANDS];      /* current mode ratios (glided material) */
    al_glide x_bowg;               /* bow velocity glide */
    al_dc    x_dc;
    al_svf   x_tone;               /* output brightness lowpass */
    al_wander x_wan;
    al_rng   x_rng;
    double   x_bowphase;           /* not used for tone, only micro */
} t_alien_banded;

/* map the 0..1 macro onto the whole voice */
static inline void banded_macro(double t, int *mat, int *n,
                                double *bowvel, double *bowforce,
                                double *bowpos, double *bright,
                                double *fb, double *drive)
{
    *mat = (int)(t * (double)NMAT);
    if (*mat >= NMAT) *mat = NMAT - 1;
    if (*mat < 0)     *mat = 0;

    *n = (int)(3.0 + 3.0 * t);              /* 3 -> 6 modes */
    if (*n < 1)      *n = 1;
    if (*n > NBANDS) *n = NBANDS;

    *bowvel   = 0.42 + 0.36 * t;            /* soft bow -> fast hard bow */
    *bowforce = 4.5 + 2.5 * t;              /* slip curve sharpness */
    *bowpos   = 0.11 + 0.22 * t;            /* bow point (mode gain shape) */
    *bright   = 0.05 + 0.90 * t;            /* dark glassy hum -> screaming */
    *fb       = 0.9975 + 0.0010 * t;        /* ring length; < 1 always */
    if (*fb > 0.9985) *fb = 0.9985;
    *drive    = (t < 0.5) ? 0.0 : (t - 0.5) / 0.5;  /* grit up top */
}

/* stick-slip friction: velocity difference -> bow force injected into loop.
 * The classic banded-waveguide friction map: force is MAXIMAL near dv=0
 * (the bow sticks and drags the string with it) and falls off as the string
 * slips (|dv| grows).  We return a velocity the bow imparts to the loop; the
 * sign follows dv so the bank is pulled toward the bow velocity, sustaining
 * the resonators.  Bounded and odd. */
static inline double bow_friction(double dv, double force)
{
    /* Friction coefficient: 1 at dv=0 (stick), decaying to 0 (slip). This is
     * the nonlinear energy pump -- it must inject real energy every sample. */
    double a  = force * dv;
    double mu = 1.0 / (1.0 + a * a);        /* -> 1 stick, -> 0 slip */
    double f  = dv * mu;                     /* velocity injected into loop */
    return al_softclip(f * 3.0);            /* bounded (~[-1,1]) */
}

static t_int *alien_banded_perform(t_int *w)
{
    t_alien_banded *x = (t_alien_banded *)(w[1]);
    t_sample *fin = (t_sample *)(w[2]);
    t_sample *min = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    int nsamp = (int)(w[5]);

    double sr = x->x_sr, nyq = sr * 0.45;
    int i, n;

    al_denorm_off();

    /* ---- once-per-block morph, driven by the first sample ---- */
    double mac = (nsamp > 0) ? (double)min[0] : 0.0;
    double ftarget = (nsamp > 0) ? (double)fin[0] : x->x_freq;
    double t, bowvel, bowforce, bowpos, bright, fb, drive;
    int mat, nreq;

    t = mac - 100.0 * floor(mac * 0.01);
    if (!(t >= 0.0 && t <= 100.0)) t = 0.0;
    t *= 0.01;
    banded_macro(t, &mat, &nreq, &bowvel, &bowforce, &bowpos, &bright, &fb, &drive);

    if (!(ftarget >= 1.0 && ftarget <= 1e6)) ftarget = x->x_freq;
    /* glide the fundamental once per block */
    x->x_freq += 0.2 * (ftarget - x->x_freq);
    if (x->x_freq < 8.0)   x->x_freq = 8.0;
    if (x->x_freq > nyq)   x->x_freq = nyq;

    /* glide material ratios toward the selected set (smooth material morph) */
    n = nreq;
    for (i = 0; i < NBANDS; i++) {
        double tgt = BMAT[mat][i];
        x->x_ratio[i] += 0.05 * (tgt - x->x_ratio[i]);
        if (!(x->x_ratio[i] > 0.01 && x->x_ratio[i] < 100.0)) x->x_ratio[i] = tgt;
    }

    /* mode gains from bow position: sin(pi*(i+1)*pos), normalised */
    double g[NBANDS], gsum = 0.0;
    for (i = 0; i < n; i++) {
        g[i] = fabs(sin(M_PI * (double)(i + 1) * bowpos));
        if (g[i] < 0.08) g[i] = 0.08;       /* never a fully dead band */
        gsum += g[i];
    }
    if (gsum < 1e-6) gsum = 1.0;
    double invgsum = 1.0 / gsum;

    /* precompute per-band delay length + bandpass Q (bounded) */
    double dlen[NBANDS], bpq[NBANDS], mfreq[NBANDS];
    for (i = 0; i < n; i++) {
        double fmode = x->x_freq * x->x_ratio[i];
        if (fmode < 8.0)  fmode = 8.0;
        if (fmode > nyq)  fmode = nyq;
        mfreq[i] = fmode;
        {
            double d = sr / fmode;
            if (d < 2.0)          d = 2.0;
            if (d > DELMAX - 3.0) d = DELMAX - 3.0;
            /* the bandpass adds ~1 sample group delay; keep the loop at fmode */
            dlen[i] = d - 1.0;
            if (dlen[i] < 1.0) dlen[i] = 1.0;
        }
        /* loop bandpass Q: keep it modest so the bandpass passes the loop
         * energy (a very high Q here would choke the waveguide).  The RING
         * comes from the delay feedback, not from this filter. */
        bpq[i] = 2.0 + 6.0 * bright;
    }

    /* output brightness lowpass: dark glassy hum -> wide-open bowed metal.
     * Track it to the fundamental so it opens musically with pitch too. */
    double tonehz = x->x_freq * (2.0 + 30.0 * bright);
    if (tonehz < 120.0)  tonehz = 120.0;
    if (tonehz > nyq)    tonehz = nyq;

    double bvg;   /* smoothed bow velocity target held across the block */
    double micro; /* micro bow variation from wander */

    while (nsamp--) {
        double s = 0.0, jsum = 0.0;

        fin++; min++;                         /* consumed at block rate */

        /* slow micro variation so the bow sings (never repeats exactly) */
        al_wander_tick(&x->x_wan, &x->x_rng, 2.0, sr);
        micro = 0.06 * al_wander_bip(&x->x_wan, 0);
        bvg = al_glide_tick(&x->x_bowg, bowvel * (1.0 + micro), 20.0, sr);

        /* mean junction velocity: the bow contacts the whole bank.  This is
         * the velocity the string presents to the bow at the contact point. */
        for (i = 0; i < n; i++) jsum += x->x_junc[i] * g[i];
        jsum *= invgsum;

        /* stick-slip friction on (bow velocity - junction velocity).  This is
         * the nonlinear energy source that keeps the bank singing. */
        double dv = bvg - jsum;
        double excite = bow_friction(dv, bowforce);

        for (i = 0; i < n; i++) {
            /* BANDED WAVEGUIDE: the delay line is the resonator (feedback near
             * 1 => long ring at the mode); the bandpass is the LOOP FILTER that
             * keeps only energy near this mode circulating.  The bow injects
             * energy into the delayed loop signal every sample. */
            double loop = al_del_read(x->x_buf[i], DELMASK, x->x_wr[i], dlen[i]);
            double in   = excite * g[i] + fb * loop;
            double bpv  = al_reson(&x->x_bp[i], in, mfreq[i], bpq[i], sr);

            /* recentre the loop signal around the delayed value: the bandpass
             * passband gain restores the loop energy so the delay ring builds
             * to a musical amplitude instead of decaying to nothing. */
            double y = bpv;

            if (!isfinite(y)) y = 0.0;
            if (y > -1e-20 && y < 1e-20) y = 0.0;   /* denormal flush */
            if (y >  1e6) y =  1e6;
            if (y < -1e6) y = -1e6;

            al_del_write(x->x_buf[i], DELMASK, &x->x_wr[i], y);
            x->x_junc[i] = y;                  /* junction velocity for coupling */
            s += y * g[i];
        }
        s *= invgsum;

        s = al_dc_tick(&x->x_dc, s);
        s *= 2.0;                              /* bring modal sum up to level */
        s = al_svf_lp(&x->x_tone, s, tonehz, 0.2, sr);  /* brightness tilt */
        if (drive > 0.0)
            /* grit up top, but keep the level (light makeup) so the bright
             * bowed-metal stays loud rather than being squashed quiet */
            s = al_softclip(s * (1.0 + 2.0 * drive));
        else
            s = al_softclip(s);

        /* crash net */
        if (!(s > -1e6 && s < 1e6)) s = 0.0;
        if (s >  1.5) s =  1.5;
        if (s < -1.5) s = -1.5;

        *out++ = (t_sample)(s * 0.34);   /* loudness-trimmed to ~-12 dBFS */
    }

    (void)bvg;
    return (w + 6);
}

static void alien_banded_dsp(t_alien_banded *x, t_signal **sp)
{
    if (sp[0]->s_sr >= 1000.0) x->x_sr = sp[0]->s_sr;
    if (x->x_sr < 1000.0) x->x_sr = 44100.0;
    dsp_add(alien_banded_perform, 5, x,
            sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_banded_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_banded *x = (t_alien_banded *)pd_new(alien_banded_tilde_class);
    int i;
    (void)s; (void)argc; (void)argv;

    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    x->x_freq = 110.0;
    x->x_bowphase = 0.0;

    al_rng_seed(&x->x_rng, 0x1234abcdu);
    al_dc_init(&x->x_dc);
    al_svf_init(&x->x_tone);
    al_glide_init(&x->x_bowg, 0.1);
    al_wander_init(&x->x_wan, &x->x_rng, 4);

    for (i = 0; i < NBANDS; i++) {
        al_bq_init(&x->x_bp[i]);
        memset(x->x_buf[i], 0, sizeof(x->x_buf[i]));
        x->x_wr[i] = 0;
        x->x_junc[i] = 0.0;
        x->x_ratio[i] = BMAT[0][i];
    }

    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);   /* macro */
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_banded_tilde_setup(void)
{
    alien_banded_tilde_class = class_new(gensym("alien_banded~"),
        (t_newmethod)alien_banded_new, 0, sizeof(t_alien_banded),
        CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_banded_tilde_class, t_alien_banded, x_f);
    class_addmethod(alien_banded_tilde_class, (t_method)alien_banded_dsp,
                    gensym("dsp"), A_CANT, 0);
}
