/* alien_xfx07~ -- auto-bred xeno_fx effect
 * chain=ring+fold+svf
 * inlet0 signal input | inlet1 param 0..100 (wraps) | outlet0 signal
 */
#include "m_pd.h"
#include "alien.h"
#include <string.h>

static t_class *alien_xfx07_class;

typedef struct _alien_xfx07 {
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
    al_svf st2;
} t_alien_xfx07;

static inline double alien_xfx07_tick(t_alien_xfx07 *x, double in, double pr, double sr)
{
    double pn = al_param100(pr);
    double dry0 = in;
    double s = in;
    double env = al_env_tick(&x->envf, s, 0.04838, 0.0336);
    double lfo = 0.5 + 0.5*sin(AL_2PI*al_phasor_tick(&x->lfo_ph, 3.33655, sr));
    double shv = 0.5 + 0.5*al_sh_tick(&x->sh, &x->rng, 10.79256, sr);
    double fb = x->fbprev;
    al_wander_tick(&x->wnd, &x->rng, 0.2036, sr);
        double w0=al_wander_val(&x->wnd,0), w1=al_wander_val(&x->wnd,1), w2=al_wander_val(&x->wnd,2), w3=al_wander_val(&x->wnd,3), w4=al_wander_val(&x->wnd,4);
        { double m=sin(AL_2PI*al_phasor_tick(&x->st0,al_clamp(pow(2.0,(10.4402)+(w2)*(2.94444)),-2.32193,10.8138),sr)); s=al_ringmod(s,m,al_clamp((0.47341)+(lfo)*(-0.51754),0.05,1)); }
        s = al_fold(s, al_clamp((0.57177)+(shv)*(-0.09069),0.03,1));
        { double dry=s, lp,bp,hp; al_svf_tick3(&x->st2,s,al_clamp(pow(2.0,(8.61252)+(w4)*(-2.72313)),6.32193,13.0532),al_clamp((0.87951)+(pn)*(0.31417),0.05,0.92),sr,&lp,&bp,&hp); s=al_lerp(dry,bp,al_clamp((0.58662)+(pn)*(0.67231),0.15,1)); }
    s = al_lerp(dry0, s, al_clamp((0.65098)+(pn)*(0.37293),0.25,1));
    s = al_dc_tick(&x->dc, s);
    if (!(s==s)) s = 0.0;
    s = al_softclip(s);
    x->fbprev = fabs(s);
    return s * 0.6005;
}

static t_int *alien_xfx07_perform(t_int *w)
{
    t_alien_xfx07 *x = (t_alien_xfx07 *)(w[1]);
    t_sample *in = (t_sample *)(w[2]);
    t_sample *pin = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    int n = (int)(w[5]);
    double sr = x->x_sr;
    al_denorm_off();
    while (n--) *out++ = (t_sample)alien_xfx07_tick(x, *in++, *pin++, sr);
    return (w + 6);
}

static void alien_xfx07_dsp(t_alien_xfx07 *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    dsp_add(alien_xfx07_perform, 5, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

static void *alien_xfx07_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_xfx07 *x = (t_alien_xfx07 *)pd_new(alien_xfx07_class);
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    al_rng_seed(&x->rng, 3749617833u);
    al_wander_init(&x->wnd, &x->rng, 5);
    al_dc_init(&x->dc); al_env_init(&x->envf); al_phasor_init(&x->lfo_ph); al_sh_init(&x->sh);
    x->fbprev = 0.0;
    al_phasor_init(&x->st0);
    al_svf_init(&x->st2);
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_xfx07_tilde_setup(void)
{
    alien_xfx07_class = class_new(gensym("alien_xfx07~"),
        (t_newmethod)alien_xfx07_new, 0, sizeof(t_alien_xfx07), CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_xfx07_class, t_alien_xfx07, x_f);
    class_addmethod(alien_xfx07_class, (t_method)alien_xfx07_dsp, gensym("dsp"), A_CANT, 0);
}
