/* alien_xfx09~ -- auto-bred xeno_fx effect
 * chain=comb+drive+svf+comb+chorus+fold+fold
 * inlet0 signal input | inlet1 param 0..100 (wraps) | outlet0 signal
 */
#include "m_pd.h"
#include "alien.h"
#include <string.h>

static t_class *alien_xfx09_class;

typedef struct _alien_xfx09 {
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
    double st0_buf[8192]; int st0_wr;
    al_svf st2;
    double st3_buf[8192]; int st3_wr;
    al_chorus st4;
} t_alien_xfx09;

static inline double alien_xfx09_tick(t_alien_xfx09 *x, double in, double pr, double sr)
{
    double pn = al_param100(pr);
    double dry0 = in;
    double s = in;
    double env = al_env_tick(&x->envf, s, 0.09844, 0.0028);
    double lfo = 0.5 + 0.5*sin(AL_2PI*al_phasor_tick(&x->lfo_ph, 6.83902, sr));
    double shv = 0.5 + 0.5*al_sh_tick(&x->sh, &x->rng, 2.94576, sr);
    double fb = x->fbprev;
    al_wander_tick(&x->wnd, &x->rng, 1.1777, sr);
        double w0=al_wander_val(&x->wnd,0), w1=al_wander_val(&x->wnd,1), w2=al_wander_val(&x->wnd,2), w3=al_wander_val(&x->wnd,3), w4=al_wander_val(&x->wnd,4), w5=al_wander_val(&x->wnd,5), w6=al_wander_val(&x->wnd,6), w7=al_wander_val(&x->wnd,7);
        { double dry=s; double d=al_clamp(al_clamp((77.4878)+(fb)*(120.63),3,170)*0.001*sr,1.0,8190.0); double y=al_comb(x->st0_buf,8191,&x->st0_wr,s,d,al_clamp((0.41027)+(w7)*(0.81417),-0.86,0.86)); s=al_lerp(dry,y,al_clamp((0.95)+(lfo)*(-0.09383),0.12,0.95)); }
        { double dry=s; s = al_lerp(dry, al_drive(s + (al_clamp((-0.19861)+(pn)*(-0.1417),-0.25,0.25)), al_clamp((0.5696)+(fb)*(-0.25453),0.02,0.95)), al_clamp((0.5696)+(fb)*(-0.25453),0.02,0.95)); }
        { double dry=s, lp,bp,hp; al_svf_tick3(&x->st2,s,al_clamp(pow(2.0,(6.32193)+(pn)*(-0.97003)),6.32193,13.0532),al_clamp((0.55567)+(lfo)*(-0.49498),0.05,0.92),sr,&lp,&bp,&hp); s=al_lerp(dry,lp,al_clamp((0.55175)+(w0)*(-0.60068),0.15,1)); }
        { double dry=s; double d=al_clamp(al_clamp((82.3171)+(lfo)*(12.2698),3,170)*0.001*sr,1.0,8190.0); double y=al_comb(x->st3_buf,8191,&x->st3_wr,s,d,al_clamp((-0.86)+(pn)*(0.81754),-0.86,0.86)); s=al_lerp(dry,y,al_clamp((0.27457)+(w6)*(0.25713),0.12,0.95)); }
        { double dry=s; double y=al_chorus_tick(&x->st4,s,al_clamp((5.28078)+(w6)*(3.05902),0.2,18),al_clamp((1.55993)+(w0)*(2.3891),0.03,6),sr); s=al_lerp(dry,y,al_clamp((0.63336)+(pn)*(0.10999),0.1,0.9)); }
        s = al_fold(s, al_clamp((0.47594)+(w6)*(-0.04351),0.03,1));
        s = al_fold(s, al_clamp((0.73887)+(0.0)*(0),0.03,1));
    s = al_lerp(dry0, s, al_clamp((0.39052)+(0.0)*(0),0.25,1));
    s = al_dc_tick(&x->dc, s);
    if (!(s==s)) s = 0.0;
    s = al_softclip(s);
    x->fbprev = fabs(s);
    return s * 0.8674;
}

static t_int *alien_xfx09_perform(t_int *w)
{
    t_alien_xfx09 *x = (t_alien_xfx09 *)(w[1]);
    t_sample *in = (t_sample *)(w[2]);
    t_sample *pin = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    int n = (int)(w[5]);
    double sr = x->x_sr;
    al_denorm_off();
    while (n--) *out++ = (t_sample)alien_xfx09_tick(x, *in++, *pin++, sr);
    return (w + 6);
}

static void alien_xfx09_dsp(t_alien_xfx09 *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    dsp_add(alien_xfx09_perform, 5, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

static void *alien_xfx09_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_xfx09 *x = (t_alien_xfx09 *)pd_new(alien_xfx09_class);
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    al_rng_seed(&x->rng, 3617226502u);
    al_wander_init(&x->wnd, &x->rng, 8);
    al_dc_init(&x->dc); al_env_init(&x->envf); al_phasor_init(&x->lfo_ph); al_sh_init(&x->sh);
    x->fbprev = 0.0;
    memset(x->st0_buf,0,sizeof(x->st0_buf)); x->st0_wr=0;
    al_svf_init(&x->st2);
    memset(x->st3_buf,0,sizeof(x->st3_buf)); x->st3_wr=0;
    al_chorus_init(&x->st4);
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_xfx09_tilde_setup(void)
{
    alien_xfx09_class = class_new(gensym("alien_xfx09~"),
        (t_newmethod)alien_xfx09_new, 0, sizeof(t_alien_xfx09), CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_xfx09_class, t_alien_xfx09, x_f);
    class_addmethod(alien_xfx09_class, (t_method)alien_xfx09_dsp, gensym("dsp"), A_CANT, 0);
}
