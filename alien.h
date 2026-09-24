/* alien.h -- a header-only, composable DSP toolkit for the alien instruments.
 *
 * One include gives you a kit of small unit-generators abstracted from every
 * external built so far. Everything is `static inline` and self-contained
 * (only <math.h>/<stdint.h>), so each Pd external can #include "alien.h" and
 * compile standalone -- no linking, no shared objects.
 *
 * Design: each block is a tiny struct of state + an `_init` and a `_tick`.
 * You compose an instrument by declaring a few blocks and calling their ticks
 * in a chain, e.g.:
 *
 *     al_phasor osc; al_svf lp; al_dc dc;              // declare
 *     al_phasor_init(&osc); al_svf_init(&lp); al_dc_init(&dc);   // in _new
 *     // in perform, per sample:
 *     double p = al_phasor_tick(&osc, freq, sr);
 *     double s = al_saw_bl(p, freq/sr);               // band-limited saw
 *     s = al_fold(s, fold);                            // wavefolder
 *     s = al_svf_lp(&lp, s, cutoff, res, sr);          // filter
 *     s = al_dc_tick(&dc, s);                          // block DC
 *     out = al_softclip(s);
 *
 * Sections: [core] [osc] [shape] [filter] [env] [delay] [noise] [chaos]
 *           [modal] [spectral] [fdn]
 */

#ifndef ALIEN_H
#define ALIEN_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define AL_PI  3.14159265358979323846
#define AL_2PI 6.28318530717958647692

/* ================================================================= core */

