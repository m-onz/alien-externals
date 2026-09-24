/* alien_xfx11~ -- auto-bred xeno_fx effect
 * chain=crusher+drive+comb
 * inlet0 signal input | inlet1 param 0..100 (wraps) | outlet0 signal
 */
#include "m_pd.h"
#include "alien.h"
#include <string.h>

static t_class *alien_xfx11_class;

typedef struct _alien_xfx11 {
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
    double st2_buf[8192]; int st2_wr;
} t_alien_xfx11;

static inline double alien_xfx11_tick(t_alien_xfx11 *x, double in, double pr, double sr)
{
    double pn = al_param100(pr);
    double dry0 = in;
    double s = in;
    double env = al_env_tick(&x->envf, s, 0.05955, 0.01594);
    double lfo = 0.5 + 0.5*sin(AL_2PI*al_phasor_tick(&x->lfo_ph, 2.54435, sr));
    double shv = 0.5 + 0.5*al_sh_tick(&x->sh, &x->rng, 6.58938, sr);
    double fb = x->fbprev;
    al_wander_tick(&x->wnd, &x->rng, 1.9474, sr);
        double w0=al_wander_val(&x->wnd,0), w1=al_wander_val(&x->wnd,1), w2=al_wander_val(&x->wnd,2), w3=al_wander_val(&x->wnd,3), w4=al_wander_val(&x->wnd,4), w5=al_wander_val(&x->wnd,5), w6=al_wander_val(&x->wnd,6), w7=al_wander_val(&x->wnd,7);
        { double dry=s; double y=al_bitcrush(s, al_clamp((4.9272)+(0.0)*(0),3,14)); s=al_lerp(dry,y,al_clamp((0.66154)+(0.0)*(0),0.1,1)); }
        { double dry=s; s = al_lerp(dry, al_drive(s + (al_clamp((-0.12829)+(pn)*(-0.24135),-0.25,0.25)), al_clamp((0.35577)+(w6)*(-0.22909),0.02,0.95)), al_clamp((0.35577)+(w6)*(-0.22909),0.02,0.95)); }
        { double dry=s; double d=al_clamp(al_clamp((18.8401)+(w5)*(113.909),3,170)*0.001*sr,1.0,8190.0); double y=al_comb(x->st2_buf,8191,&x->st2_wr,s,d,al_clamp((0.70002)+(fb)*(0.17414),-0.86,0.86)); s=al_lerp(dry,y,al_clamp((0.78132)+(shv)*(-0.28912),0.12,0.95)); }
    s = al_lerp(dry0, s, al_clamp((0.92821)+(shv)*(0.44675),0.25,1));
    s = al_dc_tick(&x->dc, s);
    if (!(s==s)) s = 0.0;
    s = al_softclip(s);
    x->fbprev = fabs(s);
    return s * 0.9497;
}

static t_int *alien_xfx11_perform(t_int *w)
{
    t_alien_xfx11 *x = (t_alien_xfx11 *)(w[1]);
    t_sample *in = (t_sample *)(w[2]);
    t_sample *pin = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    int n = (int)(w[5]);
    double sr = x->x_sr;
    al_denorm_off();
    while (n--) *out++ = (t_sample)alien_xfx11_tick(x, *in++, *pin++, sr);
    return (w + 6);
}

static void alien_xfx11_dsp(t_alien_xfx11 *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    dsp_add(alien_xfx11_perform, 5, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

static void *alien_xfx11_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_xfx11 *x = (t_alien_xfx11 *)pd_new(alien_xfx11_class);
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    al_rng_seed(&x->rng, 1254574200u);
    al_wander_init(&x->wnd, &x->rng, 8);
    al_dc_init(&x->dc); al_env_init(&x->envf); al_phasor_init(&x->lfo_ph); al_sh_init(&x->sh);
    x->fbprev = 0.0;
    memset(x->st2_buf,0,sizeof(x->st2_buf)); x->st2_wr=0;
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_xfx11_tilde_setup(void)
{
    alien_xfx11_class = class_new(gensym("alien_xfx11~"),
        (t_newmethod)alien_xfx11_new, 0, sizeof(t_alien_xfx11), CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_xfx11_class, t_alien_xfx11, x_f);
    class_addmethod(alien_xfx11_class, (t_method)alien_xfx11_dsp, gensym("dsp"), A_CANT, 0);
}
