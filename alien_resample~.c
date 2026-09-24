/* alien_resample~.c  --  serial-resampling / feedback-distortion bass
 *
 * A real-time emulation of the modern "resampling" production workflow: bounce
 * a bass, distort it, bitcrush it, pitch it, re-sample it -- then do it again,
 * generation after generation, until the artifacts themselves become the
 * instrument. Here that whole loop runs live, per sample, inside a short
 * internal buffer that carries the signal around a FEEDBACK ring. Each pass
 * through the ring re-processes the buffered signal and re-buffers it, so the
 * artifacts COMPOUND like real generations of re-bouncing.
 *
 * SOURCE:  a fat bass osc at f0 -- band-limited saw + square body plus a clean
 * sine SUB an octave down for weight.
 *
 * THE RESAMPLING RING (the sound):  a short delay/buffer holds the signal.
 * Every sample we read it back at a MODULATED rate (the "re-pitch" of a
 * resample), run the read value through the generational chain
 *
 *     drive/tanh  ->  wavefold  ->  bitcrush + sample-rate-reduce (hold)
 *
 * mix a little fresh source in, and write the result back into the buffer with
 * a feedback gain strictly < 1. Because the read rate is not 1.0 the buffer is
 * literally re-pitched every lap, so pitch-shift artifacts (aliasing, spectral
 * smearing, sub-octave beating) accumulate. The DRY source is blended with the
 * re-processed ring at the output.
 *
 * THE MACRO (inlet 1, 0..100, read PER SAMPLE, smoothed):  drives the whole
 * chain DEPTH at once -- feedback amount, crush amount, the pitch-shift ratio,
 * and fold/drive. Feeding an LFO or S&H into inlet 1 PUMPS the resampling: the
 * artifact texture heaves and evolves. The movement is the evolving RESAMPLED
 * ARTIFACT, NOT a filter sweep.
 *
 * THE BRAIN (al_wander, slow autonomous):  gently drifts the pitch-shift ratio,
 * crush and fold so successive generations never land the same way -- endless
 * non-repeating generational drift on a held note.
 *
 *   inlet 0   signal/float   fundamental in Hz        (CLASS_MAINSIGNALIN)
 *   inlet 1   signal/float   MACRO 0..100 -- pumps the resampling, per sample
 *   outlet 0  signal
 *
 * SAFETY (build uses -ffast-math -> magnitude clamps, NOT isnan/isinf):
 *   this is a feedback loop, so it is guarded hard. Loop feedback is strictly
 *   < 1 (<= ~0.92). Every buffer write is DC-blocked (so the loop cannot drift
 *   to a rail), finite-guarded `if(!(s>-1e6 && s<1e6)) s=0;`, and clamped to
 *   ~[-1.5,1.5] then denormal-flushed. Read/write indices are held in bounds.
 *   f0 is clamped to [0.01, 0.45*sr]. On any non-finite the ring buffer + loop
 *   state are reset to zero. A final object-level guard bounds the output to
 *   ~[-1.5,1.5]. Artifact-rich aliased grime is the GOAL; NaN/Inf/full-scale
 *   garbage is not.
 *
 * build (macOS): -bundle -undefined dynamic_lookup -o alien_resample~.pd_darwin
 */

#include "m_pd.h"
#include "alien.h"

#define RS_LEN   16384          /* resampling ring buffer (power of two)  */
#define RS_MASK  (RS_LEN - 1)

static t_class *alien_resample_class;

typedef struct _alien_resample {
    t_object   x_obj;
    t_float    x_f;             /* scalar fallback for inlet 0            */
    double     x_sr;

    /* ---- source ---- */
    al_phasor  saw;             /* band-limited saw/square body           */
    al_phasor  sub;             /* clean sine sub-octave                  */
    al_phasor  oct;            /* clean octave-up "resample" layer        */
    al_phasor  suboct;         /* extra sub two octaves down (weight)     */

    /* ---- the resampling ring (short feedback delay, read at UNITY) ---- */
    double     buf[RS_LEN];     /* the internal "bounce" buffer           */
    int        wr;             /* write head into the ring               */
    double     shval;          /* bitcrush / sample-rate-reduce hold      */
    double     shcnt;
    double     srr;            /* sample-rate-reduce anti-alias lowpass   */
    double     loopz;          /* Karplus-style damping LP inside loop    */
    al_dc      loopdc;         /* DC block INSIDE the loop (anti-rail)    */

    /* ---- brain + smoothing ---- */
    al_rng     rng;
    al_wander  brain;          /* slow generational drift                */
    al_dc      outdc;          /* output DC blocker                       */
    double     msm;            /* smoothed macro (anti-zipper)            */
} t_alien_resample;

/* interpolated read from the ring, `d` samples behind the write head */
static inline double rs_read(t_alien_resample *x, double d)
{
    double rp, fr; int i0, i1;
    if (d < 1.0)          d = 1.0;
    if (d > RS_LEN - 2.0) d = RS_LEN - 2.0;
    rp = (double)x->wr - d;
    while (rp < 0.0) rp += (double)RS_LEN;
    i0 = (int)rp; fr = rp - (double)i0;
    i1 = (i0 + 1) & RS_MASK; i0 &= RS_MASK;
    return x->buf[i0] * (1.0 - fr) + x->buf[i1] * fr;
}

