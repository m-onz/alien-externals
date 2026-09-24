/* alien_concat~ -- concatenative / corpus-based micro-montage DRONE
 *
 * A self-contained mini version of descriptor-driven corpus synthesis in the
 * spirit of IRCAM CataRT / MuBu: no audio files, no analysis of external sound.
 * Instead, at init the object SYNTHESISES its own CORPUS of N short single-cycle
 * grain-units, each with a DIFFERENT timbre (varying harmonic content: some
 * sine-ish and dark, some bright/saw, some formant-bumped, some noisy or
 * inharmonic, some wavefolded). For each unit two spectral DESCRIPTORS are
 * measured at build time and stored: a normalised spectral CENTROID (brightness)
 * and a NOISINESS value. Those two numbers are the unit's coordinate in a tiny
 * descriptor space.
 *
 * Playback is a granular / concatenation engine. It plays overlapping windowed
 * units at the fundamental f0 (Hann crossfade so units butt together with no
 * click). Each time a new unit is scheduled the engine SELECTS from the corpus
 * the unit whose (centroid, noisiness) descriptor is NEAREST a moving TARGET.
 * The macro sets that target: sweeping the macro drags the target from
 * dark+smooth toward bright+noisy, so the montage morphs through the corpus --
 * an ever-evolving collage, not a loop. A slow al_wander brain jitters the
 * target and the unit choice so a held note keeps re-selecting and never
 * repeats. Overlap-add is normalised, gently filtered, DC-blocked and softclip.
 *
 *   inlet 0   signal/float   fundamental in Hz  (playback pitch of the units)
 *   inlet 1   signal/float   MACRO 0..100 -- target descriptor: 0 dark/smooth,
 *                            100 bright/noisy; sweeps the montage across the corpus
 *   outlet 0  signal
 *
 * SAFETY (build uses -ffast-math): unit indices masked/clamped in bounds; f0
 * clamped [8, 0.45*sr]; overlap-add divided by the running window sum so it can
 * never rail; Hann crossfade avoids clicks; object-level guard
 * `if(!(s>-1e6 && s<1e6)) s=0;` then bound to ~[-1.5,1.5]. LOUDNESS ~ -12 dBFS.
 *
 * build:
 *   (macOS: -bundle -undefined dynamic_lookup -o alien_concat~.pd_darwin)
 */

#include "m_pd.h"
#include "alien.h"

#define CC_ULEN   1024          /* samples per corpus unit (single cycle) */
#define CC_MASK   (CC_ULEN - 1)
#define CC_N      24            /* number of corpus units */
#define CC_VOICE  4             /* overlapping concatenation voices */

static t_class *alien_concat_class;

typedef struct _ccunit {
    double buf[CC_ULEN];        /* one single-cycle unit waveform */
    double cen;                 /* descriptor: spectral centroid 0..1 (brightness) */
    double noi;                 /* descriptor: noisiness 0..1 */
} t_ccunit;

typedef struct _ccvoice {
    int    act;
    int    unit;                /* which corpus unit this voice plays */
    double ph;                  /* read phase into the unit, 0..1 */
    double inc;                 /* phase increment per sample */
    double wp;                  /* window phase 0..1 */
    double winc;                /* window increment */
} t_ccvoice;

typedef struct _alien_concat {
    t_object   x_obj;
    t_float    x_f;             /* scalar fallback for inlet 0 */
    double     x_sr;

    t_ccunit   corp[CC_N];      /* the synthesised corpus */
    t_ccvoice  vc[CC_VOICE];    /* overlapping concatenation voices */
    double     clk;             /* concatenation scheduler clock */
    int        last;            /* last selected unit (avoid immediate repeat) */

    al_svf     lp;              /* gentle tone filter */
    al_dc      dc;              /* DC block */
    al_wander  wnd;             /* jitter brain */
    al_rng     rng;

    double     msm;             /* smoothed macro 0..1 (anti-zipper) */
    double     tgc, tgn;        /* smoothed target centroid / noisiness */
} t_alien_concat;

