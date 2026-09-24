/* alien_xsnap_ep00012_01~ -- auto-bred specimen (recipe genome; do not hand-edit)
 * source=kuramoto  stages=['drive', 'pmshape', 'svf']
 * inlet0 freq Hz | inlet1 param 0..100 (wraps) | outlet0 signal
 */
#include "m_pd.h"
#include "alien.h"
#include <string.h>

static t_class *alien_xsnap_ep00012_01_class;


typedef struct _alien_xsnap_ep00012_01 {
    t_object x_obj;
    t_float  x_f;
    double   x_sr;
    al_rng    rng;
    al_wander wnd;
    al_dc     dc;
    al_phasor _lfo; al_sh _sh; al_env _env; al_v3 _cha; double _chaacc, _chav, _fbprev;
    al_kura src0;
    al_phasor st1;
    al_svf st2;
} t_alien_xsnap_ep00012_01;

static t_int *alien_xsnap_ep00012_01_perform(t_int *w)
{
    t_alien_xsnap_ep00012_01 *x = (t_alien_xsnap_ep00012_01 *)(w[1]);
    t_sample *fin = (t_sample *)(w[2]);
    t_sample *pin = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    int n = (int)(w[5]);
    double sr = x->x_sr;
    al_denorm_off();   /* flush denormals -> resonator tails can't stall the host */
    while (n--) {
        double f0 = *fin++, pr = *pin++;
        double pn = al_param100(pr);
        double s = 0.0;
        if (f0 < 1.0) f0 = 1.0; if (f0 > sr*0.45) f0 = sr*0.45;
        al_wander_tick(&x->wnd, &x->rng, 1.025, sr);
        double w0=al_wander_val(&x->wnd,0), w1=al_wander_val(&x->wnd,1), w2=al_wander_val(&x->wnd,2), w3=al_wander_val(&x->wnd,3);
        double lfo = al_sin01(al_phasor_tick(&x->_lfo, 1.0588, sr))*0.5+0.5;
        double shv = al_sh_tick(&x->_sh, &x->rng, 10.6971, sr)*0.5+0.5;
        x->_chaacc += 0.01856; if (x->_chaacc>=1.0){ x->_chaacc-=floor(x->_chaacc); x->_chav=al_map_logistic(x->_chav, 3.5383); }
        double cha = x->_chav;
        double env = x->_env.e;
        double fb  = x->_fbprev;
        (void)(lfo+shv+cha+env+fb);
        /* source: kuramoto */
        s = al_kura_tick(&x->src0,f0,sr,al_clamp((0.1222)+(0.0)*(0),(0),(0.6)),al_clamp((2.37)+(pn)*(0.7044),(0),(8)));
        /* stage 0: drive */
        s = al_drive(s, al_clamp((0.1833)+(pn)*(0.3922),(0),(0.85)));
        /* stage 1: pmshape */
        { double _c = al_phasor_tick(&x->st1, al_clamp(f0*al_clamp((2.2706)+(fb)*(1.1329),(0.125),(8)),20.0,sr*0.45), sr);
          double _y = sin(AL_2PI*(_c + al_clamp((1.0863)+(w0)*(3.4896),(0),(5))*s));
          s = al_lerp(s, _y, al_clamp((0.4812)+(env)*(0.398),(0),(1))); }
        /* stage 2: svf */
        s = al_svf_lp(&x->st2, s, al_clamp((80)*pow(2.0,(3.6264)+(pn)*(1.2337)),(60),(15000)), al_clamp((0.1737)+(0.0)*(0),(0),(0.9)), sr);
        s = al_dc_tick(&x->dc, s);
        if (!(s==s)) s = 0.0;
        al_env_tick(&x->_env, s, 0.1696, 0.02847);
        { double _ov = al_softclip(s); x->_fbprev = fabs(_ov);
           *out++ = (t_sample)(_ov * 0.6); }
    }
    return (w + 6);
}

static void alien_xsnap_ep00012_01_dsp(t_alien_xsnap_ep00012_01 *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    dsp_add(alien_xsnap_ep00012_01_perform, 5, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

static void *alien_xsnap_ep00012_01_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_xsnap_ep00012_01 *x = (t_alien_xsnap_ep00012_01 *)pd_new(alien_xsnap_ep00012_01_class);
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    al_rng_seed(&x->rng, 962888093u);
    al_wander_init(&x->wnd, &x->rng, 4);
    al_dc_init(&x->dc);
    al_phasor_init(&x->_lfo); al_sh_init(&x->_sh); al_env_init(&x->_env);
    x->_cha.x=x->_cha.y=x->_cha.z=0.1; x->_chaacc=0.0; x->_chav=0.5; x->_fbprev=0.0;
    al_kura_init(&x->src0,&x->rng,8);
    al_phasor_init(&x->st1);
    al_svf_init(&x->st2);
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_xsnap_ep00012_01_tilde_setup(void)
{
    alien_xsnap_ep00012_01_class = class_new(gensym("alien_xsnap_ep00012_01~"),
        (t_newmethod)alien_xsnap_ep00012_01_new, 0, sizeof(t_alien_xsnap_ep00012_01), CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_xsnap_ep00012_01_class, t_alien_xsnap_ep00012_01, x_f);
    class_addmethod(alien_xsnap_ep00012_01_class, (t_method)alien_xsnap_ep00012_01_dsp, gensym("dsp"), A_CANT, 0);
}
