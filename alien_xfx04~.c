/* alien_xfx04~ -- auto-bred xeno_fx effect
 * chain=downsample+freqshift+chorus
 * inlet0 signal input | inlet1 param 0..100 (wraps) | outlet0 signal
 */
#include "m_pd.h"
#include "alien.h"
#include <string.h>

static t_class *alien_xfx04_class;

typedef struct _alien_xfx04 {
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
    double st0_hold; int st0_count;
    al_hilbert st1_h; al_phasor st1_p;
    al_chorus st2;
} t_alien_xfx04;

static inline double alien_xfx04_tick(t_alien_xfx04 *x, double in, double pr, double sr)
{
    double pn = al_param100(pr);
    double dry0 = in;
    double s = in;
    double env = al_env_tick(&x->envf, s, 0.08173, 0.02092);
    double lfo = 0.5 + 0.5*sin(AL_2PI*al_phasor_tick(&x->lfo_ph, 4.64131, sr));
    double shv = 0.5 + 0.5*al_sh_tick(&x->sh, &x->rng, 6.07762, sr);
    double fb = x->fbprev;
    al_wander_tick(&x->wnd, &x->rng, 1.762, sr);
        double w0=al_wander_val(&x->wnd,0), w1=al_wander_val(&x->wnd,1), w2=al_wander_val(&x->wnd,2), w3=al_wander_val(&x->wnd,3), w4=al_wander_val(&x->wnd,4), w5=al_wander_val(&x->wnd,5), w6=al_wander_val(&x->wnd,6);
        { double dry=s; int h=(int)(al_clamp((52.7354)+(pn)*(-67.1753),1,90)); if(h<1)h=1; if(--x->st0_count<=0){x->st0_count=h; x->st0_hold=s;} s=al_lerp(dry,x->st0_hold,al_clamp((0.62889)+(pn)*(0.48414),0.1,1)); }
        { double dry=s,I,Q,ph,c,q,y; al_hilbert_tick(&x->st1_h,s,&I,&Q); ph=al_phasor_tick(&x->st1_p,fabs(al_clamp((-405.379)+(w3)*(-6.19756),-900,900)),sr); c=cos(AL_2PI*ph); q=sin(AL_2PI*ph); y=(al_clamp((-405.379)+(w3)*(-6.19756),-900,900)>=0? I*c-Q*q : I*c+Q*q); s=al_lerp(dry,y,al_clamp((0.95)+(pn)*(0.57758),0.08,0.95)); }
        { double dry=s; double y=al_chorus_tick(&x->st2,s,al_clamp((6.73357)+(pn)*(5.38391),0.2,18),al_clamp((4.92766)+(lfo)*(4.38107),0.03,6),sr); s=al_lerp(dry,y,al_clamp((0.80321)+(lfo)*(-0.00104),0.1,0.9)); }
    s = al_lerp(dry0, s, al_clamp((0.90908)+(w0)*(0.48019),0.25,1));
    s = al_dc_tick(&x->dc, s);
    if (!(s==s)) s = 0.0;
    s = al_softclip(s);
    x->fbprev = fabs(s);
    return s * 0.8864;
}

static t_int *alien_xfx04_perform(t_int *w)
{
    t_alien_xfx04 *x = (t_alien_xfx04 *)(w[1]);
    t_sample *in = (t_sample *)(w[2]);
    t_sample *pin = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    int n = (int)(w[5]);
    double sr = x->x_sr;
    al_denorm_off();
    while (n--) *out++ = (t_sample)alien_xfx04_tick(x, *in++, *pin++, sr);
    return (w + 6);
}

static void alien_xfx04_dsp(t_alien_xfx04 *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    dsp_add(alien_xfx04_perform, 5, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

static void *alien_xfx04_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_xfx04 *x = (t_alien_xfx04 *)pd_new(alien_xfx04_class);
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    al_rng_seed(&x->rng, 3872912648u);
    al_wander_init(&x->wnd, &x->rng, 7);
    al_dc_init(&x->dc); al_env_init(&x->envf); al_phasor_init(&x->lfo_ph); al_sh_init(&x->sh);
    x->fbprev = 0.0;
    x->st0_hold=0.0; x->st0_count=0;
    al_hilbert_init(&x->st1_h); al_phasor_init(&x->st1_p);
    al_chorus_init(&x->st2);
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_xfx04_tilde_setup(void)
{
    alien_xfx04_class = class_new(gensym("alien_xfx04~"),
        (t_newmethod)alien_xfx04_new, 0, sizeof(t_alien_xfx04), CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_xfx04_class, t_alien_xfx04, x_f);
    class_addmethod(alien_xfx04_class, (t_method)alien_xfx04_dsp, gensym("dsp"), A_CANT, 0);
}