/* ---- build one corpus unit with a chosen timbre family, measure its
 * descriptors. Five families spread across the dark/smooth -> bright/noisy
 * space so the corpus densely covers the descriptor plane. */
static void concat_build_unit(t_alien_concat *x, int u)
{
    t_ccunit *U = &x->corp[u];
    int i, h;
    int fam = u % 5;                       /* timbre family */
    double stage = (double)u / (double)(CC_N - 1);   /* 0..1 across corpus */
    double peak = 1e-9;

    /* additive spectrum coefficients (up to 32 harmonics) */
    double amp[32], phs[32];
    int    nh = 1;
    double noisemix = 0.0;                 /* how much white noise to blend */
    double foldamt  = 0.0;                 /* wavefolder amount */
    double inharm   = 0.0;                 /* inharmonic detune of partials */

    for (h = 0; h < 32; h++) { amp[h] = 0.0; phs[h] = al_rand(&x->rng); }

    switch (fam) {
        case 0: /* sine-ish / dark: few low partials, steep rolloff */
            nh = 2 + (int)(stage * 3.0);
            for (h = 0; h < nh; h++) amp[h] = 1.0 / (double)((h + 1) * (h + 1));
            break;
        case 1: /* bright / saw: many partials, 1/n rolloff */
            nh = 8 + (int)(stage * 22.0);
            for (h = 0; h < nh; h++) amp[h] = 1.0 / (double)(h + 1);
            break;
        case 2: /* formant-bumped: a resonant peak somewhere up the series */
            nh = 6 + (int)(stage * 18.0);
            {
                double fc = 2.0 + stage * 14.0;   /* formant harmonic index */
                double bw = 2.0 + stage * 3.0;
                for (h = 0; h < nh; h++) {
                    double d = ((double)(h + 1) - fc) / bw;
                    amp[h] = (0.25 / (double)(h + 1)) + exp(-d * d);
                }
            }
            break;
        case 3: /* noisy / inharmonic: detuned partials + noise blend */
            nh = 6 + (int)(stage * 14.0);
            inharm   = 0.05 + stage * 0.35;
            noisemix = 0.15 + stage * 0.55;
            for (h = 0; h < nh; h++)
                amp[h] = (0.4 + 0.6 * al_rand(&x->rng)) / (double)(h + 1);
            break;
        default: /* folded: moderate spectrum then wavefolded (adds odd harmonics) */
            nh = 3 + (int)(stage * 6.0);
            foldamt = 0.2 + stage * 0.7;
            for (h = 0; h < nh; h++) amp[h] = 1.0 / (double)(h + 1);
            break;
    }
    if (nh < 1)  nh = 1;
    if (nh > 32) nh = 32;

    /* render one cycle */
    for (i = 0; i < CC_ULEN; i++) {
        double p = (double)i / (double)CC_ULEN, s = 0.0;
        for (h = 0; h < nh; h++) {
            double ratio = (double)(h + 1) * (1.0 + inharm * ((h & 1) ? 0.5 : -0.5));
            s += amp[h] * sin(AL_2PI * (ratio * p + phs[h]));
        }
        if (foldamt > 0.0) s = al_fold(s * 0.6, foldamt);
        if (noisemix > 0.0) s = al_lerp(s, al_randb(&x->rng), noisemix);
        U->buf[i] = s;
        if (fabs(s) > peak) peak = fabs(s);
    }
    /* normalise unit to ~unity peak so every unit plays at a comparable level */
    { double g = 0.9 / peak; for (i = 0; i < CC_ULEN; i++) U->buf[i] *= g; }

    /* ---- measure descriptors from the rendered cycle ----
     * centroid: amplitude-weighted mean harmonic index (via one DFT bin loop),
     * normalised into 0..1; noisiness: high-frequency energy / total energy of
     * the first-difference (a cheap spectral-flatness proxy). */
    {
        double num = 0.0, den = 0.0;
        int kmax = 24;
        for (h = 1; h <= kmax; h++) {
            double re = 0.0, im = 0.0, mag;
            for (i = 0; i < CC_ULEN; i++) {
                double a = AL_2PI * (double)h * (double)i / (double)CC_ULEN;
                re += U->buf[i] * cos(a);
                im -= U->buf[i] * sin(a);
            }
            mag = sqrt(re * re + im * im);
            num += (double)h * mag;
            den += mag;
        }
        U->cen = (den > 1e-9) ? (num / den) / (double)kmax : 0.0;
        U->cen = al_clamp(U->cen, 0.0, 1.0);
    }
    {
        double etot = 1e-12, ediff = 0.0, prev = U->buf[CC_ULEN - 1];
        for (i = 0; i < CC_ULEN; i++) {
            double d = U->buf[i] - prev; prev = U->buf[i];
            ediff += d * d;
            etot  += U->buf[i] * U->buf[i];
        }
        /* first-difference energy ratio: high for hiss/inharmonic, low for smooth */
        U->noi = al_clamp((ediff / etot) * 0.15, 0.0, 1.0);
    }
}

