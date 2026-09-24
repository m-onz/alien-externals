/* alien_membranex~ -- ANGRY WOBBLE SQUELCHY (but CLEAR) drum-membrane BASS
 *
 * A bass mutation of alien_membrane~. The SOURCE is the same 2D wave-equation
 * mesh, re-struck at the note frequency so it produces a sustained, pitched,
 * inharmonic-but-TONAL drum/metal tone (not noise). That ringing mesh is then
 * turned into an aggressive dub bass:
 *
 *     mesh (re-struck at f0) -> DC block -> crossover highpass
 *        -> drive + gentle fold (ANGRY)
 *        -> resonant ladder, cutoff = MACRO read PER SAMPLE (SQUELCHY WOB)
 *        -> optional resonant SVF bandpass (extra squelch)
 *        + clean sine SUB one octave below (owns the lows)
 *        -> DC block -> final guard/softclip
 *
 * THE WOB (inlet 1, macro 0..100, read PER SAMPLE, one-pole smoothed): drives
 * the ladder cutoff over a wide range, plus scales drive/fold/resonance, so a
 * fast LFO into inlet 1 gives a big talking squelch with no zipper. Macro also
 * raises the overall aggression as it opens.
 *
 * THE COMPLEXITY (al_wander brain): a slow autonomous brain drifts the ladder
 * resonance, drive, fold and the mesh low-pass loss so a held note keeps
 * evolving -- staying TONAL (spectral flatness kept LOW; back off res/drive if
 * it hisses).
 *
 * The mesh stability is kept intact: grid clamp (g*g <= NCELL), tension strictly
 * below the 2D explicit-FD CFL limit (0.47 < 0.5), per-cell magnitude clamps,
 * denormal flush, and IEEE-bit finite guards that survive -ffast-math.
 *
 *   inlet 0   signal/float   fundamental in Hz     (CLASS_MAINSIGNALIN)
 *   inlet 1   signal/float   MACRO 0..100 (the WOB) -- LFO this for the wobble
 *   outlet 0  signal
 *
 * SAFETY: f0 clamped [0.01, 0.45*sr]; mesh clamps kept; ladder stable; all state
 * finite-guarded + denormal-flushed; final guard if(!(s>-1e6&&s<1e6)) s=0; then
 * bounded to ~[-1.5,1.5]. Final gain normalised for integrated RMS ~ -12 dBFS
 * and peak <= ~0.95 (the previous membrane ran hot -- this keeps peak in check).
 */

#include "m_pd.h"
#include "alien.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Grid limits (as in alien_membrane~): macro maps t in [0,1] onto g = 8..28 for
 * a slightly smaller mesh (denser, more tonal low modes for bass), MAXG carries
 * headroom so g*g can never exceed NCELL. */
#define MAXG  32
#define NCELL (MAXG * MAXG)

/* -ffast-math folds isfinite()/isnan(): test the raw IEEE-754 bits instead. */
static inline int mx_bad(float v)
{
    union { float f; unsigned int u; } p;
    p.f = v;
    return ((p.u & 0x7f800000u) == 0x7f800000u);
}
static inline float mx_denorm(float v)
{
    return (fabsf(v) < 1e-20f) ? 0.f : v;
}

static t_class *alien_membranex_tilde_class;

typedef struct _alien_membranex {
    t_object  x_obj;
    t_float   x_f;

    /* --- mesh engine (copied from alien_membrane~) --- */
    float    x_u[NCELL], x_v[NCELL];    /* current, previous            */
    int      x_g;
    int      x_lastg;
    double   x_lp;                      /* mesh top-octave low-pass      */
    double   x_dc_x1, x_dc_y1;          /* mesh output DC-blocker        */
    double   x_env;                     /* mesh output AGC envelope      */
    double   x_phase;                   /* re-strike phase (f0)          */
    double   x_sr;

    /* --- bass chain --- */
    al_phasor  sub;                     /* clean sine sub-octave         */
    al_ladder  lad;                     /* resonant squelch ladder       */
    al_svf     bp;                      /* resonant bandpass (squelch)   */
    al_dc      dcout;                   /* final DC block                */
    al_wander  wnd;                     /* slow evolution brain          */
    al_rng     rng;
    double     hpx, hpy;                /* crossover highpass state      */
    double     msm;                     /* smoothed macro (anti-zipper)  */
} t_alien_membranex;

#define IDX(x_, i, j) ((i) * (x_)->x_g + (j))

