/* alien_xfx05~ -- auto-bred xeno_fx effect
 * chain=allpass+ring+downsample+ring
 * inlet0 signal input | inlet1 param 0..100 (wraps) | outlet0 signal
 */
#include "m_pd.h"
#include "alien.h"
#include <string.h>

static t_class *alien_xfx05_class;

typedef struct _alien_xfx05 {
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
    al_phasor st1;
    double st2_hold; int st2_count;
    al_phasor st3;
} t_alien_xfx05;

static inline double alien_xfx05_tick(t_alien_xfx05 *x, double in, double pr, double sr)
{
    double pn = al_param100(pr);
    double dry0 = in;
    double s = in;
    double env = al_env_tick(&x->envf, s, 0.06487, 0.02285);
    double lfo = 0.5 + 0.5*sin(AL_2PI*al_phasor_tick(&x->lfo_ph, 6.5079, sr));
    double shv = 0.5 + 0.5*al_sh_tick(&x->sh, &x->rng, 1.5439, sr);
    double fb = x->fbprev;
    al_wander_tick(&x->wnd, &x->rng, 1.37, sr);
        double w0=al_wander_val(&x->wnd,0), w1=al_wander_val(&x->wnd,1), w2=al_wander_val(&x->wnd,2), w3=al_wander_val(&x->wnd,3), w4=al_wander_val(&x->wnd,4), w5=al_wander_val(&x->wnd,5), w6=al_wander_val(&x->wnd,6), w7=al_wander_val(&x->wnd,7);
        { double dry=s; double d=al_clamp(al_clamp((72.1313)+(pn)*(2.14151),2,90)*0.001*sr,1.0,4094.0); double y=al_apdelay(x->st0_buf,4095,&x->st0_wr,s,d,al_clamp((0.31974)+(pn)*(0.3636),-0.78,0.78)); s=al_lerp(dry,y,al_clamp((0.39697)+(w4)*(0.05666),0.15,1)); }
        { double m=sin(AL_2PI*al_phasor_tick(&x->st1,al_clamp(pow(2.0,(10.2865)+(w7)*(-2.07752)),-2.32193,10.8138),sr)); s=al_ringmod(s,m,al_clamp((0.73293)+(pn)*(0.06751),0.05,1)); }
        { double dry=s; int h=(int)(al_clamp((65.159)+(shv)*(-60.4045),1,90)); if(h<1)h=1; if(--x->st2_count<=0){x->st2_count=h; x->st2_hold=s;} s=al_lerp(dry,x->st2_hold,al_clamp((0.98992)+(w1)*(-0.39139),0.1,1)); }
        { double m=sin(AL_2PI*al_phasor_tick(&x->st3,al_clamp(pow(2.0,(9.28334)+(pn)*(-0.62342)),-2.32193,10.8138),sr)); s=al_ringmod(s,m,al_clamp((0.21258)+(env)*(-0.53274),0.05,1)); }
    s = al_lerp(dry0, s, al_clamp((0.52774)+(pn)*(0.59255),0.25,1));
    s = al_dc_tick(&x->dc, s);
    if (!(s==s)) s = 0.0;
    s = al_softclip(s);
    x->fbprev = fabs(s);
    return s * 0.7468;
}

static t_int *alien_xfx05_perform(t_int *w)
{
    t_alien_xfx05 *x = (t_alien_xfx05 *)(w[1]);
    t_sample *in = (t_sample *)(w[2]);
    t_sample *pin = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    int n = (int)(w[5]);
    double sr = x->x_sr;
    al_denorm_off();
    while (n--) *out++ = (t_sample)alien_xfx05_tick(x, *in++, *pin++, sr);
    return (w + 6);
}

static void alien_xfx05_dsp(t_alien_xfx05 *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    dsp_add(alien_xfx05_perform, 5, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

static void *alien_xfx05_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_xfx05 *x = (t_alien_xfx05 *)pd_new(alien_xfx05_class);
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    al_rng_seed(&x->rng, 3990529143u);
    al_wander_init(&x->wnd, &x->rng, 8);
    al_dc_init(&x->dc); al_env_init(&x->envf); al_phasor_init(&x->lfo_ph); al_sh_init(&x->sh);
    x->fbprev = 0.0;
    memset(x->st0_buf,0,sizeof(x->st0_buf)); x->st0_wr=0;
    al_phasor_init(&x->st1);
    x->st2_hold=0.0; x->st2_count=0;
    al_phasor_init(&x->st3);
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_xfx05_tilde_setup(void)
{
    alien_xfx05_class = class_new(gensym("alien_xfx05~"),
        (t_newmethod)alien_xfx05_new, 0, sizeof(t_alien_xfx05), CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_xfx05_class, t_alien_xfx05, x_f);
    class_addmethod(alien_xfx05_class, (t_method)alien_xfx05_dsp, gensym("dsp"), A_CANT, 0);
}
