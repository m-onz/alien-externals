/* alien_feednet~.c  --  feedback / nonlinear oscillator network
 *
 * A small network of four oscillators that modulate each other.  Each
 * oscillator's frequency and amplitude are driven by the PREVIOUS-SAMPLE
 * outputs of its neighbours through a modulation MATRIX with feedback.
 * That one-sample delay makes the whole thing a recurrent nonlinear
 * system: it self-generates, drifts, and never quite repeats, giving the
 * writhing metallic "alien" timbres of Autechre / Florian Hecker -- but
 * kept bounded and TUNABLE rather than exploding into white noise.
 *
 *   osc0 (f0)  <--cross FM / AM-->  osc1 (f0*r1)
 *      |   \                       /   |
 *      |    \--- matrix feedback --/   |
 *   osc3 (f0*r3) <-------------> osc2 (f0*r2)
 *
 * Each osc phase increment is f_i * (1 + sum_j M[i][j] * prev[j]) and each
 * osc output is scaled by (1 + AM depth * prev of a neighbour).  The matrix
 * M and the AM routing are slowly drifted by a single al_wander brain so
 * the texture evolves on a held note.  Every prev[] state is hard-clamped
 * each sample, so the loop can shimmer and writhe but cannot diverge.
 *
 *   inlet 0   signal/float   fundamental in Hz (anchors the network base)
 *   inlet 1   signal/float   MACRO 0..100 (wrapped mod 100) -- one knob that
 *                            morphs the whole voice: near 0 a clean, nearly
 *                            static harmonic tone; rising, the cross-mod
 *                            deepens into shimmering, writhing, metallic
 *                            self-evolving textures; near 100 dense alien
 *                            complexity -- always bounded, always pitched.
 *
 *   outlet 0  signal
 *
 * build:
 *   gcc -shared -fPIC -O2 -o alien_feednet~.pd_linux alien_feednet~.c -lm
 *   (macOS: -bundle -undefined dynamic_lookup -o alien_feednet~.pd_darwin)
 */

#include "m_pd.h"
#include "alien.h"
#include <math.h>

#define NOSC 4

static t_class *alien_feednet_tilde_class;

typedef struct _alien_feednet {
    t_object x_obj;
    t_float  x_f;

    double     x_sr;
    al_phasor  x_ph[NOSC];     /* per-oscillator phasors                 */
    double     x_prev[NOSC];   /* previous-sample outputs (clamped)      */

    al_rng     x_rng;
    al_wander  x_wan;          /* slow matrix / routing drift brain      */
    al_dc      x_dc;
} t_alien_feednet;

/* fixed harmonic-ish ratios: base + two mild inharmonic partials.  Small
 * simple ratios keep a clear pitch centre even when the matrix is hot. */
static const double RATIO[NOSC] = { 1.0, 2.0, 3.0, 4.02 };

/* ------------------------------------------------------------------ */

