/* alien_xfx14~ -- auto-bred xeno_fx effect
 * chain=fold+freqshift+allpass
 * inlet0 signal input | inlet1 param 0..100 (wraps) | outlet0 signal
 */
#include "m_pd.h"
#include "alien.h"
#include <string.h>

static t_class *alien_xfx14_class;

typedef struct _alien_xfx14 {
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
    al_hilbert st1_h; al_phasor st1_p;
    double st2_buf[4096]; int st2_wr;
} t_alien_xfx14;

static inline double alien_xfx14_tick(t_alien_xfx14 *x, double in, double pr, double sr)
{
    double pn = al_param100(pr);
    double dry0 = in;
    double s = in;
    double env = al_env_tick(&x->envf, s, 0.02056, 0.01248);
    double lfo = 0.5 + 0.5*sin(AL_2PI*al_phasor_tick(&x->lfo_ph, 1.88696, sr));
    double shv = 0.5 + 0.5*al_sh_tick(&x->sh, &x->rng, 4.40385, sr);
    double fb = x->fbprev;
    al_wander_tick(&x->wnd, &x->rng, 1.7843, sr);
        double w0=al_wander_val(&x->wnd,0), w1=al_wander_val(&x->wnd,1), w2=al_wander_val(&x->wnd,2), w3=al_wander_val(&x->wnd,3), w4=al_wander_val(&x->wnd,4), w5=al_wander_val(&x->wnd,5), w6=al_wander_val(&x->wnd,6), w7=al_wander_val(&x->wnd,7);
        s = al_fold(s, al_clamp((0.85724)+(pn)*(-0.27755),0.03,1));
        { double dry=s,I,Q,ph,c,q,y; al_hilbert_tick(&x->st1_h,s,&I,&Q); ph=al_phasor_tick(&x->st1_p,fabs(al_clamp((-623.215)+(w0)*(-583.627),-900,900)),sr); c=cos(AL_2PI*ph); q=sin(AL_2PI*ph); y=(al_clamp((-623.215)+(w0)*(-583.627),-900,900)>=0? I*c-Q*q : I*c+Q*q); s=al_lerp(dry,y,al_clamp((0.95)+(shv)*(-0.84793),0.08,0.95)); }
        { double dry=s; double d=al_clamp(al_clamp((76.8009)+(pn)*(43.5279),2,90)*0.001*sr,1.0,4094.0); double y=al_apdelay(x->st2_buf,4095,&x->st2_wr,s,d,al_clamp((0.13474)+(pn)*(0.04083),-0.78,0.78)); s=al_lerp(dry,y,al_clamp((0.81062)+(w1)*(-0.24821),0.15,1)); }
    s = al_lerp(dry0, s, al_clamp((0.82122)+(env)*(0.27485),0.25,1));
    s = al_dc_tick(&x->dc, s);
    if (!(s==s)) s = 0.0;
    s = al_softclip(s);
    x->fbprev = fabs(s);
    return s * 0.6591;
}

static t_int *alien_xfx14_perform(t_int *w)
{
    t_alien_xfx14 *x = (t_alien_xfx14 *)(w[1]);
    t_sample *in = (t_sample *)(w[2]);
    t_sample *pin = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    int n = (int)(w[5]);
    double sr = x->x_sr;
    al_denorm_off();
    while (n--) *out++ = (t_sample)alien_xfx14_tick(x, *in++, *pin++, sr);
    return (w + 6);
}

static void alien_xfx14_dsp(t_alien_xfx14 *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    dsp_add(alien_xfx14_perform, 5, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

static void *alien_xfx14_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_xfx14 *x = (t_alien_xfx14 *)pd_new(alien_xfx14_class);
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    al_rng_seed(&x->rng, 1651356842u);
    al_wander_init(&x->wnd, &x->rng, 8);
    al_dc_init(&x->dc); al_env_init(&x->envf); al_phasor_init(&x->lfo_ph); al_sh_init(&x->sh);
    x->fbprev = 0.0;
    al_hilbert_init(&x->st1_h); al_phasor_init(&x->st1_p);
    memset(x->st2_buf,0,sizeof(x->st2_buf)); x->st2_wr=0;
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_xfx14_tilde_setup(void)
{
    alien_xfx14_class = class_new(gensym("alien_xfx14~"),
        (t_newmethod)alien_xfx14_new, 0, sizeof(t_alien_xfx14), CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_xfx14_class, t_alien_xfx14, x_f);
    class_addmethod(alien_xfx14_class, (t_method)alien_xfx14_dsp, gensym("dsp"), A_CANT, 0);
}
