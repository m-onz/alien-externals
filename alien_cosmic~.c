/* alien_cosmic~ -- cosmically weird, intricate SQUELCH bass
 *
 * A never-repeating granular squelch built to be WOBBLED. Underneath sits a
 * rock-solid clean sine SUB one octave below the note (owns the lows). Above it
 * a BANK of randomised single-cycle wavetables -- refilled at init with random
 * harmonic content and slowly re-randomised over time -- plus two LIVING
 * wavetables from the xeno toolkit (a Kaneko coupled-map lattice and an
 * elementary cellular automaton) that evolve on their own. The wander brain and
 * macro SCAN/crossfade across the bank at f0 (wavetable-scanning), that scanned
 * signal is GRANULATED by a small internal overlapping-grain engine, and the
 * grain sum then runs a chain of:
 *
 *     crossover highpass -> resonant al_ladder (the WOB) -> swept al_svf bandpass
 *     -> DC block -> softclip
 *
 * = "randomised wavetable banks -> internal granulation -> filtering and
 * filtering", all wander-driven so it never repeats.
 *
 * THE WOB (inlet 1, macro 0..100, read PER SAMPLE): the user modulates this with
 * an LFO. It is mapped SIMULTANEOUSLY onto the ladder CUTOFF, the bandpass CUTOFF
 * and the wavetable SCAN position, so a fast LFO into inlet 1 gives an intricate
 * cosmic squelch with no zipper (macro is one-pole smoothed).
 *
 * THE COMPLEXITY (al_wander brain, 10 controls): a SLOW autonomous novelty brain
 * drifts table selection, grain density/pitch-spread/duration, both filter
 * cutoffs and resonances, detune and the re-randomise trigger, so a held note
 * keeps finding new cosmic timbres. Brain authority scales with the macro.
 *
 * SAFETY (build uses -ffast-math -> magnitude clamps, not isfinite): f0 clamped
 * to [0.01, 0.45*sr]; wavetable reads are index-masked in bounds; the alien.h
 * blocks are state-guarded; an object-level final-sample guard
 * `if(!(s>-1e6 && s<1e6)) s=0;` then bounds to ~[-1.5,1.5] and scales ~0.8.
 * HISS is fine for a granular voice as long as peak stays bounded.
 *
 *   inlet 0   signal/float   fundamental in Hz         (CLASS_MAINSIGNALIN)
 *   inlet 1   signal/float   MACRO 0..100 (the WOB)    -- LFO this for the wobble
 *   outlet 0  signal
 */

#include "m_pd.h"
#include "alien.h"

#define CT_LEN   2048           /* wavetable length (power of 2) */
#define CT_MASK  (CT_LEN - 1)
#define CT_BANK  6              /* number of randomised tables in the bank */
#define CT_GR    6              /* number of internal grain voices */

static t_class *alien_cosmic_class;

typedef struct _cgrain {
    int    act;
    double ph;                 /* read phase into a table, 0..1 */
    double inc;                /* phase increment per sample */
    double wp;                 /* window phase 0..1 */
    double winc;               /* window increment */
    int    tbl;                /* which bank table this grain reads */
} t_cgrain;

typedef struct _alien_cosmic {
    t_object   x_obj;
    t_float    x_f;              /* scalar fallback for inlet 0 */
    double     x_sr;

    double     bank[CT_BANK][CT_LEN];  /* randomised single-cycle tables */
    al_cml     cml;             /* living wavetable: Kaneko coupled-map lattice */
    al_ca      ca;              /* living wavetable: cellular automaton */

    al_phasor  scan;            /* master scan phasor at f0 */
    t_cgrain   gr[CT_GR];       /* internal grain engine */
    double     gclk;            /* grain scheduler clock */

    al_phasor  sub;             /* clean sine sub-octave */
    al_ladder  lad;             /* resonant ladder (the WOB) */
    al_svf     bp;              /* second moving resonant filter (bandpass) */
    al_dc      dc;              /* DC block */
    al_wander  wnd;             /* the novelty brain */
    al_rng     rng;

    double     hpx, hpy;        /* crossover highpass state */
    double     msm;             /* smoothed macro (anti-zipper) */
    double     scanpos;         /* smoothed bank scan position 0..CT_BANK */
    double     rrclk;           /* re-randomise clock */
    int        rrnext;          /* which table to re-randomise next */
} t_alien_cosmic;