static inline double al_clamp(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
static inline double al_wrap01(double p) { p -= floor(p); return p; }
static inline double al_lerp(double a, double b, double t) { return a + t * (b - a); }
/* Catmull-Rom cubic through a1,a2 with neighbours a0,a3 (t in 0..1) */
static inline double al_cubic(double a0, double a1, double a2, double a3, double t) {
    double t2 = t * t, t3 = t2 * t;
    return 0.5 * ((2.0*a1) + (-a0+a2)*t + (2.0*a0-5.0*a1+4.0*a2-a3)*t2
                  + (-a0+3.0*a1-3.0*a2+a3)*t3);
}
/* smooth, bounded saturator (the house softclip) */
static inline double al_softclip(double v) {
    if (v >  3.0) return 1.0;
    if (v < -3.0) return -1.0;
    return v * (27.0 + v*v) / (27.0 + 9.0*v*v);
}
static inline double al_mtof(double m) { return 440.0 * pow(2.0, (m - 69.0) / 12.0); }
static inline double al_ftom(double f) { return 69.0 + 12.0 * log(f / 440.0) / log(2.0); }

/* map a raw right-inlet value onto 0..1, wrapping any input modulo 100
 * (the xeno convention: 150 -> 0.5, 9000 -> 0.0, -10 -> 0.9) */
static inline double al_param100(double raw) {
    double p = raw - 100.0 * floor(raw / 100.0);
    return p * 0.01;
}

/* xorshift32 RNG */
typedef struct { uint32_t s; } al_rng;
static inline void   al_rng_seed(al_rng *r, uint32_t seed) { r->s = seed ? seed : 2463534242u; }
static inline uint32_t al_rng_u(al_rng *r) {
    uint32_t x = r->s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; r->s = x; return x;
}
static inline double al_rand(al_rng *r)  { return (al_rng_u(r) & 0xffffff) / (double)0x1000000; } /* 0..1 */
static inline double al_randb(al_rng *r) { return al_rand(r) * 2.0 - 1.0; }                        /* -1..1 */
static inline double al_gauss(al_rng *r) { double s=0; int i; for(i=0;i<4;i++) s+=al_randb(r); return s*0.5; }

/* DC blocker (one-pole highpass, R=0.999) */
typedef struct { double x1, y1; } al_dc;
static inline void   al_dc_init(al_dc *d) { d->x1 = d->y1 = 0.0; }
static inline double al_dc_tick(al_dc *d, double x) {
    double y = x - d->x1 + 0.999 * d->y1; d->x1 = x; d->y1 = y; return y;
}

/* one-pole lowpass; coef in 0..1 (see al_cut2coef), higher = brighter */
typedef struct { double z; } al_lop;
static inline void   al_lop_init(al_lop *f) { f->z = 0.0; }
static inline double al_lop_tick(al_lop *f, double x, double coef) { f->z += coef*(x - f->z); return f->z; }
static inline double al_cut2coef(double hz, double sr) {
    double c = 1.0 - exp(-AL_2PI * hz / sr); return c < 0 ? 0 : (c > 1 ? 1 : c);
}

/* ================================================================= osc */

typedef struct { double p; } al_phasor;
static inline void   al_phasor_init(al_phasor *o) { o->p = 0.0; }
/* advance and return phase in 0..1 */
static inline double al_phasor_tick(al_phasor *o, double hz, double sr) {
    o->p += hz / sr;
    if (o->p >= 1.0 || o->p < 0.0) o->p -= floor(o->p);
    return o->p;
}
static inline double al_sin01(double p)          { return sin(AL_2PI * p); }
static inline double al_saw01(double p)          { return 2.0 * p - 1.0; }
static inline double al_squ01(double p, double pw){ return p < pw ? 1.0 : -1.0; }
static inline double al_tri01(double p)          { return 4.0 * fabs(p - 0.5) - 1.0; }

/* polyBLEP anti-aliasing correction; dt = hz/sr */
static inline double al_polyblep(double t, double dt) {
    if (t < dt)            { t /= dt;         return t + t - t*t - 1.0; }
    else if (t > 1.0 - dt) { t = (t-1.0)/dt;  return t*t + t + t + 1.0; }
    return 0.0;
}
static inline double al_saw_bl(double p, double dt) { return (2.0*p - 1.0) - al_polyblep(p, dt); }
static inline double al_squ_bl(double p, double dt) {
    double s = p < 0.5 ? 1.0 : -1.0;
    s += al_polyblep(p, dt);
    s -= al_polyblep(al_wrap01(p + 0.5), dt);
    return s;
}
/* Casio-style phase distortion: warp phase toward a resonant peak (amt 0..1) */
static inline double al_pd(double p, double amt) {
    return al_wrap01(p + amt * 0.5 * sin(AL_2PI * p));
}

/* ================================================================= shape */

/* sine wavefolder; amt 0..1 (0 = clean, higher = more folds) */
static inline double al_fold(double x, double amt) {
    double g = 1.0 + amt * 5.0; return sin(0.5 * AL_PI * g * x);
}
/* soft overdrive with gain compensation; amt 0..1 */
static inline double al_drive(double x, double amt) {
    return al_softclip(x * (1.0 + amt * 6.0)) / (1.0 + amt * 1.5);
}
/* Chebyshev waveshaper: out = sum w[k] T_{k+1}(x). Feed a cosine of amp 1 and
 * w are the harmonic amplitudes. x is clamped to [-1,1]. */
static inline double al_cheby(double x, const double *w, int n) {
    double t0 = 1.0, t1, tk, y; int i;
    if (x > 1.0) x = 1.0; if (x < -1.0) x = -1.0;
    t1 = x; y = w[0] * t1;
    for (i = 1; i < n; i++) { tk = 2.0*x*t1 - t0; t0 = t1; t1 = tk; y += w[i]*tk; }
    return y;
}
static inline double al_bitcrush(double x, double bits) {
    double q = pow(2.0, bits - 1.0); if (q < 1.0) q = 1.0;
    return floor(x * q + 0.5) / q;
}

/* ================================================================= filter */

/* TPT state-variable filter (Cytomic). Call the _lp/_bp/_hp helper you want. */
typedef struct { double ic1, ic2; } al_svf;
static inline void al_svf_init(al_svf *f) { f->ic1 = f->ic2 = 0.0; }
static inline void al_svf_tick3(al_svf *f, double x, double cut, double res, double sr,
                                double *lp, double *bp, double *hp) {
    double g  = tan(AL_PI * al_clamp(cut, 10.0, sr*0.49) / sr);
    double k  = 2.0 - 2.0 * al_clamp(res, 0.0, 0.995);
    double a1 = 1.0 / (1.0 + g*(g + k)), a2 = g*a1, a3 = g*a2;
    double v3 = x - f->ic2;
    double v1 = a1*f->ic1 + a2*v3;
    double v2 = f->ic2 + a2*f->ic1 + a3*v3;
    f->ic1 = 2.0*v1 - f->ic1; f->ic2 = 2.0*v2 - f->ic2;
    if (bp) *bp = v1; if (lp) *lp = v2; if (hp) *hp = x - k*v1 - v2;
}
static inline double al_svf_lp(al_svf *f, double x, double cut, double res, double sr) {
    double lp; al_svf_tick3(f, x, cut, res, sr, &lp, 0, 0); return lp;
}
static inline double al_svf_bp(al_svf *f, double x, double cut, double res, double sr) {
    double bp; al_svf_tick3(f, x, cut, res, sr, 0, &bp, 0); return bp;
}
static inline double al_svf_hp(al_svf *f, double x, double cut, double res, double sr) {
    double hp; al_svf_tick3(f, x, cut, res, sr, 0, 0, &hp); return hp;
}

/* RBJ 0 dB-peak bandpass resonator (bounded however high the Q) */
typedef struct { double x1, x2, y1, y2; } al_bq;
static inline void   al_bq_init(al_bq *b) { b->x1=b->x2=b->y1=b->y2=0.0; }
static inline double al_reson(al_bq *b, double x, double freq, double q, double sr) {
    double w0 = AL_2PI * al_clamp(freq, 1.0, sr*0.49) / sr;
    double cw = cos(w0), sw = sin(w0);
    double alpha = sw / (2.0 * (q < 0.5 ? 0.5 : q));
    double a0 = 1.0 + alpha, b0 = alpha/a0, a1 = -2.0*cw/a0, a2 = (1.0-alpha)/a0;
    double y = b0*(x - b->x2) - a1*b->y1 - a2*b->y2;
    b->x2 = b->x1; b->x1 = x; b->y2 = b->y1; b->y1 = y;
    return y;
}

/* Zavalishin TPT 4-pole ladder lowpass with resonance (res 0..1, ~1 self-osc) */
typedef struct { double z[4]; } al_ladder;
static inline void   al_ladder_init(al_ladder *l) { l->z[0]=l->z[1]=l->z[2]=l->z[3]=0.0; }
static inline double al_ladder_tick(al_ladder *l, double x, double cut, double res, double sr) {
    double g = tan(AL_PI * al_clamp(cut, 10.0, sr*0.45) / sr);
    double G = g/(1.0+g), k = 4.0 * al_clamp(res, 0.0, 1.0);
    double S = (l->z[0]*(1-G)*(1-G)*(1-G) + l->z[1]*(1-G)*(1-G)
              + l->z[2]*(1-G) + l->z[3]);   /* approx feedback state */
    double u = (x - k*S) / (1.0 + k*G*G*G*G);
    int i; double v, in = u;
    for (i = 0; i < 4; i++) {
        v = (in - l->z[i]) * G; double o = v + l->z[i]; l->z[i] = o + v; in = o;
        if (!(l->z[i]==l->z[i]) || fabs(l->z[i])>1e3) l->z[i] = 0.0; /* guard */
    }
    return in;
}

/* ================================================================= env */

/* exponential attack/decay one-shot (like an 808 pluck) */
typedef struct { double v, dec; } al_ad;
static inline void   al_ad_init(al_ad *e) { e->v = 0.0; e->dec = 0.999; }
static inline void   al_ad_setdecay(al_ad *e, double ms, double sr) {
    e->dec = exp(-1.0 / (0.001 * (ms < 1.0 ? 1.0 : ms) * sr));
}
static inline void   al_ad_trig(al_ad *e) { e->v = 1.0; }
static inline double al_ad_tick(al_ad *e) { double v = e->v; e->v *= e->dec; return v; }

/* portamento / smoothing toward a target over `ms` */
typedef struct { double cur; } al_glide;
static inline void   al_glide_init(al_glide *g, double v) { g->cur = v; }
static inline double al_glide_tick(al_glide *g, double target, double ms, double sr) {
    double a = (ms < 0.1) ? 1.0 : 1.0 - exp(-1.0 / (0.001 * ms * sr));
    g->cur += a * (target - g->cur); return g->cur;
}

/* ================================================================= delay
 * Buffers are caller-owned power-of-two arrays; pass len-1 as `mask`. This
 * keeps the header allocation-free (each external owns its own storage). */
static inline void   al_del_write(double *buf, int mask, int *wr, double x) {
    buf[*wr] = x; *wr = (*wr + 1) & mask;
}
static inline double al_del_read(const double *buf, int mask, int wr, double delay) {
    double rp = (double)wr - delay;
    while (rp < 0.0) rp += (mask + 1);
    int i = (int)rp; double f = rp - i;
    return al_lerp(buf[i & mask], buf[(i+1) & mask], f);
}
/* one first-order allpass (dispersion); coef a in (-1,1) */
typedef struct { double x1, y1; } al_ap;
static inline void   al_ap_init(al_ap *p) { p->x1 = p->y1 = 0.0; }
static inline double al_ap_tick(al_ap *p, double x, double a) {
    double y = -a*x + p->x1 + a*p->y1; p->x1 = x; p->y1 = y; return y;
}

/* ================================================================= noise */
static inline double al_white(al_rng *r) { return al_randb(r); }

/* Paul Kellet pink filter */
typedef struct { double b0, b1, b2, b3, b4, b5, b6; } al_pink;
static inline void   al_pink_init(al_pink *p) { memset(p, 0, sizeof(*p)); }
static inline double al_pink_tick(al_pink *p, al_rng *r) {
    double w = al_randb(r);
    p->b0 = 0.99886*p->b0 + w*0.0555179; p->b1 = 0.99332*p->b1 + w*0.0750759;
    p->b2 = 0.96900*p->b2 + w*0.1538520; p->b3 = 0.86650*p->b3 + w*0.3104856;
    p->b4 = 0.55000*p->b4 + w*0.5329522; p->b5 = -0.7616*p->b5 - w*0.0168980;
    double o = p->b0+p->b1+p->b2+p->b3+p->b4+p->b5+p->b6 + w*0.5362;
    p->b6 = w*0.115926; return o * 0.11;
}
/* velvet / dust: sparse random impulses at ~density Hz */
static inline double al_dust(al_rng *r, double density, double sr) {
    double thr = density / sr; return (al_rand(r) < thr) ? al_randb(r) : 0.0;
}
/* sample & hold: returns a new random value every `hz`, held between */
typedef struct { double p, v; } al_sh;
static inline void   al_sh_init(al_sh *s) { s->p = 1.0; s->v = 0.0; }
static inline double al_sh_tick(al_sh *s, al_rng *r, double hz, double sr) {
    s->p += hz / sr; if (s->p >= 1.0) { s->p -= floor(s->p); s->v = al_randb(r); } return s->v;
}

/* ================================================================= chaos */
/* discrete maps: pass current state, get next (kept in range) */
static inline double al_map_logistic(double x, double r) { x = r*x*(1.0-x); return al_clamp(x,0,1); }
static inline double al_map_tent(double x, double mu)    { x = mu*(x<0.5?x:1.0-x); return al_clamp(x,0,1); }
static inline double al_map_sine(double x, double a)     { x = a*sin(AL_PI*x); return al_clamp(x,0,1); }

/* continuous flows (explicit Euler steps); state in al_v3. Keep dt small
 * (Euler diverges for large dt). Each step clamps state and guards against
 * non-finite values so a composed voice can never rail on a blow-up. */
typedef struct { double x, y, z; } al_v3;
static inline void al_v3_safe(al_v3 *s, double lim) {
    if (!(s->x==s->x) || fabs(s->x)>lim) s->x = 0.0;
    if (!(s->y==s->y) || fabs(s->y)>lim) s->y = 0.0;
    if (!(s->z==s->z) || fabs(s->z)>lim) s->z = 0.0;
}
static inline void al_lorenz(al_v3 *s, double dt) {
    double dx=10.0*(s->y-s->x), dy=s->x*(28.0-s->z)-s->y, dz=s->x*s->y-(8.0/3.0)*s->z;
    s->x+=dt*dx; s->y+=dt*dy; s->z+=dt*dz; al_v3_safe(s, 100.0);
}
static inline void al_rossler(al_v3 *s, double dt, double c) {
    double dx=-s->y-s->z, dy=s->x+0.2*s->y, dz=0.2+s->z*(s->x-c);
    s->x+=dt*dx; s->y+=dt*dy; s->z+=dt*dz; al_v3_safe(s, 100.0);
}
static inline void al_chua(al_v3 *s, double dt, double alpha) {
    double m0=-1.143, m1=-0.714, beta=28.0;
    double h=m1*s->x+0.5*(m0-m1)*(fabs(s->x+1.0)-fabs(s->x-1.0));
    double dx=alpha*(s->y-s->x-h), dy=s->x-s->y+s->z, dz=-beta*s->y;
    s->x+=dt*dx; s->y+=dt*dy; s->z+=dt*dz; al_v3_safe(s, 60.0);
}
/* Duffing / Van der Pol as 2-state; store in al_v3 (x=pos,y=vel) */
static inline void al_duffing(al_v3 *s, double dt, double delta, double gamma, double drive) {
    double dx=s->y, dy=-delta*s->y + s->x - s->x*s->x*s->x + gamma*drive;
    s->x+=dt*dx; s->y+=dt*dy; al_v3_safe(s, 20.0);
}
static inline void al_vdp(al_v3 *s, double dt, double mu) {
    double dx=s->y, dy=mu*(1.0-s->x*s->x)*s->y - s->x;
    s->x+=dt*dx; s->y+=dt*dy; al_v3_safe(s, 20.0);
}

/* ================================================================= modal
 * Inharmonic mode-ratio tables abstracted from the modal/bowed engines.
 * al_material(m, out8) fills out[0..7] with a material's mode ratios. */
static inline void al_material(int m, double *out) {
    static const double T[7][8] = {
        {1,2.756,5.404,8.933,13.34,18.64,24.82,31.87}, /* 0 bar/aluphone */
        {1,2.32,4.25,6.63,9.38,12.44,15.80,19.43},     /* 1 bowl */
        {1,2.40,4.72,7.70,11.30,15.44,20.10,25.25},    /* 2 glass */
        {1,2.76,5.40,8.93,13.34,18.64,24.82,31.87},    /* 3 tube */
        {1,2.01,3.00,4.02,5.01,6.03,7.00,8.04},        /* 4 string */
        {1,1.71,3.14,4.28,6.83,8.11,10.7,13.9},        /* 5 alien */
        {1,2.0,3.0,4.15,5.43,6.79,8.21,9.33}           /* 6 bell */
    };
    int i, k = ((m % 7) + 7) % 7;
    for (i = 0; i < 8; i++) out[i] = T[k][i];
}

/* ================================================================= spectral
 * Iterative radix-2 FFT (dir=-1 forward, +1 inverse, unnormalised). */
static inline void al_fft(double *re, double *im, int n, int dir) {
    int i, j, k, m, step; double c, s, tr, ti, ang, wr, wi, wt;
    for (i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { tr=re[i];re[i]=re[j];re[j]=tr; ti=im[i];im[i]=im[j];im[j]=ti; }
    }
    for (step = 1; step < n; step <<= 1) {
        m = step << 1; ang = dir * AL_PI / step; wr = cos(ang); wi = sin(ang);
        for (k = 0; k < n; k += m) {
            c = 1.0; s = 0.0;
            for (j = 0; j < step; j++) {
                int a = k + j, b = k + j + step;
                tr = c*re[b] - s*im[b]; ti = c*im[b] + s*re[b];
                re[b] = re[a]-tr; im[b] = im[a]-ti; re[a]+=tr; im[a]+=ti;
                wt = c*wr - s*wi; s = c*wi + s*wr; c = wt;
            }
        }
    }
}
static inline void al_hann(double *w, int n) {
    int i; for (i = 0; i < n; i++) w[i] = 0.5 - 0.5*cos(AL_2PI*i/(n-1));
}

/* IIR Hilbert transformer (polyphase allpass pair) for frequency shifting.
 * Feed a sample; get the in-phase (I) and quadrature (Q) outputs 90 deg apart.
 * shifted_up = I*cos - Q*sin ; shifted_down = I*cos + Q*sin. */
typedef struct {
    double ax1[4], ax2[4], ay1[4], ay2[4];
    double bx1[4], bx2[4], by1[4], by2[4];
    double bdelay;
} al_hilbert;
static inline void al_hilbert_init(al_hilbert *h) { memset(h, 0, sizeof(*h)); }
static inline void al_hilbert_tick(al_hilbert *h, double in, double *I, double *Q) {
    static const double A[4]={0.6923877778065970,0.9360654322959236,0.9882295226860060,0.9987488452737050};
    static const double B[4]={0.4021921162426028,0.8561710882420250,0.9722909545651573,0.9952884791278394};
    int i; double v = in;
    for (i = 0; i < 4; i++) {
        double y = A[i]*v + h->ax2[i] - A[i]*h->ay2[i];
        h->ax2[i]=h->ax1[i]; h->ax1[i]=v; h->ay2[i]=h->ay1[i]; h->ay1[i]=y; v = y;
    }
    *I = v;
    v = h->bdelay; h->bdelay = in;
    for (i = 0; i < 4; i++) {
        double y = B[i]*v + h->bx2[i] - B[i]*h->by2[i];
        h->bx2[i]=h->bx1[i]; h->bx1[i]=v; h->by2[i]=h->by1[i]; h->by1[i]=y; v = y;
    }
    *Q = v;
}

/* ================================================================= fdn
 * Lossless Householder feedback mix for feedback delay networks. Reflects the
 * vector f[0..n-1] in place: f -> f - (2/n) * sum(f). Multiply by a loop gain
 * and saturate outside for a bounded, self-oscillating network. */
static inline void al_householder(double *f, int n) {
    double sum = 0.0; int i;
    for (i = 0; i < n; i++) sum += f[i];
    sum *= 2.0 / (double)n;
    for (i = 0; i < n; i++) f[i] -= sum;
}

/* ================================================================= terrain
 * wave-terrain surfaces z = f(x,y), x,y in ~[-r,r]; returns ~[-1,1] */
static inline double al_terrain(int t, double x, double y) {
    switch (((t % 6) + 6) % 6) {
        case 1:  return 0.7*(sin(3.3*x + 2.1*y) + 0.5*sin(5.0*x*y));
        case 2:  return x*x - y*y;
        case 3:  return 1.5*x*y*(x + y);
        case 4:  return sin(AL_PI*(x + y));
        case 5:  return sin(5.0*x*y);
        default: return 0.5*(x-y)*(x-1.0)*(x+1.0)*(y-1.0)*(y+1.0);
    }
}

/* =============================================================== macro-voices
 * Higher-level bundles so a specimen is a few blocks, not a hundred lines. */

/* detuned band-limited saw cluster (supersaw). detune ~0..0.05 */
#define AL_CLUSTER_MAX 7
typedef struct { al_phasor ph[AL_CLUSTER_MAX]; int n; } al_cluster;
static inline void al_cluster_init(al_cluster *c, int n) {
    int i; c->n = n < 1 ? 1 : (n > AL_CLUSTER_MAX ? AL_CLUSTER_MAX : n);
    for (i = 0; i < AL_CLUSTER_MAX; i++) { al_phasor_init(&c->ph[i]); c->ph[i].p = i*0.11; }
}
static inline double al_cluster_tick(al_cluster *c, double f0, double detune, double sr) {
    double s = 0.0; int i, n = c->n;
    for (i = 0; i < n; i++) {
        double d = 1.0 + detune * ((n>1)? ((double)i/(n-1) - 0.5)*2.0 : 0.0);
        double fd = f0*d, p = al_phasor_tick(&c->ph[i], fd, sr);
        s += al_saw_bl(p, fd/sr);
    }
    return s / n;
}

/* FM operator with 2-sample-averaged self-feedback (stable) */
typedef struct { al_phasor ph; double y1, y2; } al_fmop;
static inline void al_fmop_init(al_fmop *o) { al_phasor_init(&o->ph); o->y1 = o->y2 = 0.0; }
static inline double al_fmop_tick(al_fmop *o, double freq, double pmod, double fb, double sr) {
    double p = al_phasor_tick(&o->ph, freq, sr);
    double y = sin(AL_2PI*p + pmod + fb*0.5*(o->y1 + o->y2));
    o->y2 = o->y1; o->y1 = y; return y;
}

/* modal resonator bank (excited by `exc`, ringing at ratio[i]*f0) */
#define AL_MODBANK_MAX 12
typedef struct { al_bq r[AL_MODBANK_MAX]; double ratio[AL_MODBANK_MAX]; int n; } al_modbank;
static inline void al_modbank_init(al_modbank *m, int n, const double *ratios) {
    int i; m->n = n < 1 ? 1 : (n > AL_MODBANK_MAX ? AL_MODBANK_MAX : n);
    for (i = 0; i < m->n; i++) { al_bq_init(&m->r[i]); m->ratio[i] = ratios ? ratios[i] : (i+1); }
}
static inline double al_modbank_tick(al_modbank *m, double exc, double f0, double q, double sr) {
    double s = 0.0; int i;
    for (i = 0; i < m->n; i++) s += al_reson(&m->r[i], exc, m->ratio[i]*f0, q, sr);
    return s / m->n;
}

/* leaky integrate-and-fire neuron; returns a fire impulse (0 or ~1) */
typedef struct { double v; } al_neuron;
static inline void   al_neuron_init(al_neuron *n) { n->v = 0.0; }
static inline double al_neuron_tick(al_neuron *n, double drive, double leak, double inj) {
    n->v += drive - leak*n->v + inj;
    if (n->v >= 1.0) { n->v = 0.0; return 1.0; }
    return 0.0;
}

/* 2-operator FM voice (modulator with feedback -> carrier) */
typedef struct { al_fmop mop, cop; } al_fm2;
static inline void al_fm2_init(al_fm2 *v) { al_fmop_init(&v->mop); al_fmop_init(&v->cop); }
static inline double al_fm2_tick(al_fm2 *v, double f0, double ratio, double index, double fb, double sr) {
    double m = al_fmop_tick(&v->mop, f0*ratio, 0.0, fb, sr);
    return al_fmop_tick(&v->cop, f0, index*m, 0.0, sr);
}

/* resonant feedback comb on a caller-owned power-of-two buffer (mask = len-1) */
static inline double al_comb(double *buf, int mask, int *wr, double x, double delay, double fb) {
    double y = al_del_read(buf, mask, *wr, delay);
    al_del_write(buf, mask, wr, x + fb * y);
    return y;
}
/* Schroeder allpass section (a diffusion unit); chain several for reverb */
static inline double al_apdelay(double *buf, int mask, int *wr, double x, double delay, double g) {
    double bo = al_del_read(buf, mask, *wr, delay);
    double in = x - g * bo;
    al_del_write(buf, mask, wr, in);
    return bo + g * in;
}

/* envelope follower (atk/rel are one-pole coefs 0..1) */
typedef struct { double e; } al_env;
static inline void   al_env_init(al_env *e) { e->e = 0.0; }
static inline double al_env_tick(al_env *e, double x, double atk, double rel) {
    double a = fabs(x), c = (a > e->e) ? atk : rel;
    e->e += c * (a - e->e); return e->e;
}

/* pick a value from a ratio set by a 0..1 index (for pitch/quantise moves) */
static inline double al_quant_pick(double u, const double *r, int n) {
    int i = (int)(u * n); if (i < 0) i = 0; if (i >= n) i = n - 1; return r[i];
}

/* self-scheduling grain cloud: synthesised windowed sine grains, no buffer.
 * density = grains/sec, grainms = grain length, spread = pitch scatter. */
#define AL_CLOUD_MAX 12
typedef struct {
    double ph[AL_CLOUD_MAX], wp[AL_CLOUD_MAX], fr[AL_CLOUD_MAX];
    int    act[AL_CLOUD_MAX];
    double clk;
} al_cloud;
static inline void al_cloud_init(al_cloud *c) { memset(c, 0, sizeof(*c)); }
static inline double al_cloud_tick(al_cloud *c, al_rng *rng, double density,
                                   double grainms, double basef, double spread, double sr) {
    int i; double out = 0.0;
    double winc = 1000.0 / ((grainms < 1.0 ? 1.0 : grainms) * sr);
    c->clk += density / sr;
    if (c->clk >= 1.0) {
        c->clk -= floor(c->clk);
        for (i = 0; i < AL_CLOUD_MAX; i++) if (!c->act[i]) {     /* start a grain */
            c->act[i] = 1; c->wp[i] = 0.0; c->ph[i] = 0.0;
            c->fr[i] = basef * (1.0 + spread * al_randb(rng));
            if (c->fr[i] < 1.0) c->fr[i] = 1.0;
            break;
        }
    }
    for (i = 0; i < AL_CLOUD_MAX; i++) if (c->act[i]) {
        double w = 0.5 - 0.5 * cos(AL_2PI * c->wp[i]);
        out += w * sin(AL_2PI * c->ph[i]);
        c->ph[i] += c->fr[i] / sr; if (c->ph[i] >= 1.0) c->ph[i] -= 1.0;
        c->wp[i] += winc; if (c->wp[i] >= 1.0) c->act[i] = 0;
    }
    return out * 0.5;
}

/* =============================================================== power blocks
 * Higher-level engines for DEEP, evolving, self-differentiating specimens. */

/* --- al_wander: a novelty/complexification modulation brain ---
 * N controls that each drift toward a target, and when they arrive pick a new
 * target -- usually a small step (complexify locally), occasionally a big jump
 * (novelty). Read them as 0..1 (al_wander_val) or -1..1 (al_wander_bip) and
 * route ONE brain to a dozen parameters so a voice never repeats itself. */
#define AL_WANDER_MAX 24
typedef struct { double v[AL_WANDER_MAX], tg[AL_WANDER_MAX]; int n; } al_wander;
static inline void al_wander_init(al_wander *w, al_rng *r, int n) {
    int i; w->n = n < 1 ? 1 : (n > AL_WANDER_MAX ? AL_WANDER_MAX : n);
    for (i = 0; i < AL_WANDER_MAX; i++) { w->v[i] = al_rand(r); w->tg[i] = al_rand(r); }
}
static inline void al_wander_tick(al_wander *w, al_rng *r, double rate, double sr) {
    int i; double a = rate / sr; if (a > 0.4) a = 0.4; if (a < 0.0) a = 0.0;
    for (i = 0; i < w->n; i++) {
        w->v[i] += a * (w->tg[i] - w->v[i]);
        if (fabs(w->tg[i] - w->v[i]) < 0.004) {
            double nt = (al_rand(r) < 0.15) ? al_rand(r)         /* novelty jump */
                                            : w->v[i] + 0.35*al_randb(r); /* local */
            w->tg[i] = al_clamp(nt, 0.0, 1.0);
        }
    }
}
static inline double al_wander_val(al_wander *w, int i) { return w->v[i % w->n]; }
static inline double al_wander_bip(al_wander *w, int i) { return w->v[i % w->n]*2.0 - 1.0; }

/* --- al_formants: 5-band vowel filter with continuous a-e-i-o-u morph --- */
typedef struct { al_svf f[5]; } al_formants;
static inline void al_formants_init(al_formants *ff) { int i; for (i=0;i<5;i++) al_svf_init(&ff->f[i]); }
static inline double al_formants_tick(al_formants *ff, double x, double vowel, double q, double sr) {
    static const double VF[5][5] = {  /* a e i o u : five formant centres (Hz) */
        {800,1150,2900,3900,4950},{400,1600,2700,3300,4950},{350,1700,2700,3700,4950},
        {450,800,2830,3500,4950},{325,700,2530,3500,4950}};
    static const double VG[5] = {1.0,0.6,0.45,0.35,0.25};
    double p = al_clamp(vowel,0,1)*4.0; int vi = (int)p; double fr = p - vi;
    int j = vi+1; if (vi>4){vi=4;fr=0;} if (j>4) j=4;
    double res = al_clamp(0.6 + 0.35*q, 0, 0.97), s = 0.0; int i;
    for (i = 0; i < 5; i++) {
        double cf = VF[vi][i] + fr*(VF[j][i]-VF[vi][i]);
        s += VG[i] * al_svf_bp(&ff->f[i], x, cf, res, sr);
    }
    return s;
}

/* --- al_chorus: one modulated delay voice for width --- */
#define AL_CHORUS_LEN 2048
typedef struct { double buf[AL_CHORUS_LEN]; int wr; double ph; } al_chorus;
static inline void al_chorus_init(al_chorus *c) { memset(c, 0, sizeof(*c)); }
static inline double al_chorus_tick(al_chorus *c, double x, double depthms, double rate, double sr) {
    double d = 0.010*sr + depthms*0.001*sr*(0.5 - 0.5*cos(AL_2PI*c->ph));
    double y;
    c->ph += rate/sr; if (c->ph >= 1.0) c->ph -= 1.0;
    if (d < 1) d = 1; if (d > AL_CHORUS_LEN-2) d = AL_CHORUS_LEN-2;
    y = al_del_read(c->buf, AL_CHORUS_LEN-1, c->wr, d);
    al_del_write(c->buf, AL_CHORUS_LEN-1, &c->wr, x);
    return 0.5*(x + y);
}

/* --- al_fdnrev: self-contained 4-line FDN reverb (owns its buffers) --- */
#define AL_FDN_LEN 4096
typedef struct { double b[4][AL_FDN_LEN]; int wr[4]; double lp[4]; int len[4]; } al_fdnrev;
static inline void al_fdnrev_init(al_fdnrev *f) {
    memset(f, 0, sizeof(*f));
    f->len[0]=1237; f->len[1]=1687; f->len[2]=1993; f->len[3]=2803;
}
static inline double al_fdnrev_tick(al_fdnrev *f, double x, double size, double damp, double fb) {
    double o[4], total, sub; int i;
    double sz = 0.25 + 0.75*al_clamp(size,0,1);
    double dmp = 0.05 + 0.9*al_clamp(damp,0,1);
    double g = al_clamp(fb, 0, 0.97);
    for (i = 0; i < 4; i++) {
        int L = (int)(f->len[i]*sz); if (L<4) L=4; if (L>AL_FDN_LEN-2) L=AL_FDN_LEN-2;
        o[i] = al_del_read(f->b[i], AL_FDN_LEN-1, f->wr[i], (double)L);
        f->lp[i] += dmp*(o[i]-f->lp[i]); o[i] = f->lp[i];
    }
    total = o[0]+o[1]+o[2]+o[3]; sub = 0.5*total;   /* Householder reflect */
    for (i = 0; i < 4; i++) {
        double in = x + g*(o[i]-sub);
        al_del_write(f->b[i], AL_FDN_LEN-1, &f->wr[i], in);
    }
    return 0.5*total;
}

/* ring modulation with dry/wet mix (mix 0=dry .. 1=full ring). A strong
 * character stage: multiplies the signal by a modulator, spawning sum/diff
 * sidebands (metallic, bell-like, or clangorous depending on the ratio). */
static inline double al_ringmod(double dry, double mod, double mix) {
    return al_lerp(dry, dry * mod, mix);
}

/* Flush-to-zero: enable it once per perform block. Resonators/filters whose
 * state decays toward zero between excitations generate DENORMAL numbers, and
 * hosts (e.g. Pd's audio thread) that don't flush them can run 10-100x slower
 * -- slower than realtime -- which looks like a hang. This forces the FPU to
 * flush denormals to zero (both arm64 and x86). Call al_denorm_off() at the
 * top of a perform routine. */
#if defined(__aarch64__)
static inline void al_denorm_off(void) {
    uint64_t fpcr; __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
    fpcr |= (1ULL << 24);   /* FZ: flush-to-zero */
    __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr));
}
#elif defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#include <pmmintrin.h>
static inline void al_denorm_off(void) {
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
}
#else
static inline void al_denorm_off(void) {}
#endif

