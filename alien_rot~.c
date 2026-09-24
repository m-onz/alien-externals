/* alien_rot~.c  --  a self-mutating, disgusting bass
 *
 * A single-cycle WAVETABLE read at f0 (linear interp) is the source of a gross
 * bass -- but the table is CONTINUOUSLY OVERWRITTEN by a distorted version of
 * its own recent output (the xeno "self-writing wavetable" idea).  The waveform
 * therefore evolves out of its own guts: it grows harmonics, tears, and never
 * repeats.  On top of the self-write it is dragged through the gnarl chain --
 * inharmonic ring mod, wavefold, bitcrush, and a short comb "throat" -- and a
 * slow autonomous al_wander "brain" (~8 controls) drifts every parameter so the
 * rot keeps mutating forever.
 *
 *   SELF-WRITE:  the table cell near the read pointer is overwritten each sample
 *   with a blend of its current value and a distorted (fold/ringmod/bitcrush)
 *   version of the recent output, at a mutation rate.  Writes are CLAMPED to
 *   ~[-1.5,1.5] and the table is slowly bled toward zero-mean so it can neither
 *   blow up nor drift to a DC rail.
 *
 *   THE WOB:  inlet 1 (macro 0..100) is the "gross factor", read PER SAMPLE and
 *   mapped to mutation amount + fold/ring/crush depth, so a fast LFO into inlet 1
 *   heaves the gnarl with no zipper.
 *
 *   THE BRAIN:  an al_wander at a slow autonomous rate drifts mutation rate,
 *   ring frequency, fold, crush and comb.  Its depth scales with the macro, so
 *   at high macro the voice mutates hardest.
 *
 *   inlet 0   signal/float   fundamental in Hz  (CLASS_MAINSIGNALIN)
 *   inlet 1   signal/float   MACRO 0..100 -- the "gross factor", per-sample
 *   outlet 0  signal
 *
 * SAFETY (build uses -ffast-math -> magnitude clamps, not isnan/isinf):
 *   every table write is clamped ~[-1.5,1.5]; the table is DC-blocked via a slow
 *   mean-bleed so it cannot drift to a rail; comb feedback < 1; f0 clamped >=
 *   0.01; final object-level guard `if(!(s>-1e6 && s<1e6)) s=0;` then bounded
 *   ~[-1.5,1.5] and scaled near unity (~0.85).  Gross/aliased/HISS/RAIL is the
 *   point -- but it stays FINITE and BOUNDED, never NaN/Inf or full-scale trash.
 *
 * build:
 *   (macOS: -bundle -undefined dynamic_lookup -o alien_rot~.pd_darwin)
 */

#include "m_pd.h"
#include "alien.h"

#define ROT_LEN   1024          /* single-cycle wavetable (power of two)   */
#define ROT_MASK  (ROT_LEN - 1)
#define CLEN      8192          /* comb "throat" delay line (power of two) */
#define CMASK     (CLEN - 1)

static t_class *alien_rot_tilde_class;

typedef struct _alien_rot {
    t_object x_obj;
    t_float  x_f;               /* scalar fallback for inlet 0             */
    double   x_sr;

    /* ---- self-writing wavetable ---- */
    double   table[ROT_LEN];
    double   rpos;              /* fractional read position 0..LEN         */
    double   mean;             /* running table mean estimate (bleed)     */

    /* ---- gnarl chain state ---- */
    double   x_out1;           /* previous output (feedback / self-write) */
    double   x_phring;         /* inharmonic ring-mod osc phase 0..1      */
    double   x_shval;          /* bitcrush sample-and-hold                */
    double   x_shcnt;
    double   x_buf[CLEN];      /* comb line                               */
    int      x_wi;

    /* ---- brain + smoothing ---- */
    al_rng    rng;
    al_wander brain;           /* slow autonomous modulation brain        */
    al_glide  gmac;            /* smoothed macro (only for slow uses)     */
    al_dc     dc;              /* output DC blocker                       */
} t_alien_rot;

/* interpolated read from the comb line, `d` samples behind the write head */
static inline double rot_delread(t_alien_rot *x, double d)
{
    double rp, fr; int i0, i1;
    if (d < 1.0)        d = 1.0;
    if (d > CLEN - 2.0) d = CLEN - 2.0;
    rp = (double)x->x_wi - d;
    while (rp < 0.0) rp += (double)CLEN;
    i0 = (int)rp; fr = rp - (double)i0;
    i1 = (i0 + 1) & CMASK; i0 &= CMASK;
    return x->x_buf[i0] * (1.0 - fr) + x->x_buf[i1] * fr;
}

