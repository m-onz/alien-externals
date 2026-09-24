/* alien_xfreesnap_ep10000_08~ -- auto-bred specimen (recipe genome; do not hand-edit)
 * source=dsf  stages=['svf', 'fold', 'comb', 'svf']
 * inlet0 freq Hz | inlet1 param 0..100 (wraps) | outlet0 signal
 */
#include "m_pd.h"
#include "alien.h"
#include <string.h>

static t_class *alien_xfreesnap_ep10000_08_class;
static const double v1_st1_w[5]={0.077,0.132,0.23,0.76,-0.557};

typedef struct _alien_xfreesnap_ep10000_08 {
    t_object x_obj;
    t_float  x_f;
    double   x_sr;
    al_rng    rng;
    al_wander wnd;
    al_dc     dc;
    al_phasor _lfo; al_sh _sh; al_env _env; al_v3 _cha; double _chaacc, _chav, _fbprev; double _vbus[3]; double _ofbus; double _seqph, _seqenv; int _seqi;
    al_phasor v0_src0;
    al_svf v0_st0;
    double v0_st2_b[4096]; int v0_st2_wr;
    al_svf v0_st3;
    al_cml v1_src0;
    al_ladder v1_st0;
    al_svf v1_st2;
} t_alien_xfreesnap_ep10000_08;

static t_int *alien_xfreesnap_ep10000_08_perform(t_int *w)
{
    t_alien_xfreesnap_ep10000_08 *x = (t_alien_xfreesnap_ep10000_08 *)(w[1]);
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
        al_wander_tick(&x->wnd, &x->rng, 1.207, sr);
        double w0=al_wander_val(&x->wnd,0), w1=al_wander_val(&x->wnd,1), w2=al_wander_val(&x->wnd,2), w3=al_wander_val(&x->wnd,3);
        double lfo = al_sin01(al_phasor_tick(&x->_lfo, 8.0476, sr))*0.5+0.5;
        double shv = al_sh_tick(&x->_sh, &x->rng, 7.9677, sr)*0.5+0.5;
        x->_chaacc += 0.00892; if (x->_chaacc>=1.0){ x->_chaacc-=floor(x->_chaacc); x->_chav=al_map_logistic(x->_chav, 3.7109); }
        double cha = x->_chav;
        double env = x->_env.e;
        double fb  = x->_fbprev;
        static const double _seqtab[9] = {0.426,0.216,0.389,0.43,0.601,0.598,0.479,0.422,0.308};
        x->_seqph += (19.972)/sr; if (x->_seqph >= 9.0) x->_seqph -= 9.0;
        int _si = (int)x->_seqph; if(_si<0)_si=0; if(_si>=9)_si=8;
        if (_si != x->_seqi){ x->_seqi=_si; x->_seqenv=1.0; }
        x->_seqenv *= exp(-1000.0/((362.206)*sr));
        double seq = _seqtab[_si];
        double seqenv = x->_seqenv;
        (void)(lfo+shv+cha+env+fb+seq+seqenv);
        double v0=x->_vbus[0], v1=x->_vbus[1], v2=x->_vbus[2], ofb=x->_ofbus; (void)(v0+v1+v2+ofb);
        /* voice 0 source: dsf */
        s = 0.0;
        s = al_dsf(al_phasor_tick(&x->v0_src0,f0,sr), al_clamp((1.9864)+(pn)*(0.3362),(0.5),(6)), al_clamp((0.6437)+(pn)*(0.2203),(0),(0.92)), 16);
        /* voice 0 stage 0: svf */
        s = al_svf_lp(&x->v0_st0, s, al_clamp((80)*pow(2.0,(2.62)+(v1)*(2.6905)),(60),(15000)), al_clamp((0.1536)+(cha)*(0.3897),(0),(0.9)), sr);
        /* voice 0 stage 1: fold */
        s = al_fold(s, al_clamp((1.0147)+(0.0)*(0),(0),(1.3)));
        /* voice 0 stage 2: comb */
        s = al_comb(x->v0_st2_b, 4095, &x->v0_st2_wr, s, al_clamp((681.853)+(env)*(531.3),(20),(2000)), al_clamp((0.3521)+(pn)*(0.3619),(0),(0.9)));
        /* voice 0 stage 3: svf */
        s = al_svf_lp(&x->v0_st3, s, al_clamp((80)*pow(2.0,(1.2753)+(0.0)*(0)),(60),(15000)), al_clamp((0.3587)+(w0)*(0.1585),(0),(0.9)), sr);
        double _vo0 = al_softclip(s);
        /* voice 1 source: cml */
        s = 0.0;
        s = al_cml_tick(&x->v1_src0,f0,sr,al_clamp((3.6534)+(w1)*(0.1511),(3.5),(4)),al_clamp((0.2517)+(pn)*(0.2332),(0),(0.6)));
        /* voice 1 stage 0: ladder */
        s = al_ladder_tick(&x->v1_st0, s, al_clamp((80)*pow(2.0,(5.7226)+(seqenv)*(1.6906)),(60),(14000)), al_clamp((0.2618)+(lfo)*(0.4823),(0),(0.85)), sr);
        /* voice 1 stage 1: cheby */
        s = al_cheby(al_softclip(s*al_clamp((0.9472)+(pn)*(0.667),(0.1),(1.2))), v1_st1_w, 5);
        /* voice 1 stage 2: svf */
        s = al_svf_lp(&x->v1_st2, s, al_clamp((80)*pow(2.0,(5.9497)+(0.0)*(0)),(60),(15000)), al_clamp((0.4543)+(pn)*(0.3569),(0),(0.9)), sr);
        double _vo1 = al_softclip(s);
        s = (_vo0 + _vo1) * 0.5;
        x->_vbus[0] = _vo0;
        x->_vbus[1] = _vo1;
        x->_vbus[2] = 0.0;
        s += al_clamp(ofb * (al_clamp((0.1324)+(seq)*(0.3108),(0),(0.6))), -0.9, 0.9);
        x->_ofbus = al_softclip(s);
        s = al_dc_tick(&x->dc, s);
        if (!(s==s)) s = 0.0;
        al_env_tick(&x->_env, s, 0.43462, 0.04641);
        { double _ov = al_softclip(s); x->_fbprev = fabs(_ov);
           *out++ = (t_sample)(_ov * 0.6); }
    }
    return (w + 6);
}