/* select the corpus unit whose descriptor is nearest (tc, tn), with jitter,
 * avoiding an immediate literal repeat so the montage keeps stepping. */
static int concat_select(t_alien_concat *x, double tc, double tn, double jit)
{
    int i, best = 0;
    double bestd = 1e18;
    for (i = 0; i < CC_N; i++) {
        double dc = x->corp[i].cen - tc + jit * al_randb(&x->rng) * 0.20;
        double dn = x->corp[i].noi - tn + jit * al_randb(&x->rng) * 0.20;
        double d  = dc * dc + dn * dn;
        if (i == x->last) d += 0.02;           /* soft penalty on repeat */
        if (d < bestd) { bestd = d; best = i; }
    }
    return best;
}

static t_int *alien_concat_perform(t_int *w)
{
    t_alien_concat *x = (t_alien_concat *)(w[1]);
    t_sample *fin = (t_sample *)(w[2]);
    t_sample *min = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    int n = (int)(w[5]);
    double sr = x->x_sr;
    double smc = 1.0 - exp(-AL_2PI * 12.0 / sr);   /* macro smoothing */
    double tgc = x->tgc, tgn = x->tgn, msm = x->msm;

    al_denorm_off();

    while (n--) {
        double f0 = *fin++, pr = *min++;
        double pn = al_param100(pr);
        double wsum = 1e-6, acc = 0.0, s;
        double density, durms, jit;
        double wtgt, wjit;
        int i;

        if (!(f0 > 0.0)) f0 = 8.0;
        if (f0 < 8.0)      f0 = 8.0;
        if (f0 > sr * 0.45) f0 = sr * 0.45;

        msm += smc * (pn - msm);
        if (!(msm >= 0.0 && msm <= 1.0)) msm = al_clamp(msm, 0.0, 1.0);

        /* slow jitter brain */
        al_wander_tick(&x->wnd, &x->rng, 0.5 + msm * 0.9, sr);
        wtgt = al_wander_bip(&x->wnd, 0);      /* target wander -1..1 */
        wjit = al_wander_val(&x->wnd, 1);      /* selection jitter amount */

        /* TARGET descriptor from macro: dark/smooth (low) -> bright/noisy (high).
         * The brain nudges the target so the montage keeps drifting on a hold. */
        {
            double tc = al_clamp(0.05 + 0.90 * msm + wtgt * 0.12, 0.0, 1.0);
            double tn = al_clamp(0.02 + 0.85 * msm + wtgt * 0.10, 0.0, 1.0);
            tgc += 0.002 * (tc - tgc);
            tgn += 0.002 * (tn - tgn);
        }
        jit = 0.3 + 0.7 * wjit;

        /* ---- concatenation scheduler: overlapping units at f0. Each unit is a
         * single cycle, so a grain lasts several cycles (durms) and overlaps its
         * neighbour by half the window = seamless Hann crossfade. */
        density = 14.0 + 26.0 * (0.5 + 0.5 * msm);      /* units/sec */
        durms   = 1000.0 / (density * 0.5);             /* ~2x overlap */
        x->clk += density / sr;
        if (x->clk >= 1.0) {
            x->clk -= floor(x->clk);
            for (i = 0; i < CC_VOICE; i++) if (!x->vc[i].act) {
                int u = concat_select(x, tgc, tgn, jit);
                if (u < 0) u = 0; if (u >= CC_N) u = CC_N - 1;
                x->last = u;
                x->vc[i].act  = 1;
                x->vc[i].unit = u;
                x->vc[i].ph   = 0.0;
                x->vc[i].inc  = f0 / sr;                 /* one cycle == the unit */
                x->vc[i].wp   = 0.0;
                x->vc[i].winc = 1000.0 / (durms * sr);
                break;
            }
        }

        /* ---- overlap-add the active voices with a Hann window, normalised by
         * the running window sum so the montage can never rail. */
        for (i = 0; i < CC_VOICE; i++) if (x->vc[i].act) {
            double win = 0.5 - 0.5 * cos(AL_2PI * x->vc[i].wp);
            int un = x->vc[i].unit;
            double fp, fr, gv; int i0, i1;
            if (un < 0) un = 0; if (un >= CC_N) un = CC_N - 1;
            fp = al_wrap01(x->vc[i].ph) * CC_ULEN;
            i0 = (int)fp & CC_MASK;
            i1 = (i0 + 1) & CC_MASK;
            fr = fp - floor(fp);
            gv = al_lerp(x->corp[un].buf[i0], x->corp[un].buf[i1], fr);
            acc  += win * gv;
            wsum += win;
            x->vc[i].ph += x->vc[i].inc;
            if (x->vc[i].ph >= 1.0) x->vc[i].ph -= floor(x->vc[i].ph);
            x->vc[i].wp += x->vc[i].winc;
            if (x->vc[i].wp >= 1.0) x->vc[i].act = 0;
        }
        s = acc / wsum;                          /* window-normalised overlap-add */

        /* gentle tone filter: opens with the macro so bright units read through */
        {
            double cut = 400.0 * pow(2.0, 1.5 + msm * 4.0);
            cut = al_clamp(cut, 200.0, 15000.0);
            s = al_svf_lp(&x->lp, s, cut, 0.15, sr);
        }
        s = al_dc_tick(&x->dc, s);

        if (!(s > -1e6 && s < 1e6)) {            /* object-level guard */
            s = 0.0;
            al_svf_init(&x->lp);
            for (i = 0; i < CC_VOICE; i++) x->vc[i].act = 0;
        }

        s *= 1.6;                                /* makeup toward -12 dBFS */
        s = al_softclip(s);
        if (s >  1.5) s =  1.5; else if (s < -1.5) s = -1.5;
        *out++ = (t_sample)(s * 0.55);
    }

    x->tgc = tgc; x->tgn = tgn; x->msm = msm;
    return (w + 6);
}

static void alien_concat_dsp(t_alien_concat *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    if (x->x_sr < 1000) x->x_sr = 44100.0;
    dsp_add(alien_concat_perform, 5, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec,
            (t_int)sp[0]->s_n);
}

static void *alien_concat_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_concat *x = (t_alien_concat *)pd_new(alien_concat_class);
    int i;
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100.0;

    al_rng_seed(&x->rng, 0x0c0ca7a5u);
    for (i = 0; i < CC_N; i++) concat_build_unit(x, i);

    for (i = 0; i < CC_VOICE; i++) x->vc[i].act = 0;
    x->clk = 0.0; x->last = 0;

    al_svf_init(&x->lp);
    al_dc_init(&x->dc);
    al_wander_init(&x->wnd, &x->rng, 4);

    x->msm = 0.0; x->tgc = 0.05; x->tgn = 0.02;

    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_concat_tilde_setup(void)
{
    alien_concat_class = class_new(gensym("alien_concat~"),
        (t_newmethod)alien_concat_new, 0, sizeof(t_alien_concat),
        CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_concat_class, t_alien_concat, x_f);
    class_addmethod(alien_concat_class, (t_method)alien_concat_dsp,
                    gensym("dsp"), A_CANT, 0);
}
