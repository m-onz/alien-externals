/* alien_xfx16~ -- auto-bred xeno_fx effect
 * chain=ring+fold
 * inlet0 signal input | inlet1 param 0..100 (wraps) | outlet0 signal
 */
#include "m_pd.h"
#include "alien.h"
#include <string.h>

static t_class *alien_xfx16_class;

typedef struct _alien_xfx16 {
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
} t_alien_xfx16;

static inline double alien_xfx16_tick(t_alien_xfx16 *x, double in, double pr, double sr)
{
    double pn = al_param100(pr);
    double dry0 = in;
    double s = in;
    double env = al_env_tick(&x->envf, s, 0.06933, 0.01692);
    double lfo = 0.5 + 0.5*sin(AL_2PI*al_phasor_tick(&x->lfo_ph, 1.4149, sr));
    double shv = 0.5 + 0.5*al_sh_tick(&x->sh, &x->rng, 2.20303, sr);
    double fb = x->fbprev;
    al_wander_tick(&x->wnd, &x->rng, 4.0, sr);
        double w0=al_wander_val(&x->wnd,0), w1=al_wander_val(&x->wnd,1), w2=al_wander_val(&x->wnd,2), w3=al_wander_val(&x->wnd,3), w4=al_wander_val(&x->wnd,4), w5=al_wander_val(&x->wnd,5);
        { double m=sin(AL_2PI*al_phasor_tick(&x->st0,al_clamp(pow(2.0,(10.0751)+(w2)*(-2.97804)),-2.32193,10.8138),sr)); s=al_ringmod(s,m,al_clamp((0.93039)+(w4)*(-0.53553),0.05,1)); }
        s = al_fold(s, al_clamp((0.38347)+(w5)*(0.67976),0.03,1));
    s = al_lerp(dry0, s, al_clamp((0.55249)+(pn)*(-0.48342),0.25,1));
    s = al_dc_tick(&x->dc, s);
    if (!(s==s)) s = 0.0;
    s = al_softclip(s);
    x->fbprev = fabs(s);
    return s * 0.8147;
}

static t_int *alien_xfx16_perform(t_int *w)
{
    t_alien_xfx16 *x = (t_alien_xfx16 *)(w[1]);
    t_sample *in = (t_sample *)(w[2]);
    t_sample *pin = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    int n = (int)(w[5]);
    double sr = x->x_sr;
    al_denorm_off();
    while (n--) *out++ = (t_sample)alien_xfx16_tick(x, *in++, *pin++, sr);
    return (w + 6);
}

static void alien_xfx16_dsp(t_alien_xfx16 *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    dsp_add(alien_xfx16_perform, 5, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

static void *alien_xfx16_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_xfx16 *x = (t_alien_xfx16 *)pd_new(alien_xfx16_class);
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    al_rng_seed(&x->rng, 3461780021u);
    al_wander_init(&x->wnd, &x->rng, 6);
    al_dc_init(&x->dc); al_env_init(&x->envf); al_phasor_init(&x->lfo_ph); al_sh_init(&x->sh);
    x->fbprev = 0.0;
    al_phasor_init(&x->st0);
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_xfx16_tilde_setup(void)
{
    alien_xfx16_class = class_new(gensym("alien_xfx16~"),
        (t_newmethod)alien_xfx16_new, 0, sizeof(t_alien_xfx16), CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_xfx16_class, t_alien_xfx16, x_f);
    class_addmethod(alien_xfx16_class, (t_method)alien_xfx16_dsp, gensym("dsp"), A_CANT, 0);
}