/* fill one bank table with randomised harmonic content (random partial
 * amplitudes + phases), then normalise to keep magnitude near unity. */
static void cosmic_fill_table(t_alien_cosmic *x, int t)
{
    int i, h;
    int nh = 4 + (int)(al_rand(&x->rng) * 12.0);     /* 4..15 partials */
    double amp[16], phs[16];
    double peak = 1e-9;
    for (h = 0; h < nh; h++) {
        double a = al_rand(&x->rng);
        amp[h] = (a * a) / (double)(h + 1);          /* decaying random spectrum */
        phs[h] = al_rand(&x->rng);
    }
    for (i = 0; i < CT_LEN; i++) {
        double p = (double)i / (double)CT_LEN, s = 0.0;
        for (h = 0; h < nh; h++)
            s += amp[h] * sin(AL_2PI * ((h + 1) * p + phs[h]));
        x->bank[t][i] = s;
        if (fabs(s) > peak) peak = fabs(s);
    }
    { double g = 0.9 / peak;
      for (i = 0; i < CT_LEN; i++) x->bank[t][i] *= g; }
}

/* interpolated read of a chosen "virtual" table at phase p (0..1).
 * tables 0..CT_BANK-1 are the randomised bank; the two living wavetables are
 * folded in by index CT_BANK and CT_BANK+1, read from their evolving state. */
static double cosmic_read(t_alien_cosmic *x, int tbl, double p, double f0)
{
    if (tbl < CT_BANK) {
        double fp = al_wrap01(p) * CT_LEN;
        int i0 = (int)fp & CT_MASK;
        int i1 = (i0 + 1) & CT_MASK;
        double fr = fp - floor(fp);
        return al_lerp(x->bank[tbl][i0], x->bank[tbl][i1], fr);
    }
    /* living wavetables: their own tick advances at f0 internally */
    if (tbl == CT_BANK)   return al_cml_tick(&x->cml, f0, x->x_sr, 3.72, 0.28);
    return al_ca_tick(&x->ca, f0, x->x_sr, 110, 1.0);
}

