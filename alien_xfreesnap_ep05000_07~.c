/* alien_xfreesnap_ep05000_07~ -- auto-bred specimen (recipe genome; do not hand-edit)
 * source=nseq  stages=['comb', 'fold', 'formants']
 * inlet0 freq Hz | inlet1 param 0..100 (wraps) | outlet0 signal
 */
#include "m_pd.h"
#include "alien.h"
#include <string.h>

static t_class *alien_xfreesnap_ep05000_07_class;


typedef struct _alien_xfreesnap_ep05000_07 {
    t_object x_obj;
    t_float  x_f;
    double   x_sr;
    al_rng    rng;
    al_wander wnd;
    al_dc     dc;
    al_phasor _lfo; al_sh _sh; al_env _env; al_v3 _cha; double _chaacc, _chav, _fbprev; double _vbus[3]; double _ofbus; double _seqph, _seqenv; int _seqi;
    al_neuron v0_src0_n; al_modbank v0_src0_b; double v0_src0_r[6];
    double v0_st0_b[4096]; int v0_st0_wr;
    al_formants v0_st2;
    al_phasor v1_src0;
    al_phasor v1_st0;
    al_ladder v1_st1;
} t_alien_xfreesnap_ep05000_07;

static t_int *alien_xfreesnap_ep05000_07_perform(t_int *w)
{
    t_alien_xfreesnap_ep05000_07 *x = (t_alien_xfreesnap_ep05000_07 *)(w[1]);
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
        al_wander_tick(&x->wnd, &x->rng, 0.801, sr);
        double w0=al_wander_val(&x->wnd,0), w1=al_wander_val(&x->wnd,1), w2=al_wander_val(&x->wnd,2), w3=al_wander_val(&x->wnd,3), w4=al_wander_val(&x->wnd,4);
        double lfo = al_sin01(al_phasor_tick(&x->_lfo, 5.823, sr))*0.5+0.5;
        double shv = al_sh_tick(&x->_sh, &x->rng, 14.6441, sr)*0.5+0.5;
        x->_chaacc += 0.01342; if (x->_chaacc>=1.0){ x->_chaacc-=floor(x->_chaacc); x->_chav=al_map_logistic(x->_chav, 4); }
        double cha = x->_chav;
        double env = x->_env.e;
        double fb  = x->_fbprev;
        static const double _seqtab[16] = {0.492,0.501,0.87,0.748,0.749,0.99,0.523,0.373,0.815,0.137,0.515,0.931,0.13,0.923,0.979,0.068};
        x->_seqph += (1.588)/sr; if (x->_seqph >= 16.0) x->_seqph -= 16.0;
        int _si = (int)x->_seqph; if(_si<0)_si=0; if(_si>=16)_si=15;
        if (_si != x->_seqi){ x->_seqi=_si; x->_seqenv=1.0; }
        x->_seqenv *= exp(-1000.0/((52.6087)*sr));
        double seq = _seqtab[_si];
        double seqenv = x->_seqenv;
        (void)(lfo+shv+cha+env+fb+seq+seqenv);
        double v0=x->_vbus[0], v1=x->_vbus[1], v2=x->_vbus[2], ofb=x->_ofbus; (void)(v0+v1+v2+ofb);
        /* voice 0 source: nseq */
        s = 0.0;
        { double _fire = al_neuron_tick(&x->v0_src0_n, al_clamp((0.0044)+(v1)*(0.0663),(0.002),(0.03)), al_clamp((0.0005)+(ofb)*(0.0543),(1e-05),(0.0009)), 0.0);
          s = al_modbank_tick(&x->v0_src0_b, _fire, f0, al_clamp((14.5362)+(seqenv)*(18.5537),(8),(32)), sr) * 14.0; }
        /* voice 0 stage 0: comb */
        s = al_comb(x->v0_st0_b, 4095, &x->v0_st0_wr, s, al_clamp((791.819)+(w4)*(320.784),(20),(2000)), al_clamp((0.3407)+(lfo)*(0.3394),(0),(0.9)));
        /* voice 0 stage 1: fold */
        s = al_fold(s, al_clamp((0.4057)+(w2)*(0.2028),(0),(1.3)));
        /* voice 0 stage 2: formants */
        s = al_lerp(s, al_formants_tick(&x->v0_st2, s, al_clamp((0.2086)+(lfo)*(0.2195),(0),(1)), al_clamp((13.8905)+(ofb)*(7.7984),(2),(22)), sr), al_clamp((0.7454)+(pn)*(0.4858),(0.2),(1)));
        double _vo0 = al_softclip(s);
        /* voice 1 source: dsf */
        s = 0.0;
        s = al_dsf(al_phasor_tick(&x->v1_src0,f0,sr), al_clamp((2.4876)+(fb)*(2.6555),(0.5),(6)), al_clamp((0.6875)+(pn)*(0.2575),(0),(0.92)), 21);
        /* voice 1 stage 0: ring */
        { double _m = al_sin01(al_phasor_tick(&x->v1_st0, al_clamp(f0*al_clamp((1.0766)+(pn)*(6.6492),(0.125),(14)),20.0,sr*0.45), sr));
          s = al_ringmod(s, _m, al_clamp((0.6592)+(pn)*(0.3742),(0),(1))); }
        /* voice 1 stage 1: ladder */
        s = al_ladder_tick(&x->v1_st1, s, al_clamp((80)*pow(2.0,(2.0548)+(shv)*(0.3847)),(60),(14000)), al_clamp((0.4735)+(v1)*(0.1532),(0),(0.85)), sr);
        double _vo1 = al_softclip(s);
        s = al_lerp(_vo0, _vo1, al_clamp((0.6968)+(lfo)*(0.3029),(0),(1)));
        x->_vbus[0] = _vo0;
        x->_vbus[1] = _vo1;
        x->_vbus[2] = 0.0;
        s += al_clamp(ofb * (al_clamp((0.1469)+(w4)*(0.3848),(0),(0.6))), -0.9, 0.9);
        x->_ofbus = al_softclip(s);
        s = al_dc_tick(&x->dc, s);
        if (!(s==s)) s = 0.0;
        al_env_tick(&x->_env, s, 0.2542, 0.03771);
        { double _ov = al_softclip(s); x->_fbprev = fabs(_ov);
           *out++ = (t_sample)(_ov * 0.6); }
    }
    return (w + 6);
}

