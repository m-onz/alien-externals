/* alien_bitwise~.c  --  bytebeat: integer expressions as oscillators
 *
 * The whole instrument is one expression in a single variable t, a
 * sample counter.  Evaluate it, take the low bits, and that is the
 * waveform.  No oscillator, no filter, no envelope -- the arithmetic
 * IS the timbre.  Harsh, glitchy, rhythmic, and completely unlike
 * anything a subtractive patch produces.  Classic bytebeat formulas
 * like  t*(t>>9|t>>13)  are compiled once to a stack machine, so
 * evaluation is a tight loop with no parsing per sample.
 *
 *   inlet 0   signal/float   rate multiplier: 1 = the base bytebeat clock,
 *                            2 = an octave up.  Signal rate, so the pitch
 *                            can be swept and modulated.
 *   inlet 1   signal/float   MACRO 0..100 (wrapped mod 100) -- one knob that
 *                            morphs the whole voice along a curated path.
 *                            At 0 a sparse, low-bit, mellow low-order
 *                            pattern; sweeping up walks through a bank of
 *                            increasingly dense classic bytebeats while
 *                            raising the word size, engaging interpolation
 *                            grit and finally hard drive, so at 100 it is a
 *                            bright, dense, aliased, saturated bytebeat roar.
 *
 *   outlet 0  signal
 *
 * build:
 *   gcc -shared -fPIC -O2 -o alien_bitwise~.pd_linux alien_bitwise~.c -lm
 *   (macOS: -bundle -undefined dynamic_lookup -o alien_bitwise~.pd_darwin)
 */

#include "m_pd.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAXCODE 256
#define MAXSTK   64

static t_class *alien_bitwise_tilde_class;

/* stack machine opcodes */
enum { OP_PUSH, OP_T, OP_K, OP_R,
       OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD,
       OP_SHL, OP_SHR, OP_AND, OP_OR, OP_XOR,
       OP_NOT, OP_NEG };

typedef struct _ins { int op; long val; } t_ins;

typedef struct _alien_bitwise {
    t_object x_obj;
    t_float  x_f;

    t_ins    x_code[MAXCODE];
    int      x_ncode;

    double   x_sr, x_phase;
    long     x_t;
    double   x_rate;
    int      x_expr;        /* index of currently compiled expression      */
    int      x_bits;        /* last per-block word size (for change detect) */
    double   x_prev, x_cur;
    unsigned x_rng;
} t_alien_bitwise;

/* ------------------------------------------------------------------ */
/* compiler: shunting-yard to a flat stack program                     */
/* ------------------------------------------------------------------ */

static int prec(int op)
{
    switch (op) {
        case OP_MUL: case OP_DIV: case OP_MOD: return 7;
        case OP_ADD: case OP_SUB:              return 6;
        case OP_SHL: case OP_SHR:              return 5;
        case OP_AND:                           return 4;
        case OP_XOR:                           return 3;
        case OP_OR:                            return 2;
        default:                               return 0;
    }
}

static int compile(t_alien_bitwise *x, const char *s)
{
    int opstk[MAXSTK], nop = 0, nc = 0;
    int unary_ok = 1;                 /* a '-' here is negation */
    const char *p = s;

    while (*p) {
        int op = -1, adv = 1;

        if (*p == ' ' || *p == '\t') { p++; continue; }

        if (*p >= '0' && *p <= '9') {
            long v = 0;
            while (*p >= '0' && *p <= '9') { v = v*10 + (*p - '0'); p++; }
            if (nc >= MAXCODE) return 0;
            x->x_code[nc].op = OP_PUSH; x->x_code[nc].val = v; nc++;
            unary_ok = 0;
            continue;
        }
        if (*p == 't' || *p == 'k' || *p == 'r') {
            if (nc >= MAXCODE) return 0;
            x->x_code[nc].op = (*p == 't') ? OP_T : (*p == 'k' ? OP_K : OP_R);
            x->x_code[nc].val = 0; nc++;
            p++; unary_ok = 0;
            continue;
        }
        if (*p == '(') {
            if (nop >= MAXSTK) return 0;
            opstk[nop++] = -2;        /* marker */
            p++; unary_ok = 1;
            continue;
        }
        if (*p == ')') {
            while (nop > 0 && opstk[nop-1] != -2) {
                if (nc >= MAXCODE) return 0;
                x->x_code[nc].op = opstk[--nop]; x->x_code[nc].val = 0; nc++;
            }
            if (nop == 0) return 0;   /* unbalanced */
            nop--;
            p++; unary_ok = 0;
            continue;
        }
        if (*p == '~') {
            if (nop >= MAXSTK) return 0;
            opstk[nop++] = OP_NOT;
            p++; unary_ok = 1;
            continue;
        }

        switch (*p) {
            case '+': op = OP_ADD; break;
            case '-': op = unary_ok ? OP_NEG : OP_SUB; break;
            case '*': op = OP_MUL; break;
            case '/': op = OP_DIV; break;
            case '%': op = OP_MOD; break;
            case '&': op = OP_AND; break;
            case '|': op = OP_OR;  break;
            case '^': op = OP_XOR; break;
            case '<': if (p[1] == '<') { op = OP_SHL; adv = 2; } break;
            case '>': if (p[1] == '>') { op = OP_SHR; adv = 2; } break;
            default: break;
        }
        if (op < 0) return 0;         /* unknown character */

        if (op != OP_NEG) {
            while (nop > 0 && opstk[nop-1] != -2 &&
                   prec(opstk[nop-1]) >= prec(op)) {
                if (nc >= MAXCODE) return 0;
                x->x_code[nc].op = opstk[--nop]; x->x_code[nc].val = 0; nc++;
            }
        }
        if (nop >= MAXSTK) return 0;
        opstk[nop++] = op;
        p += adv;
        unary_ok = 1;
    }
    while (nop > 0) {
        if (opstk[nop-1] == -2) return 0;
        if (nc >= MAXCODE) return 0;
        x->x_code[nc].op = opstk[--nop]; x->x_code[nc].val = 0; nc++;
    }
    if (nc == 0) return 0;
    x->x_ncode = nc;
    return 1;
}