/* ------------------------------------------------------------------ */

/* mesh macro morph, tuned tonal for bass: tense enough to ring, damping never
 * so low it drones forever, strike/pickup near centre for the strongest low
 * modes. Kept below CFL for stability. */
static inline void mx_macro(double t, int *g, double *tension, double *damp,
                            double *loss, int *bound,
                            double *sx, double *sy, double *px, double *py)
{
    *g       = (int)(10.0 + 16.0 * t + 0.5);    /* 10x10 -> 26x26            */
    if (*g < 4)    *g = 4;
    if (*g > MAXG) *g = MAXG;
    /* tension < CFL 0.5; keep a healthy ring but never on the edge */
    *tension = 0.20 + 0.26 * t;                 /* 0.20 -> 0.46 (< 0.47)     */
    if (*tension > 0.46) *tension = 0.46;
    if (*tension < 0.0)  *tension = 0.0;
    /* damping: firm at 0, long-ringing (but bounded) at 100 */
    *damp    = 0.85 - 0.78 * t;                  /* 0.85 -> 0.07              */
    if (*damp < 0.05) *damp = 0.05;
    /* HF loss: keep the top octave tamed so the mesh stays tonal, not hissy */
    *loss    = 0.65 - 0.45 * t;                  /* 0.65 -> 0.20              */
    if (*loss < 0.0)  *loss = 0.0;
    if (*loss > 1.0)  *loss = 1.0;
    *bound   = (t >= 0.7);                        /* clamped -> free plate    */

    /* strike/pickup near centre (fundamental-heavy) drifting slightly off */
    *sx = 0.5 - 0.18 * t;
    *sy = 0.5 - 0.18 * t;
    *px = 0.5 + 0.20 * t;
    *py = 0.42 + 0.16 * t;
}

