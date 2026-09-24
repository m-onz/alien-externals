/* alien_xfx19~ -- auto-bred xeno_fx effect
 * chain=allpass+formant+fold+tremolo+fold
 * inlet0 signal input | inlet1 param 0..100 (wraps) | outlet0 signal
 */
#include "m_pd.h"
#include "alien.h"
#include <string.h>

static t_class *alien_xfx19_class;

typedef struct _alien_xfx19 {
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
    double st0_buf[4096]; int st0_wr;
    al_formants st1;
    al_phasor st3;
} t_alien_xfx19;

static inline double alien_xfx19_tick(t_alien_xfx19 *x, double in, double pr, double sr)
{
    double pn = al_param100(pr);
    double dry0 = in;
    double s = in;
    double env = al_env_tick(&x->envf, s, 0.04764, 0.00211);
    double lfo = 0.5 + 0.5*sin(AL_2PI*al_phasor_tick(&x->lfo_ph, 6.31519, sr));
    double shv = 0.5 + 0.5*al_sh_tick(&x->sh, &x->rng, 4.88834, sr);
    double fb = x->fbprev;
    al_wander_tick(&x->wnd, &x->rng, 4.0, sr);
        double w0=al_wander_val(&x->wnd,0), w1=al_wander_val(&x->wnd,1), w2=al_wander_val(&x->wnd,2), w3=al_wander_val(&x->wnd,3), w4=al_wander_val(&x->wnd,4), w5=al_wander_val(&x->wnd,5), w6=al_wander_val(&x->wnd,6), w7=al_wander_val(&x->wnd,7);
        { double dry=s; double d=al_clamp(al_clamp((48.5399)+(lfo)*(-30.7402),2,90)*0.001*sr,1.0,4094.0); double y=al_apdelay(x->st0_buf,4095,&x->st0_wr,s,d,al_clamp((0.63933)+(pn)*(0.29277),-0.78,0.78)); s=al_lerp(dry,y,al_clamp((0.67781)+(fb)*(-0.37512),0.15,1)); }
        { double dry=s; s=al_lerp(dry, al_formants_tick(&x->st1,s,al_clamp((0.46923)+(0.0)*(0),0,1),al_clamp((0.94389)+(pn)*(0.82215),0,1),sr), al_clamp((0.62991)+(pn)*(0.61531),0.15,0.95)); }
        s = al_fold(s, al_clamp((0.46305)+(0.0)*(0),0.03,1));
        { double l=0.5+0.5*sin(AL_2PI*al_phasor_tick(&x->st3,al_clamp((11.4251)+(w6)*(-9.65354),0.05,18),sr)); s *= 1.0 - (al_clamp((0.58846)+(w6)*(0.64546),0.05,1))*(1.0-l); }
        s = al_fold(s, al_clamp((0.15008)+(w2)*(0.05263),0.03,1));
    s = al_lerp(dry0, s, al_clamp((0.25)+(pn)*(-0.2168),0.25,1));
    s = al_dc_tick(&x->dc, s);
    if (!(s==s)) s = 0.0;
    s = al_softclip(s);
    x->fbprev = fabs(s);
    return s * 0.9103;
}

static t_int *alien_xfx19_perform(t_int *w)
{
    t_alien_xfx19 *x = (t_alien_xfx19 *)(w[1]);
    t_sample *in = (t_sample *)(w[2]);
    t_sample *pin = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    int n = (int)(w[5]);
    double sr = x->x_sr;
    al_denorm_off();
    while (n--) *out++ = (t_sample)alien_xfx19_tick(x, *in++, *pin++, sr);
    return (w + 6);
}

static void alien_xfx19_dsp(t_alien_xfx19 *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    dsp_add(alien_xfx19_perform, 5, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

static void *alien_xfx19_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_xfx19 *x = (t_alien_xfx19 *)pd_new(alien_xfx19_class);
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    al_rng_seed(&x->rng, 1285344110u);
    al_wander_init(&x->wnd, &x->rng, 8);
    al_dc_init(&x->dc); al_env_init(&x->envf); al_phasor_init(&x->lfo_ph); al_sh_init(&x->sh);
    x->fbprev = 0.0;
    memset(x->st0_buf,0,sizeof(x->st0_buf)); x->st0_wr=0;
    al_formants_init(&x->st1);
    al_phasor_init(&x->st3);
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_xfx19_tilde_setup(void)
{
    alien_xfx19_class = class_new(gensym("alien_xfx19~"),
        (t_newmethod)alien_xfx19_new, 0, sizeof(t_alien_xfx19), CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_xfx19_class, t_alien_xfx19, x_f);
    class_addmethod(alien_xfx19_class, (t_method)alien_xfx19_dsp, gensym("dsp"), A_CANT, 0);
}
