/* alien_xfx20~ -- auto-bred xeno_fx effect
 * chain=ring+drive+allpass+fold+formant
 * inlet0 signal input | inlet1 param 0..100 (wraps) | outlet0 signal
 */
#include "m_pd.h"
#include "alien.h"
#include <string.h>

static t_class *alien_xfx20_class;

typedef struct _alien_xfx20 {
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
    double st2_buf[4096]; int st2_wr;
    al_formants st4;
} t_alien_xfx20;

static inline double alien_xfx20_tick(t_alien_xfx20 *x, double in, double pr, double sr)
{
    double pn = al_param100(pr);
    double dry0 = in;
    double s = in;
    double env = al_env_tick(&x->envf, s, 0.03684, 0.01468);
    double lfo = 0.5 + 0.5*sin(AL_2PI*al_phasor_tick(&x->lfo_ph, 1.6969, sr));
    double shv = 0.5 + 0.5*al_sh_tick(&x->sh, &x->rng, 11.5217, sr);
    double fb = x->fbprev;
    al_wander_tick(&x->wnd, &x->rng, 2.1548, sr);
        double w0=al_wander_val(&x->wnd,0), w1=al_wander_val(&x->wnd,1), w2=al_wander_val(&x->wnd,2), w3=al_wander_val(&x->wnd,3), w4=al_wander_val(&x->wnd,4), w5=al_wander_val(&x->wnd,5), w6=al_wander_val(&x->wnd,6), w7=al_wander_val(&x->wnd,7), w8=al_wander_val(&x->wnd,8);
        { double m=sin(AL_2PI*al_phasor_tick(&x->st0,al_clamp(pow(2.0,(4.70913)+(0.0)*(0)),-2.32193,10.8138),sr)); s=al_ringmod(s,m,al_clamp((0.79762)+(pn)*(-0.12279),0.05,1)); }
        { double dry=s; s = al_lerp(dry, al_drive(s + (al_clamp((-0.1795)+(fb)*(0.15131),-0.25,0.25)), al_clamp((0.66875)+(w8)*(0.41826),0.02,0.95)), al_clamp((0.66875)+(w8)*(0.41826),0.02,0.95)); }
        { double dry=s; double d=al_clamp(al_clamp((35.5322)+(shv)*(58.469),2,90)*0.001*sr,1.0,4094.0); double y=al_apdelay(x->st2_buf,4095,&x->st2_wr,s,d,al_clamp((0.04266)+(w8)*(0.50046),-0.78,0.78)); s=al_lerp(dry,y,al_clamp((0.98238)+(lfo)*(-0.49065),0.15,1)); }
        s = al_fold(s, al_clamp((0.32894)+(shv)*(-0.23793),0.03,1));
        { double dry=s; s=al_lerp(dry, al_formants_tick(&x->st4,s,al_clamp((0.24249)+(w7)*(-0.74412),0,1),al_clamp((0.70241)+(pn)*(0.10915),0,1),sr), al_clamp((0.93345)+(pn)*(-0.0126),0.15,0.95)); }
    s = al_lerp(dry0, s, al_clamp((0.64528)+(pn)*(-0.24013),0.25,1));
    s = al_dc_tick(&x->dc, s);
    if (!(s==s)) s = 0.0;
    s = al_softclip(s);
    x->fbprev = fabs(s);
    return s * 0.7974;
}

static t_int *alien_xfx20_perform(t_int *w)
{
    t_alien_xfx20 *x = (t_alien_xfx20 *)(w[1]);
    t_sample *in = (t_sample *)(w[2]);
    t_sample *pin = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    int n = (int)(w[5]);
    double sr = x->x_sr;
    al_denorm_off();
    while (n--) *out++ = (t_sample)alien_xfx20_tick(x, *in++, *pin++, sr);
    return (w + 6);
}

static void alien_xfx20_dsp(t_alien_xfx20 *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    dsp_add(alien_xfx20_perform, 5, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

static void *alien_xfx20_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_xfx20 *x = (t_alien_xfx20 *)pd_new(alien_xfx20_class);
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    al_rng_seed(&x->rng, 3769962844u);
    al_wander_init(&x->wnd, &x->rng, 9);
    al_dc_init(&x->dc); al_env_init(&x->envf); al_phasor_init(&x->lfo_ph); al_sh_init(&x->sh);
    x->fbprev = 0.0;
    al_phasor_init(&x->st0);
    memset(x->st2_buf,0,sizeof(x->st2_buf)); x->st2_wr=0;
    al_formants_init(&x->st4);
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_xfx20_tilde_setup(void)
{
    alien_xfx20_class = class_new(gensym("alien_xfx20~"),
        (t_newmethod)alien_xfx20_new, 0, sizeof(t_alien_xfx20), CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_xfx20_class, t_alien_xfx20, x_f);
    class_addmethod(alien_xfx20_class, (t_method)alien_xfx20_dsp, gensym("dsp"), A_CANT, 0);
}