static long run_code(t_alien_bitwise *x, long t, long k)
{
    long st[MAXSTK];
    int sp = 0, i;
    for (i = 0; i < x->x_ncode; i++) {
        t_ins *in = &x->x_code[i];
        long a, b;
        switch (in->op) {
            case OP_PUSH: if (sp < MAXSTK) st[sp++] = in->val; break;
            case OP_T:    if (sp < MAXSTK) st[sp++] = t; break;
            case OP_K:    if (sp < MAXSTK) st[sp++] = k; break;
            case OP_R:
                x->x_rng ^= x->x_rng << 13; x->x_rng ^= x->x_rng >> 17;
                x->x_rng ^= x->x_rng << 5;
                if (sp < MAXSTK) st[sp++] = (long)(x->x_rng & 0xffff);
                break;
            case OP_NOT: if (sp >= 1) st[sp-1] = ~st[sp-1]; break;
            case OP_NEG: if (sp >= 1) st[sp-1] = -st[sp-1]; break;
            default:
                if (sp < 2) break;
                b = st[--sp]; a = st[--sp];
                switch (in->op) {
                    case OP_ADD: st[sp++] = a + b; break;
                    case OP_SUB: st[sp++] = a - b; break;
                    case OP_MUL: st[sp++] = a * b; break;
                    case OP_DIV: st[sp++] = b ? a / b : 0; break;
                    case OP_MOD: st[sp++] = b ? a % b : 0; break;
                    /* shifts are masked: C leaves >=64 undefined */
                    case OP_SHL: st[sp++] = a << (b & 31); break;
                    case OP_SHR: st[sp++] = a >> (b & 31); break;
                    case OP_AND: st[sp++] = a & b; break;
                    case OP_OR:  st[sp++] = a | b; break;
                    case OP_XOR: st[sp++] = a ^ b; break;
                    default: st[sp++] = 0; break;
                }
                break;
        }
    }
    return (sp > 0) ? st[sp-1] : 0;
}

static inline double softclip(double v)
{
    if (v >  3.0) return 1.0;
    if (v < -3.0) return -1.0;
    return v * (27.0 + v*v) / (27.0 + 9.0*v*v);
}

/* a bank of classic bytebeat expressions, ordered from the simplest /
 * sparsest / most mellow (index 0) to the densest / most aggressive
 * (index NBEXPR-1).  The macro walks this list. */
#define NBEXPR 8
static const char *BEXPR[NBEXPR] = {
    "t>>4|t&t>>5|t>>7",                /* 0  sparse glitch, low order   */
    "((t>>1)*(15&t>>7))|t>>4|t>>5",    /* 1  rhythmic                    */
    "(t*5&t>>7)|(t*3&t>>10)",          /* 2  interference                */
    "t*(t>>9|t>>13)",                  /* 3  the canonical one           */
    "(t>>7|t|t>>6)*10+((t>>11)&7)",    /* 4  chirpy                      */
    "t*((t>>12|t>>8)&63&t>>4)",        /* 5  the "42 melody"             */
    "t*(t>>11&t>>8&123&t>>3)",         /* 6  gated, dense                */
    "t*(((t>>12)|(t>>8))&(63&(t>>4)))" /* 7  dense melodic roar          */
};

/* ------------------------------------------------------------------ */

/* normalise a raw macro value (any float) into t in [0,1). */
static inline double macro_norm(double mac)
{
    double t;
    if (!isfinite(mac)) return 0.0;
    t = mac - 100.0 * floor(mac * 0.01);      /* wrap into [0,100)         */
    if (!(t >= 0.0 && t <= 100.0)) t = 0.0;   /* paranoia guard            */
    t *= 0.01;
    if (t < 0.0) t = 0.0;
    if (t >= 1.0) t = 0.999999;               /* keep strictly < 1        */
    return t;
}

