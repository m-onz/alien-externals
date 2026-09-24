/* alien_neuro~ -- a modern neurofunk SCREECH bass.
 *
 * Where alien_reese~ is a detuned-saw growl, this is its metallic cousin: the
 * body is FEEDBACK FM (an al_fm2 pair plus a self-modulating operator stack)
 * that screams like a distorted digital modem, layered with a touch of
 * al_cluster supersaw for weight.  That source is then dragged through a
 * MOVING comb/notch, a pseudo-MULTIBAND mid distortion (an al_svf bandpass
 * around ~500 Hz..2 kHz driven hard through al_drive/tanh and recombined), a
 * FORMANT bank for vocal snarl, an al_ladder, and a touch of al_bitcrush for
 * digital grit up top.
 *
 * THE WOB.  inlet 1 (the 0..100 macro) is the movement control -- feed it a
 * signal LFO.  It is read PER SAMPLE (smoothed to kill zipper) and mapped
 * TOGETHER onto: FM index, comb/notch position, mid drive amount, and formant
 * vowel.  A fast LFO on inlet 1 therefore gives one big, coherent, moving
 * metallic growl.
 *
 * THE COMPLEXITY.  An al_wander brain (8 controls) drifts autonomously at a
 * SLOW rate, morphing the FM ratio/index/feedback, notch offset, formant
 * vowel, drive and crush depth -- so the metallic character keeps evolving and
 * never repeats on a held note.  The brain's depth scales up with the macro,
 * so at rest it's fairly stable and at full wob it churns.
 *
 *   inlet 0  signal/float  fundamental in Hz   (CLASS_MAINSIGNALIN)
 *   inlet 1  signal/float  MACRO 0..100 -- the MOVEMENT control (wobble this)
 *   outlet 0 signal
 *
 * Safety (build uses -ffast-math, so magnitude clamps do the work): FM index
 * and feedback are bounded (feedback strictly < 1) so the FM can't diverge;
 * comb feedback strictly < 1 with the delay index clamped in-bounds; f0 clamped
 * to [0.01, 0.45*sr]; every fragile integrator denormal-flushed; the object
 * output is finite-guarded (`if(!(s>-1e6 && s<1e6)) s=0;`) then bounded
 * ~[-1.5,1.5] and scaled near unity (~0.8).  HISS/RAIL is expected for this
 * screech and is not sanitised, but the peak stays bounded and finite.
 */

#include "m_pd.h"
#include "alien.h"

#define DLEN  4096            /* comb delay line length (power of 2) */
#define DMASK (DLEN - 1)

static t_class *alien_neuro_tilde_class;

typedef struct _alien_neuro {
    t_object   x_obj;
    t_float    x_f;            /* scalar fallback for inlet 0 */
    double     x_sr;

    /* ---- source: feedback FM + a self-modulating stack + supersaw body ---- */
    al_fm2     x_fm;           /* 2-op feedback FM pair (the screech)          */
    al_fmop    x_op3;          /* extra self-modulating operator (metallic)   */
    double     x_op3_last;     /* its last output (cross-fed back in)          */
    al_cluster x_clu;          /* supersaw body                                */

    /* ---- moving comb / notch ---- */
    double     x_buf[DLEN];
    int        x_wi;

    /* ---- pseudo-multiband mid distortion ---- */
    al_svf     x_mid;          /* bandpass tap for the mids                     */

    /* ---- formant + ladder + grit ---- */
    al_formants x_form;
    al_ladder  x_lad;

    /* ---- the wander brain (autonomous complexity) ---- */
    al_wander  x_brain;
    al_rng     x_rng;

    /* ---- macro smoothing (kills zipper on a fast LFO) ---- */
    double     x_mac;

    /* ---- DC blocker ---- */
    al_dc      x_dc;
} t_alien_neuro;

/* interpolated read from the comb line, `d` samples behind the write head */
static inline double neuro_combread(t_alien_neuro *x, double d)
{
    double rp, fr;
    int i0, i1;
    if (d < 1.0)        d = 1.0;
    if (d > DLEN - 2.0) d = DLEN - 2.0;
    rp = (double)x->x_wi - d;
    while (rp < 0.0) rp += (double)DLEN;
    i0 = (int)rp;
    fr = rp - (double)i0;
    i1 = (i0 + 1) & DMASK;
    i0 &= DMASK;
    return x->x_buf[i0] * (1.0 - fr) + x->x_buf[i1] * fr;
}

/* flush denormals and force finite; magnitude tests survive -ffast-math */
static inline double neuro_flushd(double v)
{
    if (!(v > -1e12 && v < 1e12)) return 0.0;
    if (v > -1e-20 && v < 1e-20)  return 0.0;
    return v;
}