static void alien_xfreesnap_ep10000_08_dsp(t_alien_xfreesnap_ep10000_08 *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    dsp_add(alien_xfreesnap_ep10000_08_perform, 5, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

static void *alien_xfreesnap_ep10000_08_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_xfreesnap_ep10000_08 *x = (t_alien_xfreesnap_ep10000_08 *)pd_new(alien_xfreesnap_ep10000_08_class);
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    al_rng_seed(&x->rng, 830740080u);
    al_wander_init(&x->wnd, &x->rng, 4);
    al_dc_init(&x->dc);
    al_phasor_init(&x->_lfo); al_sh_init(&x->_sh); al_env_init(&x->_env);
    x->_cha.x=x->_cha.y=x->_cha.z=0.1; x->_chaacc=0.0; x->_chav=0.5; x->_fbprev=0.0;
    x->_vbus[0]=x->_vbus[1]=x->_vbus[2]=0.0; x->_ofbus=0.0;
    x->_seqph=0.0; x->_seqenv=0.0; x->_seqi=-1;
    al_phasor_init(&x->v0_src0);
    al_svf_init(&x->v0_st0);
    memset(x->v0_st2_b,0,sizeof(x->v0_st2_b)); x->v0_st2_wr=0;
    al_svf_init(&x->v0_st3);
    al_cml_init(&x->v1_src0,&x->rng);
    al_ladder_init(&x->v1_st0);
    al_svf_init(&x->v1_st2);
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_xfreesnap_ep10000_08_tilde_setup(void)
{
    alien_xfreesnap_ep10000_08_class = class_new(gensym("alien_xfreesnap_ep10000_08~"),
        (t_newmethod)alien_xfreesnap_ep10000_08_new, 0, sizeof(t_alien_xfreesnap_ep10000_08), CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_xfreesnap_ep10000_08_class, t_alien_xfreesnap_ep10000_08, x_f);
    class_addmethod(alien_xfreesnap_ep10000_08_class, (t_method)alien_xfreesnap_ep10000_08_dsp, gensym("dsp"), A_CANT, 0);
}