/* =============================================================== folder ports
 * Bounded engines ported from the ./synths collection for the breeder grammar.
 * Every one is amplitude-bounded and state-clamped so a composed voice is safe. */

/* ---- DSF: finite discrete summation formula (band-limited harmonic stack) --
 * phase 0..1, beta = harmonic spacing ratio, a = rolloff 0..0.95, N harmonics. */
static inline double al_dsf(double phase, double beta, double a, int N) {
    double th = AL_2PI*phase, bt = AL_2PI*beta, ak = 1.0, sum = 0.0, norm = 0.0;
    int k; if (N < 1) N = 1; if (N > 48) N = 48;
    if (a > 0.95) a = 0.95; if (a < 0.0) a = 0.0;
    for (k = 0; k < N; k++) { sum += ak*sin(th + k*bt); norm += ak; ak *= a; }
    return (norm > 1e-9) ? sum/norm : 0.0;
}

/* ---- GENDY: Xenakis dynamic stochastic polyline with mirror barriers ------- */
#define AL_GENDY_MAX 24
typedef struct { double amp[AL_GENDY_MAX], dur[AL_GENDY_MAX]; int n, idx; double frac; } al_gendy;
static inline void al_gendy_init(al_gendy *g, al_rng *r, int n) {
    int i; if (n < 3) n = 3; if (n > AL_GENDY_MAX) n = AL_GENDY_MAX;
    g->n = n; g->idx = 0; g->frac = 0.0;
    for (i = 0; i < n; i++) { g->amp[i] = al_randb(r); g->dur[i] = 1.0; }
}
static inline double al_gendy_tick(al_gendy *g, al_rng *r, double f0, double sr,
                                   double ampstep, double durstep) {
    int nx; double d = g->dur[g->idx]; if (d < 0.3) d = 0.3;
    g->frac += (f0 * g->n / sr) / d;
    while (g->frac >= 1.0) {
        g->frac -= 1.0; g->idx++; if (g->idx >= g->n) g->idx = 0;
        { double a = g->amp[g->idx] + ampstep*al_gauss(r);
          if (a >  1.0) a =  2.0 - a; if (a < -1.0) a = -2.0 - a;
          g->amp[g->idx] = al_clamp(a, -1.0, 1.0);
          { double du = g->dur[g->idx] + durstep*al_gauss(r);
            if (du > 1.7) du = 3.4 - du; if (du < 0.3) du = 0.6 - du;
            g->dur[g->idx] = al_clamp(du, 0.3, 1.7); } }
    }
    nx = g->idx + 1; if (nx >= g->n) nx = 0;
    return al_lerp(g->amp[g->idx], g->amp[nx], g->frac);
}