static t_int *alien_feednet_perform(t_int *w)
{
    t_alien_feednet *x = (t_alien_feednet *)(w[1]);
    t_sample *fin = (t_sample *)(w[2]);
    t_sample *min = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    int nsamp = (int)(w[5]);

    double sr  = x->x_sr;
    double nyq = 0.45 * sr;

    al_denorm_off();

    while (nsamp--) {
        double f0  = *fin++;
        double mac = *min++;
        double t   = al_param100(mac);
        double y, mix;
        int i, j;

        if (!(t >= 0.0 && t <= 1.0)) t = 0.0;   /* NaN/inf guard */
        if (!(f0 == f0)) f0 = 55.0;             /* NaN guard */
        f0 = al_clamp(f0, 8.0, nyq);

        /* macro-driven, bounded modulation depths ----------------------
         * fmdep : cross-FM depth (frequency modulation between oscs)
         * amdep : cross-AM depth (amplitude modulation between oscs)
         * a clean near-static tone at t=0 -> deep writhing net at t=1.   */
        /* cross-FM depth rises then eases at the very top so the net stays
         * in the sensitive, ever-evolving regime rather than locking. */
        double fmdep = 0.03 + 0.60 * t - 0.15 * t * t;   /* peak ~mid-top */
        double amdep = 0.45 * t;                          /* 0 -> 0.45    */

        /* drift the matrix so the texture never repeats -- faster as the
         * macro rises so the network visibly writhes on a held note. */
        al_wander_tick(&x->x_wan, &x->x_rng, 0.8 + 6.0 * t, sr);

        /* advance / evaluate the network.  All cross-terms use x_prev[]
         * (one-sample delay), which is hard-clamped below, so every phase
         * increment and every amplitude factor is bounded. */
        mix = 0.0;
        for (i = 0; i < NOSC; i++) {
            /* modulation summed from the OTHER oscillators' prev outputs,
             * weighted by a drifting bipolar matrix entry in ~[-1,1]. */
            double fmod = 0.0, amod = 0.0;
            for (j = 0; j < NOSC; j++) {
                if (j == i) continue;
                double mw = al_wander_bip(&x->x_wan, i * NOSC + j);   /* -1..1 */
                fmod += mw * x->x_prev[j];
                amod += mw * x->x_prev[(j + 1) % NOSC];
            }
            /* bounded FM: increment factor stays well inside (0, ~2) */
            double fmf = 1.0 + fmdep * fmod;
            fmf = al_clamp(fmf, 0.05, 2.5);
            /* slow drift of the ratio itself (bounded, keeps pitch) breaks
             * any exact periodic lock so the net keeps evolving at the top */
            double det = 1.0 + (0.002 + 0.05 * t) * al_wander_bip(&x->x_wan, i);
            double fi = f0 * RATIO[i] * det * fmf;
            fi = al_clamp(fi, 8.0, nyq);        /* runaway freq -> clamp  */

            double p = al_phasor_tick(&x->x_ph[i], fi, sr);
            double s = al_sin01(p);

            /* bounded AM from a neighbour */
            double amf = 1.0 + amdep * amod;
            amf = al_clamp(amf, 0.0, 2.0);
            s *= amf;

            /* finite guard + hard clamp of the feedback state each sample
             * so the recurrent loop can shimmer but never diverge. */
            if (!(s > -1e6 && s < 1e6)) s = 0.0;   /* reset on non-finite */
            s = al_clamp(s, -1.5, 1.5);
            x->x_prev[i] = s;

            /* weight later (higher) partials down so the base stays pitched */
            mix += s * (0.9 / (1.0 + 0.35 * i));
        }

        /* keep a little of osc0 clean in the mix so a pitch centre always
         * survives even at maximum matrix depth (never pure noise). */
        y = mix * (0.45 + 0.25 * t) + x->x_prev[0] * (0.30 - 0.22 * t);

        /* gentle drive up top for grit, DC-block, softclip */
        if (t > 0.6) y = al_drive(y, (t - 0.6) * 0.8);
        y = al_dc_tick(&x->x_dc, y);
        y = al_softclip(y);

        if (!(y > -1e6 && y < 1e6)) { y = 0.0; al_dc_init(&x->x_dc); }
        y = al_clamp(y, -1.5, 1.5);

        *out++ = (t_sample)(y * 0.48);      /* ~ -12 dBFS nominal */
    }
    return (w + 6);
}

static void alien_feednet_dsp(t_alien_feednet *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    if (x->x_sr < 1000.0) x->x_sr = 44100.0;
    dsp_add(alien_feednet_perform, 5, x,
            sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_feednet_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_feednet *x = (t_alien_feednet *)pd_new(alien_feednet_tilde_class);
    int i;
    (void)s; (void)argc; (void)argv;

    x->x_sr = sys_getsr();
    if (x->x_sr < 1000.0) x->x_sr = 44100.0;

    for (i = 0; i < NOSC; i++) {
        al_phasor_init(&x->x_ph[i]);
        x->x_ph[i].p = i * 0.13;      /* spread starting phases */
        x->x_prev[i] = 0.0;
    }
    al_rng_seed(&x->x_rng, 0x51ee7 + 1u);
    al_wander_init(&x->x_wan, &x->x_rng, NOSC * NOSC);
    al_dc_init(&x->x_dc);

    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_feednet_tilde_setup(void)
{
    alien_feednet_tilde_class = class_new(gensym("alien_feednet~"),
        (t_newmethod)alien_feednet_new, 0, sizeof(t_alien_feednet),
        CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_feednet_tilde_class, t_alien_feednet, x_f);
    class_addmethod(alien_feednet_tilde_class, (t_method)alien_feednet_dsp,
                    gensym("dsp"), A_CANT, 0);
}
