/* alien_logic~.c  --  LFSR / logic-gate "Lunetta" synthesiser
 *
 * Structured digital noise and tones built the way SID, POKEY and CMOS
 * "Lunetta" patches build them: shift registers clocked at an audio rate,
 * their bits XORed back on themselves and cross-wired through logic gates.
 * Where [alien_bitwise~] is ARITHMETIC on a sample counter, this is pure
 * bit/register LOGIC -- taps, feedback, flip-flop divisions -- so it sings
 * with the buzzy, gated, pseudo-random character of vintage silicon rather
 * than smooth math.  A single clock rate sets the pitch and the whole
 * timbre; the register wiring ("circuit") sets the flavour.
 *
 *   inlet 0   signal/float   clock rate in Hz -- the shift/update rate,
 *                            which IS the pitch and grain of the sound
 *   inlet 1   signal/float   MACRO 0..100 (wrapped mod 100) -- one knob that
 *                            morphs the whole voice: at 0 a smooth, mellow
 *                            15-bit LFSR tone (interpolated, few bits, no
 *                            feedback or drive); rising it steps through the
 *                            six register circuits while adding bits,
 *                            feedback perturbation and drive, until at 100 a
 *                            dense, aliased dual-LFSR hash gated hard and
 *                            driven into grit.  It sweeps circuit/bits/
 *                            feedback/drive/mode together on a hand-tuned
 *                            trajectory and reseeds the registers whenever
 *                            the macro crosses into a new circuit.
 *
 *   outlet 0  signal
 *
 * build:
 *   gcc -shared -fPIC -O2 -o alien_logic~.pd_linux alien_logic~.c -lm
 *   (macOS: -bundle -undefined dynamic_lookup -o alien_logic~.pd_darwin)
 */

#include "m_pd.h"
#include <math.h>
#include <string.h>

#define NCIRC 6

static t_class *alien_logic_tilde_class;

typedef struct _alien_logic {
    t_object x_obj;
    t_float  x_f;

    double   x_sr;
    double   x_phase;          /* clock phase for the shift rate */

    unsigned x_lfsr_a;         /* long register  */
    unsigned x_lfsr_b;         /* short register */
    unsigned x_div;            /* flip-flop divider counter */

    double   x_prev, x_cur;    /* last two output values (for interp) */
    double   x_dc0, x_dc1;     /* DC blocker */

    int      x_circuit;        /* current circuit (tracked for re-seed) */
    unsigned x_seed;
    unsigned x_rng;            /* auxiliary noise for feedback perturbation */
} t_alien_logic;

/* ------------------------------------------------------------------ */

static unsigned rn(t_alien_logic *x)
{
    x->x_rng ^= x->x_rng << 13; x->x_rng ^= x->x_rng >> 17;
    x->x_rng ^= x->x_rng << 5;  return x->x_rng;
}

static inline double softclip(double v)
{
    if (v >  3.0) return 1.0;
    if (v < -3.0) return -1.0;
    return v * (27.0 + v*v) / (27.0 + 9.0*v*v);
}

/* count set bits (parity) of the masked taps */
static inline int parity(unsigned v)
{
    v ^= v >> 16; v ^= v >> 8; v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
    return (int)(v & 1u);
}

static void logic_reset(t_alien_logic *x)
{
    /* seed both registers non-zero (an all-zero LFSR is stuck) */
    x->x_lfsr_a = x->x_seed ? x->x_seed : 0xACE1u;
    x->x_lfsr_b = (x->x_seed ^ 0x5A5Au) ? (x->x_seed ^ 0x5A5Au) : 0x7Fu;
    x->x_lfsr_a &= 0x7FFFu; if (!x->x_lfsr_a) x->x_lfsr_a = 0x1u;
    x->x_lfsr_b &= 0x7Fu;   if (!x->x_lfsr_b) x->x_lfsr_b = 0x1u;
    x->x_div = 0;
    x->x_prev = x->x_cur = 0.0;
}

/* advance a Galois-style LFSR of given bit-width using an XOR tap mask,
 * return the new register value (masked) */
static inline unsigned lfsr_step(unsigned reg, unsigned tapmask, unsigned width)
{
    unsigned lsb = reg & 1u;
    reg >>= 1;
    if (lsb) reg ^= tapmask;
    reg &= (1u << width) - 1u;
    if (!reg) reg = 1u;                 /* never fall into the zero lock */
    return reg;
}

/* clock the circuit once, return an output word (unsigned).
 * circ selects the wiring; fb01 is the modulated feedback amount in [0,1];
 * mw is a modulation warp in [0,1]. */