/* ---- scanned synthesis: ring of coupled masses, scanned at audio rate ------ */
#define AL_SCAN_N 24
typedef struct { double pos[AL_SCAN_N], vel[AL_SCAN_N]; double ph; } al_scan;
static inline void al_scan_init(al_scan *s, al_rng *r) {
    int i; for (i = 0; i < AL_SCAN_N; i++) { s->pos[i] = 0.3*al_randb(r); s->vel[i] = 0.0; }
    s->ph = 0.0;
}
static inline double al_scan_tick(al_scan *s, al_rng *r, double f0, double sr,
                                  double tension, double damp, double drive) {
    int i, N = AL_SCAN_N; double k = 0.02 + tension*0.35; double dmp = 0.9995 - damp*0.02;
    for (i = 0; i < N; i++) {
        int l = (i+N-1)%N, rr = (i+1)%N;
        double f = k*(s->pos[l] + s->pos[rr] - 2.0*s->pos[i]) - 0.002*s->pos[i];
        s->vel[i] = al_clamp(s->vel[i]*dmp + f, -1.0, 1.0);
    }
    for (i = 0; i < N; i++) { s->pos[i] += s->vel[i]; s->pos[i] = al_clamp(s->pos[i], -4.0, 4.0); }
    s->pos[0] += drive*0.02*al_randb(r);
    s->ph += f0/sr; if (s->ph >= 1.0) s->ph -= 1.0;
    { double fp = s->ph*N; int i0 = (int)fp; double fr = fp - i0; int i1 = (i0+1)%N; i0 %= N;
      return al_clamp(al_lerp(s->pos[i0], s->pos[i1], fr)*0.5, -1.0, 1.0); }
}

