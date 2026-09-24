/* alien_massx~ -- ANGRY WOBBLE SQUELCH bass, but CLEAR (tonal, not noisy)
 *
 * The tonal SOURCE is the alien_mass~ scanned-synthesis engine: a ring of masses
 * joined by springs vibrates at a slow rate and a phase accumulator scans the
 * ring as a wavetable at f0. Pitch and timbre are decoupled, and because the
 * physics never settles the tone evolves continuously without ever sounding
 * programmed (Verplank/Mathews/Shaw, 2000). That evolving-but-tonal ring is
 * then turned into an aggressive dubstep-style bass:
 *
 *     mass-spring scanned engine  ->  drive + gentle fold (ANGRY)  ->
 *     crossover highpass          ->  resonant ladder wob (SQUELCH) ->
 *     + clean sine SUB one octave below (BASS)  ->  DC block -> softclip
 *
 * THE WOB (inlet 1, macro 0..100, read PER SAMPLE): the ladder CUTOFF is the
 * macro. It is smoothed with a one-pole so a fast LFO into inlet 1 gives a big
 * smooth squelchy wob with no zipper. Resonance is high-ish but controlled so it
 * squelches without self-oscillating into hiss. The macro also opens the drive
 * and fold as it climbs, so the bass gets ANGRIER as it opens -- but the source
 * stays the tonal ring, so it stays CLEAR (low spectral flatness) throughout.
 *
 * THE BRAIN (al_wander): a slow autonomous novelty brain gently drifts the
 * engine intensity, ladder resonance and drive so a held note keeps evolving.
 * Its depth is kept modest so the voice never tips into noise.
 *
 * SAFETY (build uses -ffast-math -> rely on magnitude clamps): the mass engine
 * keeps its own stability clamps (spring gain 4*k<1, always-damped, reflective
 * walls, blow-up reset); f0 clamped [0.01, 0.45*sr]; ladder/HP stable; all state
 * denormal-flushed; an object-level final guard `if(!(s>-1e6&&s<1e6)) s=0;` then
 * bounds to ~[-1.5,1.5].
 *
 *   inlet 0   signal/float   fundamental in Hz         (CLASS_MAINSIGNALIN)
 *   inlet 1   signal/float   MACRO 0..100 (the WOB)    -- LFO this for the wobble
 *   outlet 0  signal
 */

#include "m_pd.h"
#include "alien.h"

#define MAXM     256
#define MINM     8
#define PHYSDIV  32          /* physics runs every 32 audio samples */
#define NSHAPE   4

#define STATEMAX  8.0
#define BLOWUP    1.0e6

static t_class *alien_massx_class;

typedef struct _alien_massx {
    t_object   x_obj;
    t_float    x_f;                 /* scalar fallback for inlet 0 */

    /* --- scanned mass-spring engine (the tonal source) --- */
    double   x_pos[MAXM], x_vel[MAXM];
    float    x_tab[MAXM + 1];
    double   x_sr, x_ph;
    int      x_n, x_div;
    int      x_shape;
    unsigned x_rng;

    /* --- bass / wob chain --- */
    al_phasor  sub;                 /* clean sine sub-octave */
    al_ladder  lad;                 /* resonant ladder (the squelch) */
    al_svf     bp;                  /* extra resonant bandpass squelch */
    al_dc      dc;                  /* DC block */
    al_wander  wnd;                 /* the slow novelty brain */
    al_rng     arng;
    double     hpx, hpy;            /* crossover highpass state */
    double     msm;                 /* smoothed macro (anti-zipper) */
} t_alien_massx;

/* ---- mass-spring engine (copied from alien_mass~, kept tonal & stable) ---- */

static unsigned rn(t_alien_massx *x)
{
    x->x_rng ^= x->x_rng << 13; x->x_rng ^= x->x_rng >> 17;
    x->x_rng ^= x->x_rng << 5;  return x->x_rng;
}