static t_int *alien_membranex_perform(t_int *w)
{
    t_alien_membranex *x = (t_alien_membranex *)(w[1]);
    t_sample *in   = (t_sample *)(w[2]);
    t_sample *min  = (t_sample *)(w[3]);
    t_sample *out  = (t_sample *)(w[4]);
    int nsamp = (int)(w[5]);

    int g, i, j;
    int si, sj, pi, pj, bound;
    float damp_step;
    double tension, damp, loss, sx, sy, px, py;
    double sr  = x->x_sr;
    double hpa = exp(-AL_2PI * 55.0 / sr);        /* crossover HP ~55 Hz     */
    double smc = 1.0 - exp(-AL_2PI * 45.0 / sr);  /* macro smoothing coef    */
    double msm = x->msm;

    al_denorm_off();

    /* macro is control-rate for the MESH (grid resize etc.): first sample */
    {
        double mac = *min, t;
        t = mac - 100.0 * floor(mac * 0.01);
        if (!(t >= 0.0 && t <= 100.0)) t = 0.0;
        t *= 0.01;
        mx_macro(t, &g, &tension, &damp, &loss, &bound, &sx, &sy, &px, &py);
    }

    if (g != x->x_lastg) {
        memset(x->x_u, 0, sizeof(x->x_u));
        memset(x->x_v, 0, sizeof(x->x_v));
        x->x_lp = 0.0;
        x->x_lastg = g;
    }
    x->x_g = g;

    si = (int)(sy * (g - 1)); sj = (int)(sx * (g - 1));
    pi = (int)(py * (g - 1)); pj = (int)(px * (g - 1));
    damp_step = 1.f - (float)damp * 0.005f;
    if (damp_step > 0.99995f) damp_step = 0.99995f;
    if (damp_step < 0.90f)    damp_step = 0.90f;

    if (si < 0) si = 0; if (si >= g) si = g - 1;
    if (sj < 0) sj = 0; if (sj >= g) sj = g - 1;
    if (pi < 0) pi = 0; if (pi >= g) pi = g - 1;
    if (pj < 0) pj = 0; if (pj >= g) pj = g - 1;

    while (nsamp--) {
        float freq = *in++;
        float c2, y;
        double f0, pr, pn, mesh, sub, s, hp;
        double wq, wdr, wfold, wloss;
        double cut, res, drv, fold, depth;
        double inc;

        pr = *min++;
        f0 = (double)freq;

        /* ---- f0 guard (Hz) ---- */
        if (mx_bad(freq)) f0 = 0.01;
        if (f0 < 0.01)    f0 = 0.01;
        if (f0 > sr*0.45) f0 = sr*0.45;

        /* ---- macro 0..1, per sample, smoothed (zipper-free wob) ---- */
        pn = (pr >= 0.0 && pr <= 100.0) ? pr*0.01 : al_param100(pr);
        msm += smc * (pn - msm);
        if (!(msm >= 0.0 && msm <= 1.0)) msm = al_clamp(msm, 0.0, 1.0);

        /* ---- slow evolution brain (rate & depth grow gently with macro) ---- */
        al_wander_tick(&x->wnd, &x->rng, 0.3 + msm*1.8, sr);
        depth = 0.12 + 0.5*msm;
        wq    = al_wander_val(&x->wnd, 0);
        wdr   = al_wander_val(&x->wnd, 1);
        wfold = al_wander_val(&x->wnd, 2);
        wloss = al_wander_val(&x->wnd, 3);

        /* =============================================================
         * MESH ENGINE (re-struck at f0) -- copied from alien_membrane~
         * ============================================================= */
        c2 = (float)tension;
        if (c2 > 0.47f) c2 = 0.47f;     /* strictly below CFL (0.5)       */
        if (c2 < 0.f)   c2 = 0.f;

        inc = f0 / sr;
        if (!(inc >= 0.0 && inc < 1.0)) inc = 0.0;
        x->x_phase += inc;
        if (x->x_phase >= 1.0) {
            x->x_phase -= floor(x->x_phase);
            /* bipolar (DC-free) fixed-energy strike */
            x->x_u[IDX(x, si, sj)] += 0.7f;
            x->x_v[IDX(x, si, sj)] -= 0.7f;
        }

        for (i = 0; i < g; i++) {
            for (j = 0; j < g; j++) {
                int id = IDX(x, i, j);
                float up, dn, lf, rt, lap, nv;

                if (i == 0 || i == g-1 || j == 0 || j == g-1) {
                    if (!bound) { x->x_v[id] = 0.f; continue; }
                }
                up = (i > 0)   ? x->x_u[IDX(x, i-1, j)] : x->x_u[id];
                dn = (i < g-1) ? x->x_u[IDX(x, i+1, j)] : x->x_u[id];
                lf = (j > 0)   ? x->x_u[IDX(x, i, j-1)] : x->x_u[id];
                rt = (j < g-1) ? x->x_u[IDX(x, i, j+1)] : x->x_u[id];

                lap = up + dn + lf + rt - 4.f * x->x_u[id];
                nv  = (2.f * x->x_u[id] - x->x_v[id] + c2 * lap) * damp_step;

                if (mx_bad(nv)) nv = 0.f;
                if (nv >  8.f) nv =  8.f;
                if (nv < -8.f) nv = -8.f;
                x->x_v[id] = mx_denorm(nv);
            }
        }
        {
            int nc = g * g;
            for (i = 0; i < nc; i++) {
                float tt = x->x_u[i];
                x->x_u[i] = x->x_v[i];
                x->x_v[i] = tt;
            }
        }

        y = x->x_u[IDX(x, pi, pj)];
        if (mx_bad(y)) y = 0.f;

        /* top-octave low-pass (brain drifts loss to keep it tonal/evolving) */
        {
            double lc = 1.0 - (loss + depth*(wloss-0.5)*0.3) * 0.9;
            if (lc < 0.02) lc = 0.02; if (lc > 1.0) lc = 1.0;
            x->x_lp += lc * (y - x->x_lp);
        }
        if (mx_bad((float)x->x_lp)) x->x_lp = 0.0;
        if (fabs(x->x_lp) < 1e-20) x->x_lp = 0.0;

        /* mesh DC blocker */
        {
            double dcin = x->x_lp;
            double dcout = dcin - x->x_dc_x1 + 0.995 * x->x_dc_y1;
            if (mx_bad((float)dcout)) dcout = 0.0;
            if (fabs(dcout) < 1e-20) dcout = 0.0;
            x->x_dc_x1 = dcin;
            x->x_dc_y1 = dcout;
            y = (float)dcout;
        }
        if (mx_bad(y)) y = 0.f;

        /* mesh AGC: normalise the wild size/damping level swing to nominal */
        {
            double a = fabs((double)y);
            if (a > x->x_env) x->x_env += 0.0006 * (a - x->x_env);
            else              x->x_env += 0.0001 * (a - x->x_env);
            if (mx_bad((float)x->x_env) || x->x_env < 0.0) x->x_env = 0.0;
        }
        {
            double gain = 0.25 / (x->x_env + 0.06);
            if (gain > 1.5) gain = 1.5;
            mesh = (double)y * gain;
        }
        if (!(mesh > -1e6 && mesh < 1e6)) mesh = 0.0;

        /* =============================================================
         * BASS CHAIN
         * ============================================================= */

        /* crossover highpass so the sub owns the lows */
        hp = hpa*(x->hpy + mesh - x->hpx); x->hpx = mesh; x->hpy = hp;
        mesh = hp;

        /* ANGRY: drive + gentle fold (kept modest -> stays CLEAR, not noisy) */
        drv  = 0.12 + msm*0.30 + depth*wdr*0.18;
        mesh = al_drive(mesh, al_clamp(drv, 0.0, 0.75));
        fold = 0.05 + msm*0.20 + depth*(wfold-0.4)*0.20;
        mesh = al_lerp(mesh, al_fold(mesh, al_clamp(fold, 0.0, 0.6)),
                       0.35 + 0.35*msm);

        /* SQUELCHY WOB: resonant ladder, cutoff is the MACRO (wide sweep) */
        cut = 60.0 * pow(2.0, 0.6 + msm*5.4);
        cut = al_clamp(cut, 45.0, 9000.0);
        res = 0.35 + 0.42*msm + 0.15*depth*wq;      /* high-but-controlled Q */
        res = al_clamp(res, 0.0, 0.9);
        mesh = al_ladder_tick(&x->lad, mesh, cut, res, sr);

        /* extra squelch: a resonant bandpass tracking the ladder cutoff */
        {
            double bpv = al_svf_bp(&x->bp, mesh, cut*1.5, 0.3 + 0.5*msm, sr);
            mesh = al_lerp(mesh, bpv, 0.25 + 0.25*msm);
        }

        /* BASS: clean sine sub one octave below (owns the lows) */
        sub = al_sin01(al_phasor_tick(&x->sub, f0*0.5, sr));

        /* ---- mix ---- */
        s = (0.85 - 0.25*msm)*sub + (0.9 + 0.4*msm)*mesh;
        s = al_dc_tick(&x->dcout, s);

        if (!(s > -1e6 && s < 1e6)) {               /* object-level guard */
            s = 0.0;
            al_ladder_init(&x->lad);
            al_svf_init(&x->bp);
            x->hpx = x->hpy = 0.0;
        }

        /* normalise to hit RMS ~ -12 dBFS with peak <= ~0.95 */
        s *= 0.52;
        s = al_softclip(s);
        if (s >  1.5) s =  1.5; else if (s < -1.5) s = -1.5;
        *out++ = (t_sample)s;
    }

    x->msm = msm;
    return (w + 6);
}