/* ---- 2D membrane: wave equation on a fixed grid (strike + pickup) ---------- */
#define AL_MEM_W 12
typedef struct { double u[AL_MEM_W*AL_MEM_W], v[AL_MEM_W*AL_MEM_W]; } al_membrane;
static inline void al_membrane_init(al_membrane *m) { memset(m, 0, sizeof(*m)); }
static inline double al_membrane_tick(al_membrane *m, double c, double damp, double strike) {
    int W = AL_MEM_W, x, y; double dmp = 1.0 - damp*0.02; double cc = al_clamp(c, 0.0, 0.49);
    if (strike > 0.0) m->u[(W/2)*W + W/2] += strike;
    for (y = 1; y < W-1; y++) for (x = 1; x < W-1; x++) {
        int i = y*W + x;
        double lap = m->u[i-1] + m->u[i+1] + m->u[i-W] + m->u[i+W] - 4.0*m->u[i];
        m->v[i] = (m->v[i] + cc*lap)*dmp;
    }
    for (y = 1; y < W-1; y++) for (x = 1; x < W-1; x++) {
        int i = y*W + x; m->u[i] += m->v[i]; m->u[i] = al_clamp(m->u[i], -8.0, 8.0);
    }
    return al_clamp(m->u[(W/3)*W + W/2]*0.5, -1.0, 1.0);
}