static t_int *alien_rot_perform(t_int *w)
{
    t_alien_rot *x  = (t_alien_rot *)(w[1]);
    t_sample *fin   = (t_sample *)(w[2]);   /* inlet 0: fundamental Hz */
    t_sample *min   = (t_sample *)(w[3]);   /* inlet 1: macro 0..100   */
    t_sample *out   = (t_sample *)(w[4]);
    int nsamp       = (int)(w[5]);

    double sr    = x->x_sr;
    double *tab  = x->table;
    double rpos  = x->rpos;
    double mean  = x->mean;
    double out1  = x->x_out1;
    double phring= x->x_phring;
    double shval = x->x_shval, shcnt = x->x_shcnt;

    al_denorm_off();

    while (nsamp--) {
        double f0  = (double)(*fin++);
        double mac = (double)(*min++);
        double m, s;

        /* ---- clamps ---- */
        if (!(f0 > 0.0)) f0 = 0.01;
        if (f0 < 0.01)   f0 = 0.01;
        if (f0 > sr * 0.45) f0 = sr * 0.45;

        /* ---- macro -> gross factor 0..1, per sample (the WOB) ---- */
        m = al_param100(mac);
        if (!(m >= 0.0 && m <= 1.0)) m = 0.0;

        /* ---- slow autonomous BRAIN: drift every parameter ----
         * rate rises a little with the macro so it mutates harder when gross. */
        al_wander_tick(&x->brain, &x->rng, 0.9 + 6.0 * m, sr);
        double bdepth = 0.15 + 0.85 * m;      /* brain influence scales w/ macro */
        double b0 = al_wander_val(&x->brain, 0);
        double b1 = al_wander_val(&x->brain, 1);
        double b2 = al_wander_val(&x->brain, 2);
        double b3 = al_wander_val(&x->brain, 3);
        double b4 = al_wander_val(&x->brain, 4);
        double b5 = al_wander_val(&x->brain, 5);
        double b6 = al_wander_val(&x->brain, 6);
        double b7 = al_wander_val(&x->brain, 7);

        /* per-stage depths: macro sets the floor, brain drifts around it */
        double mutate = al_clamp(0.05 + m * 0.85 + bdepth * (b0 - 0.5) * 0.6, 0.0, 1.0);
        double foldd  = al_clamp(0.10 + m * 0.80 + bdepth * (b1 - 0.5) * 0.7, 0.0, 1.0);
        double ringd  = al_clamp(0.05 + m * 0.75 + bdepth * (b2 - 0.5) * 0.7, 0.0, 1.0);
        double crushd = al_clamp(m * 0.85 + bdepth * (b3 - 0.5) * 0.6, 0.0, 1.0);
        double combd  = al_clamp(m * 0.80 + bdepth * (b4 - 0.5) * 0.6, 0.0, 1.0);

        /* inharmonic ring ratio, drifted by the brain (never integer) */
        double ratio = 1.5 + 2.4142135623730951 * (0.4 + 0.9 * b5); /* ~2.5..3.7 */

        /* two more brain controls drift the write blend + mean-bleed rate */
        double blendw = al_clamp(0.35 + 0.4 * (b6 - 0.5), 0.0, 1.0);
        double bleedr = 0.01 + 0.03 * b7;

        /* ---- READ: phasor over the table at f0 (linear interp) ---- */
        double rinc = (double)ROT_LEN * f0 / sr;
        int ri  = (int)rpos;
        double rf = rpos - ri;
        int ri0 = ri & ROT_MASK;
        int ri1 = (ri + 1) & ROT_MASK;
        double src = al_lerp(tab[ri0], tab[ri1], rf);
        rpos += rinc;
        while (rpos >= (double)ROT_LEN) rpos -= (double)ROT_LEN;
        if (!(src > -8.0 && src < 8.0)) src = 0.0;

        /* ---- GNARL the read value (this is also what gets written back) ---- */
        double g = src;

        /* inharmonic ring mod: pitch-tracked, non-integer multiple of f0 */
        double dtring = f0 * ratio / sr; if (dtring > 0.45) dtring = 0.45;
        phring += dtring; if (phring >= 1.0) phring -= floor(phring);
        double rm = al_sin01(phring);
        g = al_ringmod(g, rm, ringd);

        /* wavefolder (brain also nudges the fold amount) */
        g = al_fold(g, foldd);

        /* bitcrush + sample-rate reduction */
        {
            double bits   = 12.0 - 10.0 * crushd;        /* 12 .. 2 bits    */
            double stride = 1.0 + 24.0 * crushd;         /* 1 .. 25 hold    */
            shcnt -= 1.0;
            if (shcnt <= 0.0) { shval = g; shcnt += stride; }
            double q = al_bitcrush(shval, bits);
            if (!(q > -8.0 && q < 8.0)) q = 0.0;
            g = al_lerp(g, q, crushd);
        }

        /* keep the distorted value bounded before it feeds back into the table */
        g = al_softclip(g * (1.0 + 0.6 * foldd));
        if (!(g > -8.0 && g < 8.0)) g = 0.0;

        /* ---- SELF-WRITE: overwrite the cell near the read pointer ----
         * blend current cell with the distorted output; mutate amount = mutate.
         * Clamp the write and bleed toward the running mean (kills DC drift). */
        {
            int wi = ri0;                      /* write at the read cell     */
            double cur = tab[wi];
            double drift = al_lerp(out1, g, blendw); /* blend of last out + gnarl */
            double nv = al_lerp(cur, drift, mutate * (0.02 + 0.30 * mutate));
            /* mean-bleed: pull the written value gently toward zero-mean     */
            nv -= bleedr * mean;
            if (!(nv > -8.0 && nv < 8.0)) nv = 0.0;
            nv = al_clamp(nv, -1.5, 1.5);
            tab[wi] = nv;
            /* running mean estimate over the table for the bleed above       */
            mean += 0.0005 * (nv - mean);
        }

        /* ---- COMB "throat" tuned near the period of f0 ---- */
        {
            double period = sr / f0;
            double fb  = 0.45 + 0.45 * combd;   /* up to 0.90 (< 1)          */
            if (period < 4.0)        period = 4.0;
            if (period > CLEN - 4.0) period = CLEN - 4.0;
            double combsig = rot_delread(x, period);
            double wr = g + fb * al_softclip(combsig);
            if (!(wr > -8.0 && wr < 8.0)) wr = 0.0;
            x->x_buf[x->x_wi] = wr;
            x->x_wi = (x->x_wi + 1) & CMASK;
            g = al_lerp(g, combsig, combd);
        }

        /* ---- drive + output guard ---- */
        s = al_softclip(g * (0.9 + 1.3 * m));
        out1 = s;                              /* feed back into self-write   */

        s = al_dc_tick(&x->dc, s);

        /* object-level final safety guard */
        if (!(s > -1e6 && s < 1e6)) s = 0.0;
        if (s >  1.5) s =  1.5;
        if (s < -1.5) s = -1.5;
        *out++ = (t_sample)(s * 0.85 * 0.41);
    }

    x->rpos = rpos;
    x->mean = mean;
    x->x_out1 = out1;
    x->x_phring = phring;
    x->x_shval = shval; x->x_shcnt = shcnt;
    return (w + 6);
}