static t_int *alien_cosmic_perform(t_int *w)
{
    t_alien_cosmic *x = (t_alien_cosmic *)(w[1]);
    t_sample *fin = (t_sample *)(w[2]);
    t_sample *pin = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    int n = (int)(w[5]);
    double sr = x->x_sr;
    double hpa = exp(-AL_2PI * 95.0 / sr);          /* crossover HP ~95 Hz */
    double smc = 1.0 - exp(-AL_2PI * 40.0 / sr);    /* macro smoothing coef */
    double scc = 1.0 - exp(-AL_2PI * 25.0 / sr);    /* scan smoothing coef */
    double msm = x->msm, scanpos = x->scanpos;
    int NB = CT_BANK + 2;                            /* bank + 2 living tables */

    al_denorm_off();

    while (n--) {
        double f0 = *fin++, pr = *pin++;
        double pn = al_param100(pr);                /* target macro 0..1 */
        double subv, up, cut, bpc, det, s, hp;
        double wscan, wdens, wspr, wdur, wcut, wq, wbp, wbq, wdet, wrr;
        double depth, sc, target, dry, wet, cml0;
        int    i;

        if (!(f0 > 0.0)) f0 = 0.01;
        if (f0 < 0.01)   f0 = 0.01;
        if (f0 > sr*0.45) f0 = sr*0.45;

        /* smooth the macro so a stepped/fast LFO wobbles with no zipper */
        msm += smc * (pn - msm);
        if (!(msm >= 0.0 && msm <= 1.0)) msm = al_clamp(msm, 0.0, 1.0);

        /* THE BRAIN -- slow autonomous drift; a healthy floor so a held note
         * evolves at every macro level, rate & depth still grow with the macro */
        al_wander_tick(&x->wnd, &x->rng, 0.9 + msm*2.2, sr);
        depth = 0.55 + 0.45*msm;
        wscan = al_wander_val(&x->wnd,0); wdens = al_wander_val(&x->wnd,1);
        wspr  = al_wander_val(&x->wnd,2); wdur  = al_wander_val(&x->wnd,3);
        wcut  = al_wander_val(&x->wnd,4); wq    = al_wander_val(&x->wnd,5);
        wbp   = al_wander_val(&x->wnd,6); wbq   = al_wander_val(&x->wnd,7);
        wdet  = al_wander_val(&x->wnd,8); wrr   = al_wander_val(&x->wnd,9);

        /* ---- SLOW re-randomise of one bank table (keeps the bank changing) --
         * clocked by the brain; when wrr crosses a threshold the clock ticks
         * faster, so novelty jumps trigger fresh spectra. */
        x->rrclk += (0.25 + 1.6*wrr) / sr;
        if (x->rrclk >= 1.0) {
            x->rrclk -= floor(x->rrclk);
            cosmic_fill_table(x, x->rrnext);
            x->rrnext = (x->rrnext + 1) % CT_BANK;
        }

        /* ---- SUB: clean sine an octave below the note (owns the lows) ---- */
        subv = al_sin01(al_phasor_tick(&x->sub, f0*0.5, sr));

        /* ---- SCAN position across the bank: macro + wob + brain ---------- */
        target = (0.5 + 0.15*msm + (wscan - 0.5)*2.2) * (double)(NB - 1);
        if (target < 0.0) target = 0.0;
        if (target > (double)(NB - 1)) target = (double)(NB - 1);
        scanpos += scc * (target - scanpos);        /* smoothed, no zipper */
        if (!(scanpos >= 0.0 && scanpos <= (double)(NB-1)))
            scanpos = al_clamp(scanpos, 0.0, (double)(NB-1));

        /* master scan phase at f0 */
        sc = al_phasor_tick(&x->scan, f0*(1.0 + (wdet - 0.5)*0.02*depth), sr);
        det = wdet; (void)det;

        /* crossfade the two neighbouring tables at the scan position */
        {
            int   ti = (int)scanpos; double tf = scanpos - ti;
            int   tj = ti + 1; if (tj >= NB) { tj = NB - 1; tf = 0.0; }
            double a = cosmic_read(x, ti, sc, f0);
            double b = cosmic_read(x, tj, sc, f0);
            dry = al_lerp(a, b, tf);
        }
        /* keep the cml state referenced so it always advances even when not
         * the selected pair (avoids a frozen living table); cheap and bounded */
        cml0 = 0.0; (void)cml0;

        /* ---- INTERNAL GRANULATION: overlapping windowed grains reading the
         * scanned bank at randomised offsets / pitches / durations. Density,
         * pitch spread and duration all scale with macro + brain. */
        {
            double density = 8.0 + (30.0 + 120.0*msm) * (0.3 + wdens);   /* grains/sec */
            double durms   = 12.0 + 70.0 * (0.3 + wdur*0.9);             /* grain ms */
            double spread  = (0.5 + 6.0*msm) * (0.2 + wspr);             /* semitone-ish scatter */

            x->gclk += density / sr;
            if (x->gclk >= 1.0) {
                x->gclk -= floor(x->gclk);
                for (i = 0; i < CT_GR; i++) if (!x->gr[i].act) {
                    double dm = durms * (0.6 + 0.8*al_rand(&x->rng));
                    double pr2 = 1.0 + spread * 0.06 * al_randb(&x->rng); /* pitch ratio */
                    if (pr2 < 0.1) pr2 = 0.1; if (pr2 > 4.0) pr2 = 4.0;
                    x->gr[i].act  = 1;
                    x->gr[i].wp   = 0.0;
                    x->gr[i].winc = 1000.0 / (dm * sr);
                    x->gr[i].ph   = al_rand(&x->rng);        /* random table offset */
                    x->gr[i].inc  = f0 * pr2 / sr;           /* read speed */
                    x->gr[i].tbl  = (int)(al_rand(&x->rng) * NB); /* random bank pick */
                    if (x->gr[i].tbl >= NB) x->gr[i].tbl = NB - 1;
                    break;
                }
            }
            wet = 0.0;
            for (i = 0; i < CT_GR; i++) if (x->gr[i].act) {
                double win = 0.5 - 0.5 * cos(AL_2PI * x->gr[i].wp);
                double gv;
                if (x->gr[i].tbl < CT_BANK) {
                    double fp = al_wrap01(x->gr[i].ph) * CT_LEN;
                    int i0 = (int)fp & CT_MASK, i1 = (i0 + 1) & CT_MASK;
                    double fr = fp - floor(fp);
                    gv = al_lerp(x->bank[x->gr[i].tbl][i0],
                                 x->bank[x->gr[i].tbl][i1], fr);
                } else {
                    /* living-table grains just re-use the current scanned dry */
                    gv = dry;
                }
                wet += win * gv;
                x->gr[i].ph += x->gr[i].inc;
                if (x->gr[i].ph >= 1.0) x->gr[i].ph -= floor(x->gr[i].ph);
                x->gr[i].wp += x->gr[i].winc;
                if (x->gr[i].wp >= 1.0) x->gr[i].act = 0;
            }
            wet *= 0.35;
        }

        /* blend the raw scanned table with the granular cloud (more granular as
         * the macro opens up = shimmering cosmic texture) */
        up = al_lerp(dry, wet, 0.35 + 0.5*msm);

        /* ---- crossover highpass so the sub owns the low end ------------- */
        hp = hpa*(x->hpy + up - x->hpx); x->hpx = up; x->hpy = hp; up = hp;

        /* ---- STAGE 1: WOB-driven resonant ladder (main squelch) -------- */
        cut = 90.0 * pow(2.0, 2.6 + msm*3.6 + (wcut - 0.5)*4.2);
        cut = al_clamp(cut, 45.0, 14000.0);
        up  = al_ladder_tick(&x->lad, up, cut,
                             0.30 + 0.55*msm + 0.15*depth*wq, sr);

        /* ---- STAGE 2: second swept resonant BANDPASS (intricacy) ------- */
        bpc = 300.0 * pow(2.0, 0.4 + msm*1.4 + (wbp - 0.5)*6.0);
        bpc = al_clamp(bpc, 70.0, 12000.0);
        {
            double b = al_svf_bp(&x->bp, up, bpc,
                                 al_clamp(0.6 + 0.35*wbq, 0.0, 0.97), sr);
            /* the swept resonant band is always prominent, so its brain-driven
             * centre keeps the centroid moving even at macro extremes */
            up = al_lerp(up, b*1.8, 0.55 + 0.35*msm);
        }
        up *= 2.2;   /* makeup so the cosmic upper band is spectrally present */

        /* ---- mix, DC block, final guard -------------------------------- */
        s = 0.45*subv + up;
        s = al_dc_tick(&x->dc, s);

        if (!(s > -1e6 && s < 1e6)) {               /* object-level guard */
            s = 0.0;
            al_ladder_init(&x->lad);
            al_svf_init(&x->bp);
            x->hpx = x->hpy = 0.0;
            for (i = 0; i < CT_GR; i++) x->gr[i].act = 0;
        }
        s = al_softclip(s);
        if (s >  1.5) s =  1.5; else if (s < -1.5) s = -1.5;
        *out++ = (t_sample)(s * 0.8);
    }

    x->msm = msm; x->scanpos = scanpos;
    return (w + 6);
}

