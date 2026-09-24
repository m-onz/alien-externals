/* alien_xfx15~ -- auto-bred xeno_fx effect
 * chain=fold+tremolo+fold+fold
 * inlet0 signal input | inlet1 param 0..100 (wraps) | outlet0 signal
 */
#include "m_pd.h"
#include "alien.h"
#include <string.h>

static t_class *alien_xfx15_class;

typedef struct _alien_xfx15 {
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
    al_phasor st1;
} t_alien_xfx15;

static inline double alien_xfx15_tick(t_alien_xfx15 *x, double in, double pr, double sr)
{
    double pn = al_param100(pr);
    double dry0 = in;
    double s = in;
    double env = al_env_tick(&x->envf, s, 0.03813, 0.00292);
    double lfo = 0.5 + 0.5*sin(AL_2PI*al_phasor_tick(&x->lfo_ph, 8.16071, sr));
    double shv = 0.5 + 0.5*al_sh_tick(&x->sh, &x->rng, 2.93524, sr);
    double fb = x->fbprev;
    al_wander_tick(&x->wnd, &x->rng, 1.6381, sr);
        double w0=al_wander_val(&x->wnd,0), w1=al_wander_val(&x->wnd,1), w2=al_wander_val(&x->wnd,2), w3=al_wander_val(&x->wnd,3), w4=al_wander_val(&x->wnd,4), w5=al_wander_val(&x->wnd,5), w6=al_wander_val(&x->wnd,6), w7=al_wander_val(&x->wnd,7);
        s = al_fold(s, al_clamp((0.75893)+(0.0)*(0),0.03,1));
        { double l=0.5+0.5*sin(AL_2PI*al_phasor_tick(&x->st1,al_clamp((10.1731)+(w6)*(-8.06101),0.05,18),sr)); s *= 1.0 - (al_clamp((0.58846)+(w6)*(0.64546),0.05,1))*(1.0-l); }
        s = al_fold(s, al_clamp((0.12126)+(env)*(0.11386),0.03,1));
        s = al_fold(s, al_clamp((0.51756)+(lfo)*(-0.14459),0.03,1));
    s = al_lerp(dry0, s, al_clamp((0.38959)+(0.0)*(0),0.25,1));
    s = al_dc_tick(&x->dc, s);
    if (!(s==s)) s = 0.0;
    s = al_softclip(s);
    x->fbprev = fabs(s);
    return s * 0.7549;
}

static t_int *alien_xfx15_perform(t_int *w)
{
    t_alien_xfx15 *x = (t_alien_xfx15 *)(w[1]);
    t_sample *in = (t_sample *)(w[2]);
    t_sample *pin = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    int n = (int)(w[5]);
    double sr = x->x_sr;
    al_denorm_off();
    while (n--) *out++ = (t_sample)alien_xfx15_tick(x, *in++, *pin++, sr);
    return (w + 6);
}

static void alien_xfx15_dsp(t_alien_xfx15 *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    dsp_add(alien_xfx15_perform, 5, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

static void *alien_xfx15_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_xfx15 *x = (t_alien_xfx15 *)pd_new(alien_xfx15_class);
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    al_rng_seed(&x->rng, 1285344110u);
    al_wander_init(&x->wnd, &x->rng, 8);
    al_dc_init(&x->dc); al_env_init(&x->envf); al_phasor_init(&x->lfo_ph); al_sh_init(&x->sh);
    x->fbprev = 0.0;
    al_phasor_init(&x->st1);
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_xfx15_tilde_setup(void)
{
    alien_xfx15_class = class_new(gensym("alien_xfx15~"),
        (t_newmethod)alien_xfx15_new, 0, sizeof(t_alien_xfx15), CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_xfx15_class, t_alien_xfx15, x_f);
    class_addmethod(alien_xfx15_class, (t_method)alien_xfx15_dsp, gensym("dsp"), A_CANT, 0);
}