static unsigned logic_clock(t_alien_logic *x, int circ, double fb01, double mw)
{
    unsigned out = 0;
    unsigned a = x->x_lfsr_a, b = x->x_lfsr_b;

    /* optional pseudo-random XOR perturbation into the register feedback */
    unsigned perturb = 0;
    if (fb01 > 0.0 && (double)(rn(x) & 0xffff) / 65536.0 < fb01)
        perturb = (rn(x) & 0x7u);       /* flip a couple of low bits */

    switch (circ) {
        case 1: { /* short 7-bit maximal LFSR (taps 7,6): buzzy pitched noise */
            b = lfsr_step(b ^ perturb, 0x60u, 7);
            out = b;                     /* 7-bit word */
        } break;

        case 2: { /* two LFSRs XORed: metallic ring */
            a = lfsr_step(a ^ perturb, 0x6000u, 15);   /* 15-bit maximal */
            b = lfsr_step(b, 0x60u, 7);                 /* 7-bit maximal */
            out = (a >> 4) ^ (b << 1);
        } break;

        case 3: { /* LFSR gated by /2 /4 flip-flops: rhythmic */
            a = lfsr_step(a ^ perturb, 0x6000u, 15);
            x->x_div++;
            {
                unsigned d2 = (x->x_div >> 1) & 1u;   /* /2 */
                unsigned d4 = (x->x_div >> 2) & 1u;   /* /4 */
                /* AND the register bits with divider phases -> gated bursts */
                out = a;
                if (!d2) out &= 0x0F0Fu;
                if (d4)  out ^= (a >> 3);
            }
        } break;

        case 4: { /* 4-bit LFSR AND long LFSR: sparse glitch */
            a = lfsr_step(a ^ perturb, 0x6000u, 15);
            b = lfsr_step(b, 0xCu, 4);                  /* 4-bit maximal */
            out = a & (b | (b << 4) | (b << 8) | (b << 12));
        } break;

        case 5: { /* dual maximal LFSRs interleaved: dense hash */
            a = lfsr_step(a ^ perturb, 0x6000u, 15);
            b = lfsr_step(b, 0x60u, 7);
            /* interleave bits of the two registers */
            out = ((a & 0x00FFu)) | ((b & 0x00FFu) << 8);
            out ^= (out >> 5);
        } break;

        default: { /* 0: single 15-bit maximal LFSR, low bits -> tone */
            a = lfsr_step(a ^ perturb, 0x6000u, 15);
            out = a;
        } break;
    }

    /* modulation warp: occasionally cross-couple the two registers so the
     * mod audibly bends the divider/feedback relationship */
    if (mw > 0.0) {
        unsigned wshift = (unsigned)(mw * 6.0);         /* 0..6 */
        out ^= (a >> (1 + wshift)) & 0xFu;
        /* parity of a modulated tap folds b back into a slowly */
        if (parity(a & (0x1u << (wshift & 14))))
            b ^= (b << 1) & ((1u << 7) - 1u);
    }

    x->x_lfsr_a = a;
    x->x_lfsr_b = b;
    return out;
}

/* ------------------------------------------------------------------ */

/* map the 0..1 macro onto the whole parameter set (curated morph).
 * circ  : which of the 6 register circuits (discrete, sub-ranged)
 * bits  : output word width 1..16
 * fb    : feedback perturbation amount 0..1
 * drive : soft-clip drive 0..1
 * interp: 1 = linear interpolation (smooth), 0 = stepped (raw, aliased)
 * mw    : intrinsic modulation warp 0..1 */
static inline void logic_macro(double t, int *circ, int *bits, double *fb,
                               double *drive, int *interp, double *mw)
{
    /* six circuits spread across the sweep, darkest/simplest -> densest */
    static const int order[NCIRC] = { 0, 1, 3, 2, 4, 5 };
    int idx = (int)(t * NCIRC);
    if (idx < 0) idx = 0;
    if (idx > NCIRC - 1) idx = NCIRC - 1;
    *circ = order[idx];

    *bits = (int)(4.0 + 11.0 * t);            /* 4 -> 15 : few -> many bits */
    if (*bits < 1)  *bits = 1;
    if (*bits > 16) *bits = 16;

    *fb    = (t < 0.3) ? 0.0 : (t - 0.3) / 0.7;           /* grit builds up  */
    if (*fb > 1.0) *fb = 1.0;
    *drive = (t < 0.4) ? 0.05 : 0.05 + (t - 0.4) / 0.6;   /* mellow -> hot   */
    if (*drive > 1.0) *drive = 1.0;
    *mw    = (t < 0.5) ? 0.0 : (t - 0.5) * 2.0;           /* warp up top     */
    if (*mw > 1.0) *mw = 1.0;

    *interp = (t < 0.55) ? 1 : 0;             /* smooth low, aliased high    */
}