static void alien_xfreesnap_ep05000_07_dsp(t_alien_xfreesnap_ep05000_07 *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    dsp_add(alien_xfreesnap_ep05000_07_perform, 5, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

static void *alien_xfreesnap_ep05000_07_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_xfreesnap_ep05000_07 *x = (t_alien_xfreesnap_ep05000_07 *)pd_new(alien_xfreesnap_ep05000_07_class);
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    al_rng_seed(&x->rng, 984219936u);
    al_wander_init(&x->wnd, &x->rng, 5);
    al_dc_init(&x->dc);
    al_phasor_init(&x->_lfo); al_sh_init(&x->_sh); al_env_init(&x->_env);
    x->_cha.x=x->_cha.y=x->_cha.z=0.1; x->_chaacc=0.0; x->_chav=0.5; x->_fbprev=0.0;
    x->_vbus[0]=x->_vbus[1]=x->_vbus[2]=0.0; x->_ofbus=0.0;
    x->_seqph=0.0; x->_seqenv=0.0; x->_seqi=-1;
    { static const double _nr[6]={1.0,2.32,4.25,6.63,9.38,12.44}; int _i; for(_i=0;_i<6;_i++) x->v0_src0_r[_i]=_nr[_i];
      al_neuron_init(&x->v0_src0_n); al_modbank_init(&x->v0_src0_b, 6, x->v0_src0_r); }
    memset(x->v0_st0_b,0,sizeof(x->v0_st0_b)); x->v0_st0_wr=0;
    al_formants_init(&x->v0_st2);
    al_phasor_init(&x->v1_src0);
    al_phasor_init(&x->v1_st0);
    al_ladder_init(&x->v1_st1);
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_xfreesnap_ep05000_07_tilde_setup(void)
{
    alien_xfreesnap_ep05000_07_class = class_new(gensym("alien_xfreesnap_ep05000_07~"),
        (t_newmethod)alien_xfreesnap_ep05000_07_new, 0, sizeof(t_alien_xfreesnap_ep05000_07), CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_xfreesnap_ep05000_07_class, t_alien_xfreesnap_ep05000_07, x_f);
    class_addmethod(alien_xfreesnap_ep05000_07_class, (t_method)alien_xfreesnap_ep05000_07_dsp, gensym("dsp"), A_CANT, 0);
}