/* ---- Kuramoto: mean-field coupled oscillators (sync <-> desync) ------------ */
#define AL_KURA_N 12
typedef struct { double ph[AL_KURA_N], om[AL_KURA_N]; int n; } al_kura;
static inline void al_kura_init(al_kura *k, al_rng *r, int n) {
    int i; if (n < 2) n = 2; if (n > AL_KURA_N) n = AL_KURA_N; k->n = n;
    for (i = 0; i < n; i++) { k->ph[i] = al_rand(r); k->om[i] = 0.5 + 1.5*al_rand(r); }
}
static inline double al_kura_tick(al_kura *k, double f0, double sr, double spread, double K) {
    int i, n = k->n; double msin = 0.0, mcos = 0.0, out = 0.0;
    for (i = 0; i < n; i++) { msin += sin(AL_2PI*k->ph[i]); mcos += cos(AL_2PI*k->ph[i]); }
    msin /= n; mcos /= n;
    for (i = 0; i < n; i++) {
        double ci = K*(msin*cos(AL_2PI*k->ph[i]) - mcos*sin(AL_2PI*k->ph[i]));
        double f = f0*(1.0 + spread*(k->om[i] - 1.25));
        k->ph[i] += f/sr + ci/sr; k->ph[i] -= floor(k->ph[i]);
        out += sin(AL_2PI*k->ph[i]);
    }
    return al_clamp(out/n, -1.0, 1.0);
}