static t_int *alien_resample_perform(t_int *w)
{
    t_alien_resample *x = (t_alien_resample *)(w[1]);
    t_sample *fin = (t_sample *)(w[2]);   /* inlet 0: fundamental Hz */
    t_sample *min = (t_sample *)(w[3]);   /* inlet 1: macro 0..100   */
    t_sample *out = (t_sample *)(w[4]);
    int nsamp     = (int)(w[5]);

    double sr    = x->x_sr;
    double shval = x->shval, shcnt = x->shcnt;
    double srr   = x->srr, loopz = x->loopz;
    double msm   = x->msm;
    double smc   = 1.0 - exp(-AL_2PI * 30.0 / sr);   /* macro smoothing coef */

    al_denorm_off();

    while (nsamp--) {
        double f0  = (double)(*fin++);
        double mac = (double)(*min++);
        double m, s;

        /* ---- clamps ---- */
        if (!(f0 > 0.0)) f0 = 0.01;
        if (f0 < 0.01)   f0 = 0.01;
        if (f0 > sr * 0.45) f0 = sr * 0.45;

        /* ---- macro -> 0..1 per sample, then smoothed (the pump) ---- */
        m = (mac >= 0.0 && mac <= 100.0) ? mac * 0.01 : al_param100(mac);
        if (!(m >= 0.0 && m <= 1.0)) m = 0.0;
        msm += smc * (m - msm);
        if (!(msm >= 0.0 && msm <= 1.0)) msm = al_clamp(msm, 0.0, 1.0);

        /* ---- BRAIN: slow generational drift; rate rises a touch w/ macro --- */
        al_wander_tick(&x->brain, &x->rng, 0.35 + 2.5 * msm, sr);
        double bdepth = 0.15 + 0.85 * msm;
        double b0 = al_wander_val(&x->brain, 0);   /* octave-layer level drift */
        double b1 = al_wander_val(&x->brain, 1);   /* crush drift             */
        double b2 = al_wander_val(&x->brain, 2);   /* fold drift              */
        double b3 = al_wander_val(&x->brain, 3);   /* drive drift             */
        double b4 = al_wander_val(&x->brain, 4);   /* feedback drift          */
        double b5 = al_wander_val(&x->brain, 5);   /* loop-damp drift         */

        /* per-stage depths: macro sets the floor, brain drifts around it.
         * Crush is deliberately musical -- it stays >= 5 bits, never 2-bit hash. */
        double crushd = al_clamp(0.04 + msm * 0.60 + bdepth * (b1 - 0.5) * 0.35, 0.0, 1.0);
        double foldd  = al_clamp(0.05 + msm * 0.40 + bdepth * (b2 - 0.5) * 0.4, 0.0, 1.0);
        double drived = al_clamp(0.10 + msm * 0.50 + bdepth * (b3 - 0.5) * 0.35, 0.0, 1.0);

        /* feedback strictly < 1 and modest: 0.30 .. ~0.72. Low enough that the
         * loop COLORS the bass instead of drifting to broadband noise. */
        double fb = 0.30 + 0.40 * msm + 0.04 * bdepth * (b4 - 0.5);
        if (fb > 0.72) fb = 0.72;
        if (fb < 0.0)  fb = 0.0;

        /* ---- FAT PITCHED BASS SOURCE (the always-pitched anchor) ----
         * clean sine sub an octave down + a deeper sub two octaves down for
         * weight + saw/square body at f0. This DOMINATES so the fundamental is
         * always clear. Plus a cleanly-generated OCTAVE-UP layer that supplies
         * the "resampled / re-pitched octave" character with no clicks. */
        double dt   = f0 / sr;
        double psaw = al_phasor_tick(&x->saw, f0, sr);
        double body = 0.6 * al_saw_bl(psaw, dt) + 0.4 * al_squ_bl(psaw, dt);
        double subv = al_sin01(al_phasor_tick(&x->sub, f0 * 0.5, sr));
        double sub2 = al_sin01(al_phasor_tick(&x->suboct, f0 * 0.25, sr));
        double octu = al_sin01(al_phasor_tick(&x->oct, f0 * 2.0, sr));

        /* the octave-up "resample" layer: level rises with the macro & drifts */
        double octlvl = al_clamp(0.08 + 0.55 * msm + 0.30 * bdepth * (b0 - 0.5), 0.0, 0.85);
        double anchor = 0.9 * subv + 0.35 * sub2 + 0.55 * body;   /* pitched core */
        double src    = anchor + octlvl * octu;
        if (!(src > -8.0 && src < 8.0)) src = 0.0;

        /* ---- FEEDBACK DELAY read at UNITY (one period behind) ----
         * a short ring delayed by exactly one f0 period, read with NO rate
         * change -> pitch-locked, no wrap discontinuity, no glitch dropouts. */
        double period = sr / f0;
        if (period < 8.0)          period = 8.0;
        if (period > RS_LEN - 4.0) period = RS_LEN - 4.0;
        double looped = rs_read(x, period);
        if (!(looped > -8.0 && looped < 8.0)) looped = 0.0;

        /* ---- THE GENERATIONAL CHAIN (runs at unity playback) ----
         * process the injected source + the fed-back generation together. */
        double g = src + fb * looped;

        /* distort / tanh-ish drive */
        g = al_drive(g, drived);

        /* wavefold -- compounds each generation, but gently */
        g = al_fold(g, foldd);

        /* bitcrush + ANTI-ALIASED sample-rate reduction.
         * bits stay musical (>= 5); the SRR-hold is lowpassed so the stepped
         * signal doesn't fold harsh aliases back into the band. */
        {
            double bits   = 12.0 - 7.0 * crushd;     /* 12 .. 5 bits (musical) */
            double stride = 1.0 + 7.0 * crushd;      /* 1 .. 8 sample hold     */
            shcnt -= 1.0;
            if (shcnt <= 0.0) { shval = g; shcnt += stride; }
            double q = al_bitcrush(shval, bits);
            if (!(q > -8.0 && q < 8.0)) q = 0.0;
            /* anti-alias LP on the reduced stream (~sr/6, follows the crush) */
            double aacoef = al_cut2coef(sr / (6.0 + 10.0 * crushd), sr);
            srr += aacoef * (q - srr);
            g = al_lerp(g, srr, crushd);
        }

        /* ---- WRITE BACK INTO THE RING (the re-bounce) ----
         * DAMPING LOWPASS inside the loop (Karplus-style) so highs cannot
         * accumulate into hiss; DC-block inside the loop (anti-rail); softclip
         * + clamp the write. */
        /* stronger damping as the macro rises so highs cannot pile into hiss */
        double dampcoef = al_cut2coef(1200.0 + 3500.0 * (0.5 + 0.5 * b5) - 2600.0 * msm, sr);
        loopz += dampcoef * (g - loopz);           /* loop damping LP          */
        double wr_in = al_dc_tick(&x->loopdc, loopz);
        wr_in = al_softclip(wr_in);
        if (!(wr_in > -1e6 && wr_in < 1e6)) {
            /* non-finite in the feedback path -> reset the ring + loop state */
            memset(x->buf, 0, sizeof(x->buf));
            al_dc_init(&x->loopdc);
            wr_in = 0.0; shval = 0.0; shcnt = 0.0; srr = 0.0; loopz = 0.0;
        }
        wr_in = al_clamp(wr_in, -1.5, 1.5);
        x->buf[x->wr] = wr_in;
        x->wr = (x->wr + 1) & RS_MASK;

        /* ---- OUTPUT: strong DRY pitched anchor + the colored generation ----
         * the anchor keeps the fundamental clear; g (the resampled/re-processed
         * generation) colors it. Dry dominates at low macro, grit grows up. */
        s = (0.80 - 0.18 * msm) * anchor + (0.30 + 0.55 * msm) * g;
        s = al_drive(s, 0.08 + 0.18 * msm);
        s = al_dc_tick(&x->outdc, s);

        /* ---- object-level final guard ---- */
        if (!(s > -1e6 && s < 1e6)) {
            s = 0.0;
            memset(x->buf, 0, sizeof(x->buf));
            al_dc_init(&x->loopdc);
        }
        if (s >  1.5) s =  1.5;
        if (s < -1.5) s = -1.5;
        *out++ = (t_sample)(s * 0.37);
    }

    x->shval = shval; x->shcnt = shcnt;
    x->srr = srr; x->loopz = loopz;
    x->msm = msm;
    return (w + 6);
}

