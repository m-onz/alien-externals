/* alien_xfx10~ -- auto-bred xeno_fx effect
 * chain=chorus+fold+comb+ring+fold
 * inlet0 signal input | inlet1 param 0..100 (wraps) | outlet0 signal
 */
#include "m_pd.h"
#include "alien.h"
#include <string.h>

static t_class *alien_xfx10_class;

typedef struct _alien_xfx10 {
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
    double st2_buf[8192]; int st2_wr;
    al_phasor st3;
} t_alien_xfx10;

static inline double alien_xfx10_tick(t_alien_xfx10 *x, double in, double pr, double sr)
{
    double pn = al_param100(pr);
    double dry0 = in;
    double s = in;
    double env = al_env_tick(&x->envf, s, 0.03993, 0.0068);
    double lfo = 0.5 + 0.5*sin(AL_2PI*al_phasor_tick(&x->lfo_ph, 5.41359, sr));
    double shv = 0.5 + 0.5*al_sh_tick(&x->sh, &x->rng, 17.66117, sr);
    double fb = x->fbprev;
    al_wander_tick(&x->wnd, &x->rng, 0.1252, sr);
        double w0=al_wander_val(&x->wnd,0), w1=al_wander_val(&x->wnd,1), w2=al_wander_val(&x->wnd,2), w3=al_wander_val(&x->wnd,3), w4=al_wander_val(&x->wnd,4), w5=al_wander_val(&x->wnd,5), w6=al_wander_val(&x->wnd,6), w7=al_wander_val(&x->wnd,7), w8=al_wander_val(&x->wnd,8), w9=al_wander_val(&x->wnd,9);
        { double dry=s; double y=al_chorus_tick(&x->st0,s,al_clamp((13.5209)+(w5)*(-12.5956),0.2,18),al_clamp((5.62872)+(0.0)*(0),0.03,6),sr); s=al_lerp(dry,y,al_clamp((0.74741)+(w6)*(-0.09158),0.1,0.9)); }
        s = al_fold(s, al_clamp((0.68396)+(w3)*(-0.39649),0.03,1));
        { double dry=s; double d=al_clamp(al_clamp((157.81)+(shv)*(-119.467),3,170)*0.001*sr,1.0,8190.0); double y=al_comb(x->st2_buf,8191,&x->st2_wr,s,d,al_clamp((0.7115)+(pn)*(0.69765),-0.86,0.86)); s=al_lerp(dry,y,al_clamp((0.91676)+(pn)*(-0.35674),0.12,0.95)); }
        { double m=sin(AL_2PI*al_phasor_tick(&x->st3,al_clamp(pow(2.0,(3.649)+(w2)*(-3.09223)),-2.32193,10.8138),sr)); s=al_ringmod(s,m,al_clamp((0.05)+(pn)*(0.00586),0.05,1)); }
        s = al_fold(s, al_clamp((0.16558)+(pn)*(-0.11624),0.03,1));
    s = al_lerp(dry0, s, al_clamp((0.55634)+(pn)*(-0.12068),0.25,1));
    s = al_dc_tick(&x->dc, s);
    if (!(s==s)) s = 0.0;
    s = al_softclip(s);
    x->fbprev = fabs(s);
    return s * 0.684;
}

static t_int *alien_xfx10_perform(t_int *w)
{
    t_alien_xfx10 *x = (t_alien_xfx10 *)(w[1]);
    t_sample *in = (t_sample *)(w[2]);
    t_sample *pin = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    int n = (int)(w[5]);
    double sr = x->x_sr;
    al_denorm_off();
    while (n--) *out++ = (t_sample)alien_xfx10_tick(x, *in++, *pin++, sr);
    return (w + 6);
}

static void alien_xfx10_dsp(t_alien_xfx10 *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    dsp_add(alien_xfx10_perform, 5, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

static void *alien_xfx10_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_xfx10 *x = (t_alien_xfx10 *)pd_new(alien_xfx10_class);
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    al_rng_seed(&x->rng, 3500964793u);
    al_wander_init(&x->wnd, &x->rng, 10);
    al_dc_init(&x->dc); al_env_init(&x->envf); al_phasor_init(&x->lfo_ph); al_sh_init(&x->sh);
    x->fbprev = 0.0;
    al_chorus_init(&x->st0);
    memset(x->st2_buf,0,sizeof(x->st2_buf)); x->st2_wr=0;
    al_phasor_init(&x->st3);
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_xfx10_tilde_setup(void)
{
    alien_xfx10_class = class_new(gensym("alien_xfx10~"),
        (t_newmethod)alien_xfx10_new, 0, sizeof(t_alien_xfx10), CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_xfx10_class, t_alien_xfx10, x_f);
    class_addmethod(alien_xfx10_class, (t_method)alien_xfx10_dsp, gensym("dsp"), A_CANT, 0);
}