static t_int *alien_logic_perform(t_int *w)
{
    t_alien_logic *x = (t_alien_logic *)(w[1]);
    t_sample *fin = (t_sample *)(w[2]);
    t_sample *min = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    int nsamp = (int)(w[5]);

    double sr = x->x_sr;

    /* ---- PER-BLOCK derivation (never per sample) ----
     * Derive the discrete circuit/bit/mode set ONCE per block from the
     * macro's FIRST sample.  This is what stops the "reset thrash": the
     * circuit index only re-seeds on a genuine integer change, not on the
     * sample-to-sample jitter of a signal sitting on a circuit boundary. */
    double mac0 = (double)min[0];
    double t;
    double fb, drive, mw;
    int circ, bits, interp;
    unsigned mask;
    double half, invhalf;

    /* wrap the macro into [0,100) then normalise to [0,1) */
    if (!isfinite(mac0)) mac0 = 0.0;                 /* NaN/inf guard */
    t = mac0 - 100.0 * floor(mac0 * 0.01);
    if (!(t >= 0.0 && t <= 100.0)) t = 0.0;
    t *= 0.01;
    if (t < 0.0) t = 0.0;
    if (t >= 1.0) t = 0.999999;

    logic_macro(t, &circ, &bits, &fb, &drive, &interp, &mw);

    /* bits is clamped to 1..16 in logic_macro, so these shifts are safe:
     * bits-1 is 0..15, and (1u<<16)-1 fits in unsigned. */
    if (bits < 1)  bits = 1;
    if (bits > 16) bits = 16;
    mask    = (bits >= 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
    half    = (double)(1u << (bits - 1));
    if (half < 1.0) half = 1.0;
    invhalf = 1.0 / half;

    /* re-seed ONLY on an actual integer circuit change */
    if (circ != x->x_circuit) {
        x->x_circuit = circ;
        logic_reset(x);
    }

    while (nsamp--) {
        double f0 = *fin++;
        double y, o;
        (void)*min++;   /* macro consumed per-block; keep pointer in step */

        if (!isfinite(f0)) f0 = 0.0;
        if (f0 < 0.01)     f0 = 0.01;   /* forbid divide-by-zero pitch */
        if (f0 > 16000.0)  f0 = 16000.0;

        x->x_phase += f0 / sr;
        if (x->x_phase >= 1.0) {
            unsigned v;
            x->x_phase -= 1.0;
            if (x->x_phase >= 1.0) x->x_phase = 0.0;
            v = logic_clock(x, circ, fb, mw);
            x->x_prev = x->x_cur;
            x->x_cur  = ((double)(v & mask) - half) * invhalf;   /* -> [-1,1) */
        }

        y = interp ? (x->x_prev + x->x_phase * (x->x_cur - x->x_prev))
                   : x->x_cur;

        /* DC block (register words are not zero-mean) */
        o = y - x->x_dc0 + 0.999 * x->x_dc1;
        x->x_dc0 = y; x->x_dc1 = o;

        if (drive > 0.0)
            o = softclip(o * (1.0 + drive * 6.0)) / (1.0 + drive * 1.5);
        else
            o = softclip(o);

        o *= 0.6;                                   /* nominal near unity */
        if (!isfinite(o)) o = 0.0;                  /* NaN/Inf guard */
        if (o >  1.5) o =  1.5;                      /* hard safety bound */
        if (o < -1.5) o = -1.5;
        *out++ = (t_sample)o;
    }
    return (w + 6);
}

static void alien_logic_dsp(t_alien_logic *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    dsp_add(alien_logic_perform, 5, x,
            sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_logic_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_logic *x = (t_alien_logic *)pd_new(alien_logic_tilde_class);
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    x->x_phase = 0.0; x->x_dc0 = x->x_dc1 = 0.0;
    x->x_circuit = 0;
    x->x_seed = 0xACE1u; x->x_rng = 2463534242u;
    logic_reset(x);

    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);   /* macro */
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_logic_tilde_setup(void)
{
    alien_logic_tilde_class = class_new(gensym("alien_logic~"),
        (t_newmethod)alien_logic_new, 0, sizeof(t_alien_logic),
        CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_logic_tilde_class, t_alien_logic, x_f);
    class_addmethod(alien_logic_tilde_class, (t_method)alien_logic_dsp,
                    gensym("dsp"), A_CANT, 0);
}