/* DISCRETE / expensive params (expr select, word size, interp mode).
 * Derived once per block -- these decide table/compile work and array
 * sizing, so they must not be recomputed per sample. */
static inline void bitwise_macro_discrete(double t, int *expr, int *bits,
                                          int *mode)
{
    int e = (int)(t * (double)NBEXPR);
    if (e < 0) e = 0;
    if (e > NBEXPR - 1) e = NBEXPR - 1;       /* clamp to bank capacity   */
    *expr  = e;
    *bits  = (int)(6.0 + 8.0 * t);            /* 6 -> 14 bit word         */
    if (*bits < 1)  *bits = 1;
    if (*bits > 24) *bits = 24;               /* keep 1<<bits well in long */
    *mode  = (t >= 0.35 && t < 0.85) ? 1 : 0; /* smooth mid, raw at edges */
}

/* CONTINUOUS cheap param (drive) -- fine to evaluate per sample. */
static inline double bitwise_macro_drive(double t)
{
    return (t < 0.7) ? 0.0 : (t - 0.7) / 0.3;   /* grit only up top       */
}

static t_int *alien_bitwise_perform(t_int *w)
{
    t_alien_bitwise *x = (t_alien_bitwise *)(w[1]);
    t_sample *rin = (t_sample *)(w[2]);
    t_sample *min = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    int n = (int)(w[5]);

    /* ---- PER-BLOCK: derive discrete/expensive quantities ONCE from the
     * macro's value at the top of the block.  These select the compiled
     * expression, the word size (array/shift sizing) and interpolation
     * mode.  Doing the shunting-yard compile per sample was the stutter
     * and the source of thrash. */
    double tb   = macro_norm((double)min[0]);
    int    expr, bits, mode;
    long   mask;
    double half, sr;
    bitwise_macro_discrete(tb, &expr, &bits, &mode);

    /* re-compile only on an actual integer expression change */
    if (expr != x->x_expr) {
        if (compile(x, BEXPR[expr])) x->x_expr = expr;
    }
    /* on a word-size change, nudge cached samples so we don't click on a
     * stale scale; cheap, and only when bits actually changes */
    if (bits != x->x_bits) x->x_bits = bits;

    /* bits is clamped to 1..24: 1<<bits and 1<<(bits-1) stay well within
     * a 64-bit long, and (bits-1) >= 0 so the shift amount is valid. */
    mask = (1L << bits) - 1;
    half = (double)(1L << (bits - 1));

    sr = x->x_sr;
    if (!(sr >= 1000.0)) sr = 44100.0;        /* never divide by ~0        */

    while (n--) {
        double mult = *rin++;
        double mac  = *min++;
        double t, drive, inc, y;

        /* cheap continuous param: safe to evaluate per sample */
        t = macro_norm(mac);
        drive = bitwise_macro_drive(t);

        if (!(mult >= 0.0)) mult = 0.0;       /* also catches NaN          */
        if (mult > 64.0) mult = 64.0;
        inc = x->x_rate * mult / sr;
        if (!isfinite(inc)) inc = 0.0;

        x->x_phase += inc;
        while (x->x_phase >= 1.0) {
            long v;
            x->x_phase -= 1.0;
            x->x_t++;
            v = run_code(x, x->x_t, 0);
            x->x_prev = x->x_cur;
            x->x_cur  = ((double)(v & mask) - half) / half;
        }

        y = mode ? (x->x_prev + (x->x_cur - x->x_prev) * x->x_phase)
                 : x->x_cur;
        if (drive > 0.0)
            y = softclip(y * (1.0 + drive * 6.0)) / (1.0 + drive);
        y *= 0.7;

        /* flush denormals, guarantee finite + bounded output */
        if (!isfinite(y)) y = 0.0;
        if (y > 1.5)  y = 1.5;
        if (y < -1.5) y = -1.5;
        if (y < 1e-15 && y > -1e-15) y = 0.0; /* denormal flush            */
        *out++ = (t_sample)(y * 0.53);
    }
    return (w + 6);
}

static void alien_bitwise_dsp(t_alien_bitwise *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    dsp_add(alien_bitwise_perform, 5, x,
            sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */

static void *alien_bitwise_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_bitwise *x = (t_alien_bitwise *)pd_new(alien_bitwise_tilde_class);
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    x->x_phase = 0.0; x->x_t = 0;
    x->x_rate = 8000.0;
    x->x_prev = x->x_cur = 0.0; x->x_rng = 2463534242u;
    x->x_expr = 0;
    x->x_bits = 6;
    compile(x, BEXPR[0]);

    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_bitwise_tilde_setup(void)
{
    alien_bitwise_tilde_class = class_new(gensym("alien_bitwise~"),
        (t_newmethod)alien_bitwise_new, 0, sizeof(t_alien_bitwise),
        CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_bitwise_tilde_class, t_alien_bitwise, x_f);
    class_addmethod(alien_bitwise_tilde_class, (t_method)alien_bitwise_dsp,
                    gensym("dsp"), A_CANT, 0);
}