static void alien_membranex_dsp(t_alien_membranex *x, t_signal **sp)
{
    x->x_sr = (sp[0]->s_sr > 0) ? (double)sp[0]->s_sr : 44100.0;
    dsp_add(alien_membranex_perform, 5, x,
            sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_membranex_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_membranex *x = (t_alien_membranex *)pd_new(alien_membranex_tilde_class);
    (void)s; (void)argc; (void)argv;
    x->x_g = 16;
    x->x_lastg = -1;
    x->x_lp = 0.0;
    x->x_dc_x1 = 0.0;
    x->x_dc_y1 = 0.0;
    x->x_env = 0.0;
    x->x_phase = 0.0;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100.0;
    memset(x->x_u, 0, sizeof(x->x_u));
    memset(x->x_v, 0, sizeof(x->x_v));

    al_phasor_init(&x->sub);
    al_ladder_init(&x->lad);
    al_svf_init(&x->bp);
    al_dc_init(&x->dcout);
    al_rng_seed(&x->rng, 0x11eab7e5u);
    al_wander_init(&x->wnd, &x->rng, 4);
    x->hpx = x->hpy = 0.0;
    x->msm = 0.0;

    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_membranex_tilde_setup(void)
{
    alien_membranex_tilde_class = class_new(gensym("alien_membranex~"),
        (t_newmethod)alien_membranex_new, 0, sizeof(t_alien_membranex),
        CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_membranex_tilde_class, t_alien_membranex, x_f);
    class_addmethod(alien_membranex_tilde_class, (t_method)alien_membranex_dsp,
                    gensym("dsp"), A_CANT, 0);
}