/* ---- Kaneko coupled-map lattice (logistic), read as a wavetable ------------ */
#define AL_CML_N 16
typedef struct { double x[AL_CML_N]; double ph, clk; } al_cml;
static inline void al_cml_init(al_cml *c, al_rng *r) {
    int i; for (i = 0; i < AL_CML_N; i++) c->x[i] = 0.2 + 0.6*al_rand(r);
    c->ph = 0.0; c->clk = 0.0;
}
static inline double al_cml_tick(al_cml *c, double f0, double sr, double rr, double eps) {
    int i, N = AL_CML_N;
    c->clk += f0*N/sr;
    if (c->clk >= 1.0) { c->clk -= floor(c->clk);
        double f[AL_CML_N];
        for (i = 0; i < N; i++) f[i] = al_map_logistic(c->x[i], rr);
        for (i = 0; i < N; i++) { int l = (i+N-1)%N, rg = (i+1)%N;
            c->x[i] = al_clamp((1.0-eps)*f[i] + 0.5*eps*(f[l]+f[rg]), 0.0, 1.0); }
    }
    c->ph += f0/sr; if (c->ph >= 1.0) c->ph -= 1.0;
    { double fp = c->ph*N; int i0 = (int)fp; double fr = fp - i0; int i1 = (i0+1)%N; i0 %= N;
      return al_lerp(c->x[i0], c->x[i1], fr)*2.0 - 1.0; }
}