static inline double safe_d(double v)
{
    if (!(v > -BLOWUP && v < BLOWUP)) return 0.0;   /* NaN/Inf/huge -> 0 */
    if (v >  STATEMAX) return  STATEMAX;
    if (v < -STATEMAX) return -STATEMAX;
    if (v > -1.0e-15 && v < 1.0e-15) return 0.0;    /* denormal flush */
    return v;
}

static void mass_strike(t_alien_massx *x, int idx)
{
    int n = x->x_n, i;
    if (n < MINM) n = MINM;
    if (n > MAXM) n = MAXM;
    x->x_n = n;
    idx = (idx % NSHAPE + NSHAPE) % NSHAPE;
    for (i = 0; i < n; i++) {
        double t = (double)i / (double)n;
        double v;
        switch (idx) {
            case 0: v = sin(t * 2.0 * AL_PI); break;                /* sine  */
            case 1: v = sin(t * 4.0 * AL_PI); break;                /* two   */
            case 2: v = (t < 0.1) ? 1.0 : 0.0; break;               /* pulse */
            default: v = ((rn(x) & 0xffff) / 32768.0 - 1.0); break; /* rand  */
        }
        x->x_pos[i] = v;
        x->x_vel[i] = 0.0;
        x->x_tab[i] = (float)v;
    }
    x->x_tab[n] = x->x_tab[0];
    x->x_shape = idx;
}

static void mass_reset(t_alien_massx *x)
{
    int i;
    for (i = 0; i < MAXM; i++) { x->x_pos[i] = 0.0; x->x_vel[i] = 0.0; }
    mass_strike(x, x->x_shape < 0 ? 0 : x->x_shape);
}

/* macro morph -- kept modest so the ring stays tonal, biased so even at low
 * macro there is a lively, evolving ring (this is the audible bass source) */
static inline int mass_macro(double t, double *stiff, double *centre,
                             double *damp, double *speed, int *count)
{
    if (!(t >= 0.0 && t <= 1.0)) t = 0.0;

    *stiff  = 0.05 + 0.14 * t;              /* soft -> stiff (k stays < 0.25) */
    *centre = 0.03 + 0.15 * t;              /* saggy -> firm restoring        */
    *damp   = 0.30 * (1.0 - t) + 0.020;     /* dead -> lightly damped         */
    *speed  = 0.55 + 2.20 * t;              /* crawl -> fast evolution        */

    {
        int n = 20 + (int)(72.0 * t + 0.5);     /* 20 .. 92 */
        if (n < MINM) n = MINM;
        if (n > MAXM) n = MAXM;
        *count = n;
    }

    if (t < 0.35) return 0;                 /* sine  */
    if (t < 0.65) return 1;                 /* two   */
    return 2;                               /* pulse -- avoid rand shape:
                                               keep the ring harmonic/tonal   */
}

static void mass_step(t_alien_massx *x, double stiff, double centre,
                      double damp, double speed)
{
    int n = x->x_n, i;
    int blew = 0;

    double k = stiff * speed;
    double c = centre * 0.02 * speed;
    double d = 1.0 - damp * 0.02 * speed;

    if (k < 0.0)   k = 0.0;
    if (k > 0.24)  k = 0.24;       /* 4*k <= 0.96 < 1 */
    if (c < 0.0)   c = 0.0;
    if (c > 0.20)  c = 0.20;
    if (d > 0.9999) d = 0.9999;
    if (d < 0.80)   d = 0.80;

    for (i = 0; i < n; i++) {
        double l = x->x_pos[(i - 1 + n) % n];
        double r = x->x_pos[(i + 1) % n];
        double f = k * (l + r - 2.0 * x->x_pos[i]) - c * x->x_pos[i];
        double v = x->x_vel[i] * d + f;
        if (!(v > -BLOWUP && v < BLOWUP)) { blew = 1; v = 0.0; }
        x->x_vel[i] = v;
    }
    for (i = 0; i < n; i++) {
        double p = x->x_pos[i] + x->x_vel[i];
        if (!(p > -BLOWUP && p < BLOWUP)) blew = 1;
        p = safe_d(p);
        if (p >=  STATEMAX) { p =  STATEMAX; x->x_vel[i] *= -0.5; }
        if (p <= -STATEMAX) { p = -STATEMAX; x->x_vel[i] *= -0.5; }
        x->x_vel[i] = safe_d(x->x_vel[i]);
        x->x_pos[i] = p;
        x->x_tab[i] = (float)p;
    }
    x->x_tab[n] = x->x_tab[0];

    if (blew) mass_reset(x);
}

