/* alien_xfx18~ -- auto-bred xeno_fx effect
 * chain=tremolo+gate+fold+gate+comb
 * inlet0 signal input | inlet1 param 0..100 (wraps) | outlet0 signal
 */
#include "m_pd.h"
#include "alien.h"
#include <string.h>

static t_class *alien_xfx18_class;

typedef struct _alien_xfx18 {
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
    al_phasor st0;
    double st4_buf[8192]; int st4_wr;
} t_alien_xfx18;

static inline double alien_xfx18_tick(t_alien_xfx18 *x, double in, double pr, double sr)
{
    double pn = al_param100(pr);
    double dry0 = in;
    double s = in;
    double env = al_env_tick(&x->envf, s, 0.04255, 0.01446);
    double lfo = 0.5 + 0.5*sin(AL_2PI*al_phasor_tick(&x->lfo_ph, 8.31683, sr));
    double shv = 0.5 + 0.5*al_sh_tick(&x->sh, &x->rng, 7.0912, sr);
    double fb = x->fbprev;
    al_wander_tick(&x->wnd, &x->rng, 1.2594, sr);
        double w0=al_wander_val(&x->wnd,0), w1=al_wander_val(&x->wnd,1), w2=al_wander_val(&x->wnd,2), w3=al_wander_val(&x->wnd,3), w4=al_wander_val(&x->wnd,4), w5=al_wander_val(&x->wnd,5), w6=al_wander_val(&x->wnd,6), w7=al_wander_val(&x->wnd,7);
        { double l=0.5+0.5*sin(AL_2PI*al_phasor_tick(&x->st0,al_clamp((9.43849)+(pn)*(1.38401),0.05,18),sr)); s *= 1.0 - (al_clamp((0.56825)+(pn)*(-0.62309),0.05,1))*(1.0-l); }
        { double dry=s; double g=al_clamp((env-(al_clamp((0.27692)+(shv)*(-0.18654),0.01,0.35)))/(al_clamp((0.17828)+(w4)*(0.16772),0.01,0.25)),0.0,1.0); s=al_lerp(dry,s*g,al_clamp((0.39655)+(w1)*(0.37672),0.15,1)); }
        s = al_fold(s, al_clamp((0.83432)+(0.0)*(0),0.03,1));
        { double dry=s; double g=al_clamp((env-(al_clamp((0.01)+(lfo)*(-0.00206),0.01,0.35)))/(al_clamp((0.0364)+(shv)*(-0.1712),0.01,0.25)),0.0,1.0); s=al_lerp(dry,s*g,al_clamp((0.6518)+(pn)*(0.04287),0.15,1)); }
        { double dry=s; double d=al_clamp(al_clamp((68.6726)+(0.0)*(0),3,170)*0.001*sr,1.0,8190.0); double y=al_comb(x->st4_buf,8191,&x->st4_wr,s,d,al_clamp((0.21802)+(w4)*(0.70719),-0.86,0.86)); s=al_lerp(dry,y,al_clamp((0.51944)+(w4)*(-0.03874),0.12,0.95)); }
    s = al_lerp(dry0, s, al_clamp((0.74519)+(0.0)*(0),0.25,1));
    s = al_dc_tick(&x->dc, s);
    if (!(s==s)) s = 0.0;
    s = al_softclip(s);
    x->fbprev = fabs(s);
    return s * 0.9281;
}

static t_int *alien_xfx18_perform(t_int *w)
{
    t_alien_xfx18 *x = (t_alien_xfx18 *)(w[1]);
    t_sample *in = (t_sample *)(w[2]);
    t_sample *pin = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    int n = (int)(w[5]);
    double sr = x->x_sr;
    al_denorm_off();
    while (n--) *out++ = (t_sample)alien_xfx18_tick(x, *in++, *pin++, sr);
    return (w + 6);
}

static void alien_xfx18_dsp(t_alien_xfx18 *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    dsp_add(alien_xfx18_perform, 5, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

static void *alien_xfx18_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_xfx18 *x = (t_alien_xfx18 *)pd_new(alien_xfx18_class);
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    al_rng_seed(&x->rng, 4136073920u);
    al_wander_init(&x->wnd, &x->rng, 8);
    al_dc_init(&x->dc); al_env_init(&x->envf); al_phasor_init(&x->lfo_ph); al_sh_init(&x->sh);
    x->fbprev = 0.0;
    al_phasor_init(&x->st0);
    memset(x->st4_buf,0,sizeof(x->st4_buf)); x->st4_wr=0;
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_xfx18_tilde_setup(void)
{
    alien_xfx18_class = class_new(gensym("alien_xfx18~"),
        (t_newmethod)alien_xfx18_new, 0, sizeof(t_alien_xfx18), CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_xfx18_class, t_alien_xfx18, x_f);
    class_addmethod(alien_xfx18_class, (t_method)alien_xfx18_dsp, gensym("dsp"), A_CANT, 0);
}