/* ---- elementary cellular automaton as a living wavetable ------------------- */
#define AL_CA_N 32
typedef struct { unsigned char c[AL_CA_N]; double ph, clk; } al_ca;
static inline void al_ca_init(al_ca *a, al_rng *r) {
    int i; for (i = 0; i < AL_CA_N; i++) a->c[i] = (unsigned char)(al_rng_u(r) & 1);
    a->ph = 0.0; a->clk = 0.0; a->c[AL_CA_N/2] = 1;
}
static inline double al_ca_tick(al_ca *a, double f0, double sr, int rule, double clkmul) {
    int i, N = AL_CA_N;
    a->clk += f0*clkmul/sr;
    if (a->clk >= 1.0) { a->clk -= floor(a->clk);
        unsigned char nx[AL_CA_N];
        for (i = 0; i < N; i++) { int l = a->c[(i+N-1)%N], cc = a->c[i], rg = a->c[(i+1)%N];
            int idx = (l<<2)|(cc<<1)|rg; nx[i] = (unsigned char)((rule>>idx)&1); }
        for (i = 0; i < N; i++) a->c[i] = nx[i];
    }
    a->ph += f0/sr; if (a->ph >= 1.0) a->ph -= 1.0;
    { int bit = (int)(a->ph*N) % N; return a->c[bit] ? 0.7 : -0.7; }
}

#endif /* ALIEN_H */