/* ------------------------------------------------------------------ */

static t_int *alien_massx_perform(t_int *w)
{
    t_alien_massx *x = (t_alien_massx *)(w[1]);
    t_sample *fin = (t_sample *)(w[2]);
    t_sample *min = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    int nsamp = (int)(w[5]);

    double sr = x->x_sr, ph = x->x_ph;
    double stiff, centre, damp, speed;
    int shape, count, n;

    double hpa = exp(-AL_2PI * 120.0 / sr);         /* crossover HP ~120 Hz */
    double smc = 1.0 - exp(-AL_2PI * 45.0 / sr);    /* macro smoothing coef */
    double msm = x->msm;

    al_denorm_off();

    /* ---- ONCE PER BLOCK: slow structure from the macro's first sample ---- */
    {
        double mac = (nsamp > 0) ? (double)min[0] : 0.0;
        double t;
        if (!(mac > -1e9 && mac < 1e9)) mac = 0.0;
        t = mac - 100.0 * floor(mac * 0.01);      /* wrap into [0,100) */
        if (!(t >= 0.0 && t <= 100.0)) t = 0.0;
        t *= 0.01;
        shape = mass_macro(t, &stiff, &centre, &damp, &speed, &count);

        if (count != x->x_n) {
            int old = x->x_n;
            x->x_n = count;
            if (count > old) {
                int i;
                for (i = old; i < count; i++) {
                    x->x_pos[i] = 0.0;
                    x->x_vel[i] = 0.0;
                    x->x_tab[i] = 0.0f;
                }
            }
            x->x_tab[x->x_n] = x->x_tab[0];
        }
        if (shape != x->x_shape) mass_strike(x, shape);
    }

    n = x->x_n;
    if (n < MINM) n = MINM;
    if (n > MAXM) n = MAXM;
    x->x_n = n;

    while (nsamp--) {
        double f0 = *fin++, pr = *min++;
        double pn = (pr >= 0.0 && pr <= 100.0) ? pr*0.01 : al_param100(pr);
        double p, pos, frac, eng, subv, up, hp, cut, s;
        double weng, wres, wdr;
        int i0;

        if (!(f0 > 0.0)) f0 = 0.01;
        if (f0 < 0.01)   f0 = 0.01;
        if (f0 > sr*0.45) f0 = sr*0.45;

        /* smooth the macro so a fast/stepped LFO wobbles with no zipper */
        msm += smc * (pn - msm);
        if (!(msm >= 0.0 && msm <= 1.0)) msm = al_clamp(msm, 0.0, 1.0);

        /* ---- slow brain: gentle drift, kept modest to stay tonal -------- */
        al_wander_tick(&x->wnd, &x->arng, 0.25 + msm*1.0, sr);
        weng = al_wander_val(&x->wnd, 0);   /* engine intensity */
        wres = al_wander_val(&x->wnd, 1);   /* ladder resonance */
        wdr  = al_wander_val(&x->wnd, 2);   /* drive            */

        /* ---- advance / scan the mass-spring ring (the tonal source) ----- */
        if (--x->x_div <= 0) {
            x->x_div = PHYSDIV;
            mass_step(x, stiff, centre, damp, speed);
            n = x->x_n;
        }
        ph += f0 / sr;
        ph -= floor(ph);
        p    = ph;
        pos  = p * (double)n;
        i0   = (int)pos;
        if (i0 < 0)   i0 = 0;
        if (i0 >= n)  i0 = n - 1;
        frac = pos - (double)i0;
        eng  = x->x_tab[i0] + (x->x_tab[i0 + 1] - x->x_tab[i0]) * frac;
        if (!(eng > -1e6 && eng < 1e6)) eng = 0.0;

        /* engine intensity: macro + a touch of brain drift */
        eng *= 0.65 + 0.35*msm + 0.10*(weng - 0.5);

        /* ---- ANGRY: drive + gentle fold, but kept CLEAR (not overdriven) */
        {
            double drv = 0.10 + 0.35*msm + 0.12*wdr;
            eng = al_drive(eng, drv);
            /* gentle fold only, scaled small so the ring stays tonal */
            eng = al_fold(eng, 0.05 + 0.18*msm);
        }

        /* ---- crossover highpass so the clean sub owns the lows --------- */
        hp = hpa*(x->hpy + eng - x->hpx); x->hpx = eng; x->hpy = hp; up = hp;

        /* ---- SQUELCH: resonant ladder, CUTOFF is the macro ------------- */
        cut = 60.0 * pow(2.0, 0.6 + msm*5.6);       /* ~90 Hz .. ~4 kHz */
        cut = al_clamp(cut, 45.0, 9000.0);
        {
            double res = 0.35 + 0.42*msm + 0.12*wres;   /* controlled, no self-osc */
            res = al_clamp(res, 0.0, 0.90);
            up = al_ladder_tick(&x->lad, up, cut, res, sr);
            /* extra resonant bandpass squelch tracking the ladder cutoff */
            up += 0.35 * al_svf_bp(&x->bp, up, al_clamp(cut*1.2,45.0,9000.0),
                                   0.75 + 0.15*msm, sr);
        }

        /* ---- BASS: clean sine SUB one octave below the note ----------- */
        subv = al_sin01(al_phasor_tick(&x->sub, f0*0.5, sr));

        /* ---- mix, DC block, final guard ------------------------------- */
        s = (1.0 - 0.25*msm)*subv + (1.15 + 0.35*msm)*up;
        s = al_dc_tick(&x->dc, s);

        if (!(s > -1e6 && s < 1e6)) {               /* object-level guard */
            s = 0.0;
            al_ladder_init(&x->lad);
            al_svf_init(&x->bp);
            x->hpx = x->hpy = 0.0;
        }
        s = al_softclip(s);
        if (s >  1.5) s =  1.5; else if (s < -1.5) s = -1.5;
        *out++ = (t_sample)(s * 0.48);
    }

    x->x_ph = ph;
    x->msm  = msm;
    return (w + 6);
}

