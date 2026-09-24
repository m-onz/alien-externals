/* alien_xfx17~ -- auto-bred xeno_fx effect
 * chain=fold+comb
 * inlet0 signal input | inlet1 param 0..100 (wraps) | outlet0 signal
 */
#include "m_pd.h"
#include "alien.h"
#include <string.h>

static t_class *alien_xfx17_class;

typedef struct _alien_xfx17 {
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
    double st1_buf[8192]; int st1_wr;
} t_alien_xfx17;

static inline double alien_xfx17_tick(t_alien_xfx17 *x, double in, double pr, double sr)
{
    double pn = al_param100(pr);
    double dry0 = in;
    double s = in;
    double env = al_env_tick(&x->envf, s, 0.03293, 0.01668);
    double lfo = 0.5 + 0.5*sin(AL_2PI*al_phasor_tick(&x->lfo_ph, 6.79518, sr));
    double shv = 0.5 + 0.5*al_sh_tick(&x->sh, &x->rng, 2.62943, sr);
    double fb = x->fbprev;
    al_wander_tick(&x->wnd, &x->rng, 2.9806, sr);
        double w0=al_wander_val(&x->wnd,0), w1=al_wander_val(&x->wnd,1), w2=al_wander_val(&x->wnd,2), w3=al_wander_val(&x->wnd,3), w4=al_wander_val(&x->wnd,4), w5=al_wander_val(&x->wnd,5), w6=al_wander_val(&x->wnd,6), w7=al_wander_val(&x->wnd,7), w8=al_wander_val(&x->wnd,8);
        s = al_fold(s, al_clamp((0.4984)+(w6)*(-0.41967),0.03,1));
        { double dry=s; double d=al_clamp(al_clamp((48.6585)+(env)*(21.014),3,170)*0.001*sr,1.0,8190.0); double y=al_comb(x->st1_buf,8191,&x->st1_wr,s,d,al_clamp((-0.44484)+(pn)*(-0.50018),-0.86,0.86)); s=al_lerp(dry,y,al_clamp((0.3087)+(w1)*(0.26427),0.12,0.95)); }
    s = al_lerp(dry0, s, al_clamp((0.62383)+(w0)*(0.50966),0.25,1));
    s = al_dc_tick(&x->dc, s);
    if (!(s==s)) s = 0.0;
    s = al_softclip(s);
    x->fbprev = fabs(s);
    return s * 0.7417;
}

static t_int *alien_xfx17_perform(t_int *w)
{
    t_alien_xfx17 *x = (t_alien_xfx17 *)(w[1]);
    t_sample *in = (t_sample *)(w[2]);
    t_sample *pin = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    int n = (int)(w[5]);
    double sr = x->x_sr;
    al_denorm_off();
    while (n--) *out++ = (t_sample)alien_xfx17_tick(x, *in++, *pin++, sr);
    return (w + 6);
}

static void alien_xfx17_dsp(t_alien_xfx17 *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    dsp_add(alien_xfx17_perform, 5, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

static void *alien_xfx17_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_xfx17 *x = (t_alien_xfx17 *)pd_new(alien_xfx17_class);
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    al_rng_seed(&x->rng, 2188948432u);
    al_wander_init(&x->wnd, &x->rng, 9);
    al_dc_init(&x->dc); al_env_init(&x->envf); al_phasor_init(&x->lfo_ph); al_sh_init(&x->sh);
    x->fbprev = 0.0;
    memset(x->st1_buf,0,sizeof(x->st1_buf)); x->st1_wr=0;
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_xfx17_tilde_setup(void)
{
    alien_xfx17_class = class_new(gensym("alien_xfx17~"),
        (t_newmethod)alien_xfx17_new, 0, sizeof(t_alien_xfx17), CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_xfx17_class, t_alien_xfx17, x_f);
    class_addmethod(alien_xfx17_class, (t_method)alien_xfx17_dsp, gensym("dsp"), A_CANT, 0);
}
