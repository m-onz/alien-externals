/* alien_xfx03~ -- auto-bred xeno_fx effect
 * chain=downsample+crusher+comb+fold+chorus
 * inlet0 signal input | inlet1 param 0..100 (wraps) | outlet0 signal
 */
#include "m_pd.h"
#include "alien.h"
#include <string.h>

static t_class *alien_xfx03_class;

typedef struct _alien_xfx03 {
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
    double st0_hold; int st0_count;
    double st2_buf[8192]; int st2_wr;
    al_chorus st4;
} t_alien_xfx03;

static inline double alien_xfx03_tick(t_alien_xfx03 *x, double in, double pr, double sr)
{
    double pn = al_param100(pr);
    double dry0 = in;
    double s = in;
    double env = al_env_tick(&x->envf, s, 0.07901, 0.01481);
    double lfo = 0.5 + 0.5*sin(AL_2PI*al_phasor_tick(&x->lfo_ph, 0.7928, sr));
    double shv = 0.5 + 0.5*al_sh_tick(&x->sh, &x->rng, 12.5265, sr);
    double fb = x->fbprev;
    al_wander_tick(&x->wnd, &x->rng, 1.4597, sr);
        double w0=al_wander_val(&x->wnd,0), w1=al_wander_val(&x->wnd,1), w2=al_wander_val(&x->wnd,2), w3=al_wander_val(&x->wnd,3), w4=al_wander_val(&x->wnd,4), w5=al_wander_val(&x->wnd,5), w6=al_wander_val(&x->wnd,6), w7=al_wander_val(&x->wnd,7), w8=al_wander_val(&x->wnd,8), w9=al_wander_val(&x->wnd,9);
        { double dry=s; int h=(int)(al_clamp((20.6047)+(w5)*(-27.1267),1,90)); if(h<1)h=1; if(--x->st0_count<=0){x->st0_count=h; x->st0_hold=s;} s=al_lerp(dry,x->st0_hold,al_clamp((0.97733)+(env)*(-0.26326),0.1,1)); }
        { double dry=s; double y=al_bitcrush(s, al_clamp((8.45222)+(w7)*(2.53219),3,14)); s=al_lerp(dry,y,al_clamp((0.70034)+(w1)*(-0.07102),0.1,1)); }
        { double dry=s; double d=al_clamp(al_clamp((159.744)+(w0)*(-107.408),3,170)*0.001*sr,1.0,8190.0); double y=al_comb(x->st2_buf,8191,&x->st2_wr,s,d,al_clamp((0.74436)+(pn)*(0.79548),-0.86,0.86)); s=al_lerp(dry,y,al_clamp((0.27638)+(pn)*(0.41363),0.12,0.95)); }
        s = al_fold(s, al_clamp((0.47176)+(pn)*(0.11219),0.03,1));
        { double dry=s; double y=al_chorus_tick(&x->st4,s,al_clamp((8.20717)+(w2)*(5.61222),0.2,18),al_clamp((4.15135)+(pn)*(1.41943),0.03,6),sr); s=al_lerp(dry,y,al_clamp((0.85342)+(pn)*(-0.01342),0.1,0.9)); }
    s = al_lerp(dry0, s, al_clamp((0.75059)+(pn)*(-0.35229),0.25,1));
    s = al_dc_tick(&x->dc, s);
    if (!(s==s)) s = 0.0;
    s = al_softclip(s);
    x->fbprev = fabs(s);
    return s * 0.901;
}

static t_int *alien_xfx03_perform(t_int *w)
{
    t_alien_xfx03 *x = (t_alien_xfx03 *)(w[1]);
    t_sample *in = (t_sample *)(w[2]);
    t_sample *pin = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    int n = (int)(w[5]);
    double sr = x->x_sr;
    al_denorm_off();
    while (n--) *out++ = (t_sample)alien_xfx03_tick(x, *in++, *pin++, sr);
    return (w + 6);
}

static void alien_xfx03_dsp(t_alien_xfx03 *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    dsp_add(alien_xfx03_perform, 5, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

static void *alien_xfx03_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_xfx03 *x = (t_alien_xfx03 *)pd_new(alien_xfx03_class);
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    al_rng_seed(&x->rng, 1299443872u);
    al_wander_init(&x->wnd, &x->rng, 10);
    al_dc_init(&x->dc); al_env_init(&x->envf); al_phasor_init(&x->lfo_ph); al_sh_init(&x->sh);
    x->fbprev = 0.0;
    x->st0_hold=0.0; x->st0_count=0;
    memset(x->st2_buf,0,sizeof(x->st2_buf)); x->st2_wr=0;
    al_chorus_init(&x->st4);
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_xfx03_tilde_setup(void)
{
    alien_xfx03_class = class_new(gensym("alien_xfx03~"),
        (t_newmethod)alien_xfx03_new, 0, sizeof(t_alien_xfx03), CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_xfx03_class, t_alien_xfx03, x_f);
    class_addmethod(alien_xfx03_class, (t_method)alien_xfx03_dsp, gensym("dsp"), A_CANT, 0);
}