static void alien_rot_dsp(t_alien_rot *x, t_signal **sp)
{
    if (sp[0]->s_sr > 1000) x->x_sr = sp[0]->s_sr;
    dsp_add(alien_rot_perform, 5, x,
            sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

static void *alien_rot_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_rot *x = (t_alien_rot *)pd_new(alien_rot_tilde_class);
    int i; (void)s; (void)argc; (void)argv;   /* A_GIMME ctor ignoring args */

    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100.0;

    /* seed the table with a sine so it has something to mutate */
    for (i = 0; i < ROT_LEN; i++)
        x->table[i] = sin(AL_2PI * (double)i / (double)ROT_LEN);
    x->rpos = 0.0; x->mean = 0.0;
    x->x_out1 = 0.0; x->x_phring = 0.37;
    x->x_shval = 0.0; x->x_shcnt = 0.0;
    memset(x->x_buf, 0, sizeof(x->x_buf));
    x->x_wi = 0;

    al_rng_seed(&x->rng, 0xB0BBA5Eu);
    al_wander_init(&x->brain, &x->rng, 8);
    al_glide_init(&x->gmac, 0.0);
    al_dc_init(&x->dc);

    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_rot_tilde_setup(void)
{
    alien_rot_tilde_class = class_new(gensym("alien_rot~"),
        (t_newmethod)alien_rot_new, 0, sizeof(t_alien_rot),
        CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_rot_tilde_class, t_alien_rot, x_f);
    class_addmethod(alien_rot_tilde_class, (t_method)alien_rot_dsp,
                    gensym("dsp"), A_CANT, 0);
}