static t_int *alien_neuro_perform(t_int *w)
{
    t_alien_neuro *x = (t_alien_neuro *)(w[1]);
    t_sample *fin = (t_sample *)(w[2]);
    t_sample *min = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    int n = (int)(w[5]);

    double sr  = x->x_sr;
    double nyq = sr * 0.45;
    if (!(sr > 0.0)) { sr = 44100.0; nyq = sr * 0.45; }

    al_denorm_off();

    double macs = x->x_mac;

    /* macro smoothing: ~250 Hz one-pole (follows an 8 Hz LFO, kills zipper) */
    const double mac_a = 1.0 - exp(-2.0 * M_PI * 250.0 / sr);

    while (n--) {
        double f0   = *fin++;
        double mraw = *min++;
        double mt, s, src, body, band, wide, mid_hot, full;
        double index, fb, ratio, notchhz, drive, vowel, crush, combfb;
        double b_ratio, b_index, b_fb, b_notch, b_vowel, b_drive, b_crush, b_depth;
        double m1, m3;

        /* ---- f0 guard ---- */
        if (!(f0 == f0)) f0 = 0.01;
        if (f0 < 0.01)   f0 = 0.01;
        if (f0 > nyq)    f0 = nyq;

        /* ---- macro: wrap 0..100 -> [0,1), then smooth PER SAMPLE ---- */
        mt = mraw - 100.0 * floor(mraw * 0.01);
        if (!(mt >= 0.0 && mt <= 100.0)) mt = 0.0;
        mt *= 0.01;
        macs += mac_a * (mt - macs);
        if (!(macs >= 0.0)) macs = 0.0;
        if (macs > 1.0) macs = 1.0;

        /* ---- SLOW wander brain: rate & depth scale up with the macro ---- */
        al_wander_tick(&x->x_brain, &x->x_rng, 0.4 + 3.5 * macs, sr);
        b_ratio = al_wander_bip(&x->x_brain, 0);   /* FM ratio drift  -1..1 */
        b_index = al_wander_val(&x->x_brain, 1);   /* FM index        0..1  */
        b_fb    = al_wander_val(&x->x_brain, 2);   /* FM feedback     0..1  */
        b_notch = al_wander_bip(&x->x_brain, 3);   /* notch offset    -1..1 */
        b_vowel = al_wander_val(&x->x_brain, 4);   /* formant vowel   0..1  */
        b_drive = al_wander_val(&x->x_brain, 5);   /* drive           0..1  */
        b_crush = al_wander_val(&x->x_brain, 6);   /* crush depth     0..1  */
        b_depth = 0.15 + 0.85 * macs;              /* brain authority       */

        /* ---- macro -> the four movement targets (swept together) ----
         * these are cheap so they run per sample; that (plus the smoother) is
         * what lets a fast LFO on inlet 1 slide everything with no zipper. */

        /* FM index: the screech intensity.  macro drives it; brain modulates.
         * bounded so the FM can't run away. */
        index = 0.4 + 7.5 * macs * macs;
        index *= (1.0 + b_depth * (b_index - 0.5) * 1.6);
        if (index < 0.0)  index = 0.0;
        if (index > 12.0) index = 12.0;

        /* FM feedback: strictly < 1 so the operators stay bounded. */
        fb = 0.2 + 0.6 * macs;
        fb += b_depth * (b_fb - 0.5) * 0.5;
        if (fb < 0.0)  fb = 0.0;
        if (fb > 0.92) fb = 0.92;

        /* FM ratio: inharmonic, wandered for constantly-shifting metal. */
        ratio = 2.41 + b_depth * b_ratio * 1.6;   /* ~0.8 .. 4.0 */
        if (ratio < 0.5)  ratio = 0.5;
        if (ratio > 6.0)  ratio = 6.0;

        /* moving notch: macro sweeps it; brain nudges it.  Bounded to a sane
         * band so the comb delay stays in-bounds. */
        notchhz = 45.0 * pow(2.0, 6.0 * macs + b_depth * b_notch * 1.2);
        if (notchhz < 20.0)     notchhz = 20.0;
        if (notchhz > nyq*0.9)  notchhz = nyq*0.9;

        /* mid drive amount, formant vowel, crush depth -- all wob + brain. */
        drive = al_clamp(0.35 + 0.55 * macs + b_depth * (b_drive - 0.5) * 0.5, 0.0, 1.0);
        vowel = al_clamp(0.15 + 0.7 * macs + b_depth * (b_vowel - 0.5) * 0.7, 0.0, 1.0);
        crush = 16.0 - (10.0 * macs + b_depth * b_crush * 5.0);   /* 16 -> ~3 */
        if (crush < 3.0)  crush = 3.0;
        if (crush > 16.0) crush = 16.0;

        combfb = 0.55 + 0.35 * macs;
        if (combfb > 0.92) combfb = 0.92;

        /* ================= SOURCE: feedback FM + stack + supersaw ========= */

        /* 2-op feedback FM pair -- the core metallic screech */
        src = al_fm2_tick(&x->x_fm, f0, ratio, index, fb, sr);

        /* an extra self-modulating operator, cross-fed from the pair, at a
         * second inharmonic ratio -- thickens the sideband forest */
        m1 = src;
        m3 = al_fmop_tick(&x->x_op3, f0 * (ratio * 1.53 + 0.5),
                          index * 0.6 * (m1 + 0.7 * x->x_op3_last), fb, sr);
        x->x_op3_last = m3;
        src = src * 0.85 + m3 * 0.5;

        /* a bit of supersaw body underneath for weight (detune widens on wob) */
        body = al_cluster_tick(&x->x_clu, f0, 0.008 + 0.03 * macs, sr);
        s = src + body * (0.5 - 0.2 * macs);

        /* ================= MOVING COMB / NOTCH =========================== */
        {
            double D = sr / notchhz;
            double fb_read = neuro_combread(x, D);
            double wr = s + combfb * fb_read;   /* combfb strictly < 1 */
            if (wr >  4.0) wr =  4.0;
            else if (wr < -4.0) wr = -4.0;
            x->x_buf[x->x_wi] = neuro_flushd(wr);
            x->x_wi = (x->x_wi + 1) & DMASK;
            s = 0.5 * (s - fb_read);            /* feedforward notch */
        }

        /* ============ PSEUDO-MULTIBAND MID DISTORTION ==================== */
        {
            /* bandpass centre wobbles ~500 Hz..2 kHz with the macro */
            double fc = 500.0 * pow(2.0, 2.0 * macs);   /* 500 .. 2000 Hz */
            band = al_svf_bp(&x->x_mid, s, fc, 0.6, sr);
            wide = s - band;
            /* drive the mids hard, recombine over softer-driven full band */
            mid_hot = al_drive(band, al_clamp(drive * 1.3, 0.0, 1.0));
            mid_hot = tanh(mid_hot * (1.0 + drive * 3.0));
            full    = al_drive(wide, drive * 0.4);
            s = mid_hot * 0.8 + full * 0.9;
        }

        /* ============ FORMANT (vocal snarl) ============================= */
        {
            double voc = al_formants_tick(&x->x_form, s, vowel, 0.4 + 0.5 * macs, sr);
            s = s * 0.6 + voc * (0.5 + 0.4 * macs);
        }

        /* ============ LADDER (opens with the wob) ======================= */
        {
            double cut = 220.0 * pow(2.0, 2.0 + 4.2 * macs);
            cut = al_clamp(cut, 80.0, 16000.0);
            s = al_ladder_tick(&x->x_lad, s, cut, 0.2 + 0.45 * macs, sr);
            s = al_clamp(s, -3.0, 3.0);
        }

        /* ============ BITCRUSH (digital grit up top) ==================== */
        s = al_bitcrush(al_clamp(s, -1.5, 1.5), crush);

        /* ---- DC blocker ---- */
        s = al_dc_tick(&x->x_dc, s);

        /* ---- output safety: finite + bounded ~[-1.5,1.5], scale ~0.8 ---- */
        if (!(s > -1e6 && s < 1e6)) s = 0.0;
        s = al_softclip(s * 1.15) * (0.8 * 0.73);
        if (!(s > -1.5 && s < 1.5)) s = 0.0;

        *out++ = (t_sample)s;
    }

    x->x_mac = macs;
    return (w + 6);
}

