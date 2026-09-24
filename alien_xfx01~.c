/* alien_xfx01~ -- auto-bred xeno_fx effect
 * chain=chorus+freqshift+downsample+fold+tremolo
 * inlet0 signal input | inlet1 param 0..100 (wraps) | outlet0 signal
 */
#include "m_pd.h"
#include "alien.h"
#include <string.h>

static t_class *alien_xfx01_class;

typedef struct _alien_xfx01 {
    t_object x_obj;
    t_float x_f;
    double x_sr;
    al_rng rng;
    al_wander wnd;
    al_dc dc;
    al_env envf;
    al_phasor lfo_ph;
    al_sh sh;
    double fbprev;
    al_chorus st0;
    al_hilbert st1_h; al_phasor st1_p;
    double st2_hold; int st2_count;
    al_phasor st4;
} t_alien_xfx01;

static inline double alien_xfx01_tick(t_alien_xfx01 *x, double in, double pr, double sr)
{
    double pn = al_param100(pr);
    double dry0 = in;
    double s = in;
    double env = al_env_tick(&x->envf, s, 0.03096, 0.01912);
    double lfo = 0.5 + 0.5*sin(AL_2PI*al_phasor_tick(&x->lfo_ph, 2.72469, sr));
    double shv = 0.5 + 0.5*al_sh_tick(&x->sh, &x->rng, 17.43553, sr);
    double fb = x->fbprev;
    al_wander_tick(&x->wnd, &x->rng, 3.2204, sr);
        double w0=al_wander_val(&x->wnd,0), w1=al_wander_val(&x->wnd,1), w2=al_wander_val(&x->wnd,2), w3=al_wander_val(&x->wnd,3), w4=al_wander_val(&x->wnd,4);
        { double dry=s; double y=al_chorus_tick(&x->st0,s,al_clamp((12.1868)+(pn)*(9.49862),0.2,18),al_clamp((5.52222)+(env)*(1.88179),0.03,6),sr); s=al_lerp(dry,y,al_clamp((0.68979)+(fb)*(0.29251),0.1,0.9)); }
        { double dry=s,I,Q,ph,c,q,y; al_hilbert_tick(&x->st1_h,s,&I,&Q); ph=al_phasor_tick(&x->st1_p,fabs(al_clamp((-656.87)+(pn)*(-634.257),-900,900)),sr); c=cos(AL_2PI*ph); q=sin(AL_2PI*ph); y=(al_clamp((-656.87)+(pn)*(-634.257),-900,900)>=0? I*c-Q*q : I*c+Q*q); s=al_lerp(dry,y,al_clamp((0.77172)+(w4)*(0.08459),0.08,0.95)); }
        { double dry=s; int h=(int)(al_clamp((24.8712)+(pn)*(51.1174),1,90)); if(h<1)h=1; if(--x->st2_count<=0){x->st2_count=h; x->st2_hold=s;} s=al_lerp(dry,x->st2_hold,al_clamp((1)+(0.0)*(0),0.1,1)); }
        s = al_fold(s, al_clamp((1)+(w1)*(-0.36739),0.03,1));
        { double l=0.5+0.5*sin(AL_2PI*al_phasor_tick(&x->st4,al_clamp((5.03201)+(pn)*(-4.49117),0.05,18),sr)); s *= 1.0 - (al_clamp((1)+(pn)*(-0.59713),0.05,1))*(1.0-l); }
    s = al_lerp(dry0, s, al_clamp((1)+(w0)*(-0.3175),0.25,1));
    s = al_dc_tick(&x->dc, s);
    if (!(s==s)) s = 0.0;
    s = al_softclip(s);
    x->fbprev = fabs(s);
    return s * 0.5459;
}

static t_int *alien_xfx01_perform(t_int *w)
{
    t_alien_xfx01 *x = (t_alien_xfx01 *)(w[1]);
    t_sample *in = (t_sample *)(w[2]);
    t_sample *pin = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    int n = (int)(w[5]);
    double sr = x->x_sr;
    al_denorm_off();
    while (n--) *out++ = (t_sample)alien_xfx01_tick(x, *in++, *pin++, sr);
    return (w + 6);
}

static void alien_xfx01_dsp(t_alien_xfx01 *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    dsp_add(alien_xfx01_perform, 5, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

static void *alien_xfx01_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_xfx01 *x = (t_alien_xfx01 *)pd_new(alien_xfx01_class);
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    al_rng_seed(&x->rng, 3175416371u);
    al_wander_init(&x->wnd, &x->rng, 5);
    al_dc_init(&x->dc); al_env_init(&x->envf); al_phasor_init(&x->lfo_ph); al_sh_init(&x->sh);
    x->fbprev = 0.0;
    al_chorus_init(&x->st0);
    al_hilbert_init(&x->st1_h); al_phasor_init(&x->st1_p);
    x->st2_hold=0.0; x->st2_count=0;
    al_phasor_init(&x->st4);
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_xfx01_tilde_setup(void)
{
    alien_xfx01_class = class_new(gensym("alien_xfx01~"),
        (t_newmethod)alien_xfx01_new, 0, sizeof(t_alien_xfx01), CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_xfx01_class, t_alien_xfx01, x_f);
    class_addmethod(alien_xfx01_class, (t_method)alien_xfx01_dsp, gensym("dsp"), A_CANT, 0);
}