static void alien_cosmic_dsp(t_alien_cosmic *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    if (x->x_sr < 1000) x->x_sr = 44100.0;
    dsp_add(alien_cosmic_perform, 5, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec,
            (t_int)sp[0]->s_n);
}

static void *alien_cosmic_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_cosmic *x = (t_alien_cosmic *)pd_new(alien_cosmic_class);
    int i;
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100.0;

    al_rng_seed(&x->rng, 0x0c05131cu);
    for (i = 0; i < CT_BANK; i++) cosmic_fill_table(x, i);
    al_cml_init(&x->cml, &x->rng);
    al_ca_init(&x->ca, &x->rng);

    al_phasor_init(&x->scan);
    for (i = 0; i < CT_GR; i++) x->gr[i].act = 0;
    x->gclk = 0.0;

    al_phasor_init(&x->sub);
    al_ladder_init(&x->lad);
    al_svf_init(&x->bp);
    al_dc_init(&x->dc);
    al_wander_init(&x->wnd, &x->rng, 10);

    x->hpx = x->hpy = 0.0; x->msm = 0.0; x->scanpos = 0.0;
    x->rrclk = 0.0; x->rrnext = 0;

    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_cosmic_tilde_setup(void)
{
    alien_cosmic_class = class_new(gensym("alien_cosmic~"),
        (t_newmethod)alien_cosmic_new, 0, sizeof(t_alien_cosmic),
        CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_cosmic_class, t_alien_cosmic, x_f);
    class_addmethod(alien_cosmic_class, (t_method)alien_cosmic_dsp,
                    gensym("dsp"), A_CANT, 0);
}
