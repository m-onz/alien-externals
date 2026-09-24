/* alien_xfx08~ -- auto-bred xeno_fx effect
 * chain=formant+crusher+ring+tremolo+crusher
 * inlet0 signal input | inlet1 param 0..100 (wraps) | outlet0 signal
 */
#include "m_pd.h"
#include "alien.h"
#include <string.h>

static t_class *alien_xfx08_class;

typedef struct _alien_xfx08 {
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
    al_formants st0;
    al_phasor st2;
    al_phasor st3;
} t_alien_xfx08;

static inline double alien_xfx08_tick(t_alien_xfx08 *x, double in, double pr, double sr)
{
    double pn = al_param100(pr);
    double dry0 = in;
    double s = in;
    double env = al_env_tick(&x->envf, s, 0.08514, 0.02006);
    double lfo = 0.5 + 0.5*sin(AL_2PI*al_phasor_tick(&x->lfo_ph, 2.24078, sr));
    double shv = 0.5 + 0.5*al_sh_tick(&x->sh, &x->rng, 2.31051, sr);
    double fb = x->fbprev;
    al_wander_tick(&x->wnd, &x->rng, 2.2076, sr);
        double w0=al_wander_val(&x->wnd,0), w1=al_wander_val(&x->wnd,1), w2=al_wander_val(&x->wnd,2), w3=al_wander_val(&x->wnd,3), w4=al_wander_val(&x->wnd,4);
        { double dry=s; s=al_lerp(dry, al_formants_tick(&x->st0,s,al_clamp((0.58173)+(w0)*(0.56961),0,1),al_clamp((0.87476)+(fb)*(0.73444),0,1),sr), al_clamp((0.27702)+(pn)*(0.69258),0.15,0.95)); }
        { double dry=s; double y=al_bitcrush(s, al_clamp((5.99796)+(fb)*(-4.66289),3,14)); s=al_lerp(dry,y,al_clamp((0.53817)+(0.0)*(0),0.1,1)); }
        { double m=sin(AL_2PI*al_phasor_tick(&x->st2,al_clamp(pow(2.0,(2.70783)+(env)*(3.40355)),-2.32193,10.8138),sr)); s=al_ringmod(s,m,al_clamp((0.3014)+(w3)*(-0.47283),0.05,1)); }
        { double l=0.5+0.5*sin(AL_2PI*al_phasor_tick(&x->st3,al_clamp((8.75069)+(0.0)*(0),0.05,18),sr)); s *= 1.0 - (al_clamp((0.51445)+(lfo)*(0.37172),0.05,1))*(1.0-l); }
        { double dry=s; double y=al_bitcrush(s, al_clamp((9.26462)+(fb)*(6.0307),3,14)); s=al_lerp(dry,y,al_clamp((0.58985)+(fb)*(0.00179),0.1,1)); }
    s = al_lerp(dry0, s, al_clamp((0.91425)+(fb)*(-0.36374),0.25,1));
    s = al_dc_tick(&x->dc, s);
    if (!(s==s)) s = 0.0;
    s = al_softclip(s);
    x->fbprev = fabs(s);
    return s * 0.8308;
}

static t_int *alien_xfx08_perform(t_int *w)
{
    t_alien_xfx08 *x = (t_alien_xfx08 *)(w[1]);
    t_sample *in = (t_sample *)(w[2]);
    t_sample *pin = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    int n = (int)(w[5]);
    double sr = x->x_sr;
    al_denorm_off();
    while (n--) *out++ = (t_sample)alien_xfx08_tick(x, *in++, *pin++, sr);
    return (w + 6);
}

static void alien_xfx08_dsp(t_alien_xfx08 *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    dsp_add(alien_xfx08_perform, 5, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

static void *alien_xfx08_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_xfx08 *x = (t_alien_xfx08 *)pd_new(alien_xfx08_class);
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    al_rng_seed(&x->rng, 3646015000u);
    al_wander_init(&x->wnd, &x->rng, 5);
    al_dc_init(&x->dc); al_env_init(&x->envf); al_phasor_init(&x->lfo_ph); al_sh_init(&x->sh);
    x->fbprev = 0.0;
    al_formants_init(&x->st0);
    al_phasor_init(&x->st2);
    al_phasor_init(&x->st3);
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_xfx08_tilde_setup(void)
{
    alien_xfx08_class = class_new(gensym("alien_xfx08~"),
        (t_newmethod)alien_xfx08_new, 0, sizeof(t_alien_xfx08), CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_xfx08_class, t_alien_xfx08, x_f);
    class_addmethod(alien_xfx08_class, (t_method)alien_xfx08_dsp, gensym("dsp"), A_CANT, 0);
}