static void alien_neuro_dsp(t_alien_neuro *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    if (x->x_sr < 1000) x->x_sr = 44100.0;
    dsp_add(alien_neuro_perform, 5, x,
            sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

static void *alien_neuro_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_neuro *x = (t_alien_neuro *)pd_new(alien_neuro_tilde_class);
    (void)s; (void)argc; (void)argv;

    x->x_sr = sys_getsr();
    if (x->x_sr < 1000) x->x_sr = 44100.0;

    al_fm2_init(&x->x_fm);
    al_fmop_init(&x->x_op3);
    x->x_op3_last = 0.0;
    al_cluster_init(&x->x_clu, 5);

    memset(x->x_buf, 0, sizeof(x->x_buf));
    x->x_wi = 0;

    al_svf_init(&x->x_mid);
    al_formants_init(&x->x_form);
    al_ladder_init(&x->x_lad);
    al_dc_init(&x->x_dc);

    al_rng_seed(&x->x_rng, 0x9E37u);
    al_wander_init(&x->x_brain, &x->x_rng, 8);

    x->x_mac = 0.0;

    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_neuro_tilde_setup(void)
{
    alien_neuro_tilde_class = class_new(gensym("alien_neuro~"),
        (t_newmethod)alien_neuro_new, 0, sizeof(t_alien_neuro),
        CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_neuro_tilde_class, t_alien_neuro, x_f);
    class_addmethod(alien_neuro_tilde_class, (t_method)alien_neuro_dsp,
                    gensym("dsp"), A_CANT, 0);
}