static void alien_resample_dsp(t_alien_resample *x, t_signal **sp)
{
    if (sp[0]->s_sr > 1000) x->x_sr = sp[0]->s_sr;
    dsp_add(alien_resample_perform, 5, x,
            sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

static void *alien_resample_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_resample *x = (t_alien_resample *)pd_new(alien_resample_class);
    (void)s; (void)argc; (void)argv;   /* A_GIMME ctor ignoring args */

    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100.0;

    al_phasor_init(&x->saw); al_phasor_init(&x->sub);
    al_phasor_init(&x->oct); al_phasor_init(&x->suboct);
    memset(x->buf, 0, sizeof(x->buf));
    x->wr = 0;
    x->shval = 0.0; x->shcnt = 0.0;
    x->srr = 0.0; x->loopz = 0.0;
    al_dc_init(&x->loopdc); al_dc_init(&x->outdc);

    al_rng_seed(&x->rng, 0x2E5A11u);
    al_wander_init(&x->brain, &x->rng, 8);
    x->msm = 0.0;

    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_resample_tilde_setup(void)
{
    alien_resample_class = class_new(gensym("alien_resample~"),
        (t_newmethod)alien_resample_new, 0, sizeof(t_alien_resample),
        CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_resample_class, t_alien_resample, x_f);
    class_addmethod(alien_resample_class, (t_method)alien_resample_dsp,
                    gensym("dsp"), A_CANT, 0);
}