static void alien_massx_dsp(t_alien_massx *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    if (x->x_sr < 1000.0) x->x_sr = 44100.0;
    dsp_add(alien_massx_perform, 5, x,
            sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_massx_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_massx *x = (t_alien_massx *)pd_new(alien_massx_class);
    (void)s; (void)argc; (void)argv;

    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    x->x_ph = 0.0; x->x_div = PHYSDIV;
    x->x_n = 20;
    x->x_rng = 88172645u;
    memset(x->x_pos, 0, sizeof(x->x_pos));
    memset(x->x_vel, 0, sizeof(x->x_vel));
    memset(x->x_tab, 0, sizeof(x->x_tab));
    x->x_shape = -1;
    mass_strike(x, 0);          /* sine */

    al_phasor_init(&x->sub);
    al_ladder_init(&x->lad);
    al_svf_init(&x->bp);
    al_dc_init(&x->dc);
    al_rng_seed(&x->arng, 0x3a17c9du);
    al_wander_init(&x->wnd, &x->arng, 4);
    x->hpx = x->hpy = 0.0; x->msm = 0.0;

    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_massx_tilde_setup(void)
{
    alien_massx_class = class_new(gensym("alien_massx~"),
        (t_newmethod)alien_massx_new, 0, sizeof(t_alien_massx),
        CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_massx_class, t_alien_massx, x_f);
    class_addmethod(alien_massx_class, (t_method)alien_massx_dsp,
                    gensym("dsp"), A_CANT, 0);
}
