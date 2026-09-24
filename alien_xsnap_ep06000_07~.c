/* alien_xsnap_ep06000_07~ -- auto-bred specimen (recipe genome; do not hand-edit)
 * source=nseq  stages=['drive', 'comb', 'drive', 'fold', 'formants']
 * inlet0 freq Hz | inlet1 param 0..100 (wraps) | outlet0 signal
 */
#include "m_pd.h"
#include "alien.h"
#include <string.h>

static t_class *alien_xsnap_ep06000_07_class;


typedef struct _alien_xsnap_ep06000_07 {
    t_object x_obj;
    t_float  x_f;
    double   x_sr;
    al_rng    rng;
    al_wander wnd;
    al_dc     dc;
    al_phasor _lfo; al_sh _sh; al_env _env; al_v3 _cha; double _chaacc, _chav, _fbprev; double _vbus[3]; double _ofbus; double _seqph, _seqenv; int _seqi;
    al_neuron v0_src0_n; al_modbank v0_src0_b; double v0_src0_r[6];
    double v0_st1_b[4096]; int v0_st1_wr;
    al_formants v0_st4;
    al_phasor v1_src0;
    al_hilbert v1_st1_h; double v1_st1_op, v1_st1_fb;
    al_chorus v1_st2;
    al_gendy v2_src0; int v2_src0_i;
    al_phasor v2_st0;
    double v2_st1_b[4096]; int v2_st1_wr;
    al_phasor v2_st3;
    al_fdnrev v2_st4;
} t_alien_xsnap_ep06000_07;

static t_int *alien_xsnap_ep06000_07_perform(t_int *w)
{
    t_alien_xsnap_ep06000_07 *x = (t_alien_xsnap_ep06000_07 *)(w[1]);
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
        al_wander_tick(&x->wnd, &x->rng, 0.683, sr);
        double w0=al_wander_val(&x->wnd,0), w1=al_wander_val(&x->wnd,1), w2=al_wander_val(&x->wnd,2), w3=al_wander_val(&x->wnd,3), w4=al_wander_val(&x->wnd,4);
        double lfo = al_sin01(al_phasor_tick(&x->_lfo, 6.83274, sr))*0.5+0.5;
        double shv = al_sh_tick(&x->_sh, &x->rng, 14.9866, sr)*0.5+0.5;
        x->_chaacc += 0.01342; if (x->_chaacc>=1.0){ x->_chaacc-=floor(x->_chaacc); x->_chav=al_map_logistic(x->_chav, 3.5); }
        double cha = x->_chav;
        double env = x->_env.e;
        double fb  = x->_fbprev;
        static const double _seqtab[16] = {0.492,0.501,0.87,0.748,0.593,0.99,0.265,0.373,0.231,0.102,0.515,0.767,0.523,0.923,0.979,0.068};
        x->_seqph += (0.53)/sr; if (x->_seqph >= 16.0) x->_seqph -= 16.0;
        int _si = (int)x->_seqph; if(_si<0)_si=0; if(_si>=16)_si=15;
        if (_si != x->_seqi){ x->_seqi=_si; x->_seqenv=1.0; }
        x->_seqenv *= exp(-1000.0/((30.4637)*sr));
        double seq = _seqtab[_si];
        double seqenv = x->_seqenv;
        (void)(lfo+shv+cha+env+fb+seq+seqenv);
        double v0=x->_vbus[0], v1=x->_vbus[1], v2=x->_vbus[2], ofb=x->_ofbus; (void)(v0+v1+v2+ofb);
        /* voice 0 source: nseq */
        s = 0.0;
        { double _fire = al_neuron_tick(&x->v0_src0_n, al_clamp((0.0044)+(env)*(0.0663),(0.002),(0.03)), al_clamp((0.0005)+(v1)*(0.0543),(1e-05),(0.0009)), 0.0);
          s = al_modbank_tick(&x->v0_src0_b, _fire, f0, al_clamp((14.5362)+(pn)*(18.5537),(8),(32)), sr) * 14.0; }
        /* voice 0 stage 0: drive */
        s = al_drive(s, al_clamp((0.3238)+(w0)*(0.3371),(0),(0.85)));
        /* voice 0 stage 1: comb */
        s = al_comb(x->v0_st1_b, 4095, &x->v0_st1_wr, s, al_clamp((791.819)+(lfo)*(320.784),(20),(2000)), al_clamp((0.3407)+(lfo)*(0.3394),(0),(0.9)));
        /* voice 0 stage 2: drive */
        s = al_drive(s, al_clamp((0.1075)+(pn)*(0.3022),(0),(0.85)));
        /* voice 0 stage 3: fold */
        s = al_fold(s, al_clamp((0.5948)+(seqenv)*(0.5034),(0),(1.3)));
        /* voice 0 stage 4: formants */
        s = al_lerp(s, al_formants_tick(&x->v0_st4, s, al_clamp((0.2086)+(lfo)*(0.2195),(0),(1)), al_clamp((13.8905)+(pn)*(7.7984),(2),(22)), sr), al_clamp((0.7454)+(pn)*(0.4858),(0.2),(1)));
        double _vo0 = al_softclip(s);
        /* voice 1 source: dsf */
        s = 0.0;
        s = al_dsf(al_phasor_tick(&x->v1_src0,f0,sr), al_clamp((2.4876)+(fb)*(2.6555),(0.5),(6)), al_clamp((0.6875)+(pn)*(0.2575),(0),(0.92)), 21);
        /* voice 1 stage 0: fold */
        s = al_fold(s, al_clamp((0.8339)+(pn)*(0.4299),(0),(1.3)));
        /* voice 1 stage 1: fshift */
        { double _I,_Q; al_hilbert_tick(&x->v1_st1_h, s + x->v1_st1_fb*al_clamp((0.0516)+(env)*(0.2725),(0),(0.85)), &_I,&_Q);
          x->v1_st1_op += (al_clamp((274.17)+(w0)*(579.543),(-1500),(1500)))/sr; x->v1_st1_op -= floor(x->v1_st1_op);
          double _c=cos(AL_2PI*x->v1_st1_op), _sq=sin(AL_2PI*x->v1_st1_op);
          double _ssb=_I*_c - _Q*_sq; x->v1_st1_fb=al_clamp(_ssb,-1.0,1.0);
          s = al_lerp(s, _ssb, al_clamp((0.4736)+(v0)*(0.2007),(0.2),(1))); }
        /* voice 1 stage 2: chorus */
        s = al_chorus_tick(&x->v1_st2, s, al_clamp((4.8283)+(0.0)*(0),(0),(9)), al_clamp((1.1101)+(shv)*(0.7661),(0.05),(2.5)), sr);
        double _vo1 = al_softclip(s);
        /* voice 2 source: gendy */
        s = 0.0;
        if(!x->v2_src0_i){ x->v2_src0_i=1; al_gendy_init(&x->v2_src0,&x->rng,14); }
        s = al_gendy_tick(&x->v2_src0,&x->rng,f0,sr,al_clamp((0.0861)+(w4)*(0.3744),(0),(0.6)),al_clamp((0.0712)+(shv)*(0.1702),(0),(0.3)));
        /* voice 2 stage 0: ring */
        { double _m = al_sin01(al_phasor_tick(&x->v2_st0, al_clamp(f0*al_clamp((0.8128)+(seqenv)*(1.2207),(0.125),(14)),20.0,sr*0.45), sr));
          s = al_ringmod(s, _m, al_clamp((0.703)+(0.0)*(0),(0),(1))); }
        /* voice 2 stage 1: comb */
        s = al_comb(x->v2_st1_b, 4095, &x->v2_st1_wr, s, al_clamp((206.275)+(0.0)*(0),(20),(2000)), al_clamp((0.5114)+(v2)*(0.199),(0),(0.9)));
        /* voice 2 stage 2: drive */
        s = al_drive(s, al_clamp((0.3525)+(pn)*(0.5713),(0),(0.85)));
        /* voice 2 stage 3: pmshape */
        { double _c = al_phasor_tick(&x->v2_st3, al_clamp(f0*al_clamp((1.3293)+(v2)*(3.5925),(0.125),(8)),20.0,sr*0.45), sr);
          double _y = sin(AL_2PI*(_c + al_clamp((0.3828)+(pn)*(3.8067),(0),(5))*s));
          s = al_lerp(s, _y, al_clamp((0.8726)+(w3)*(0.3614),(0),(1))); }
        /* voice 2 stage 4: fdn */
        s = al_lerp(s, al_fdnrev_tick(&x->v2_st4, s, al_clamp((0.8368)+(w3)*(0.4698),(0.1),(1)), al_clamp((0.4476)+(fb)*(0.3856),(0),(0.8)), al_clamp((0.3435)+(seq)*(0.2783),(0),(0.85))), al_clamp((0.3285)+(env)*(0.1823),(0.1),(0.6)));
        double _vo2 = al_softclip(s);
        s = (_vo0 + _vo1 + _vo2) * 0.333333;
        x->_vbus[0] = _vo0;
        x->_vbus[1] = _vo1;
        x->_vbus[2] = _vo2;
        s += al_clamp(ofb * (al_clamp((0.1581)+(lfo)*(0.3729),(0),(0.6))), -0.9, 0.9);
        x->_ofbus = al_softclip(s);
        s = al_dc_tick(&x->dc, s);
        if (!(s==s)) s = 0.0;
        al_env_tick(&x->_env, s, 0.2542, 0.03771);
        { double _ov = al_softclip(s); x->_fbprev = fabs(_ov);
           *out++ = (t_sample)(_ov * 0.6); }
    }
    return (w + 6);
}

static void alien_xsnap_ep06000_07_dsp(t_alien_xsnap_ep06000_07 *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    dsp_add(alien_xsnap_ep06000_07_perform, 5, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

static void *alien_xsnap_ep06000_07_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_xsnap_ep06000_07 *x = (t_alien_xsnap_ep06000_07 *)pd_new(alien_xsnap_ep06000_07_class);
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    al_rng_seed(&x->rng, 997186805u);
    al_wander_init(&x->wnd, &x->rng, 5);
    al_dc_init(&x->dc);
    al_phasor_init(&x->_lfo); al_sh_init(&x->_sh); al_env_init(&x->_env);
    x->_cha.x=x->_cha.y=x->_cha.z=0.1; x->_chaacc=0.0; x->_chav=0.5; x->_fbprev=0.0;
    x->_vbus[0]=x->_vbus[1]=x->_vbus[2]=0.0; x->_ofbus=0.0;
    x->_seqph=0.0; x->_seqenv=0.0; x->_seqi=-1;
    { static const double _nr[6]={1.0,2.32,4.25,6.63,9.38,12.44}; int _i; for(_i=0;_i<6;_i++) x->v0_src0_r[_i]=_nr[_i];
      al_neuron_init(&x->v0_src0_n); al_modbank_init(&x->v0_src0_b, 6, x->v0_src0_r); }
    memset(x->v0_st1_b,0,sizeof(x->v0_st1_b)); x->v0_st1_wr=0;
    al_formants_init(&x->v0_st4);
    al_phasor_init(&x->v1_src0);
    al_hilbert_init(&x->v1_st1_h); x->v1_st1_op=0.0; x->v1_st1_fb=0.0;
    al_chorus_init(&x->v1_st2);
    x->v2_src0_i=0;
    al_phasor_init(&x->v2_st0);
    memset(x->v2_st1_b,0,sizeof(x->v2_st1_b)); x->v2_st1_wr=0;
    al_phasor_init(&x->v2_st3);
    al_fdnrev_init(&x->v2_st4);
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_xsnap_ep06000_07_tilde_setup(void)
{
    alien_xsnap_ep06000_07_class = class_new(gensym("alien_xsnap_ep06000_07~"),
        (t_newmethod)alien_xsnap_ep06000_07_new, 0, sizeof(t_alien_xsnap_ep06000_07), CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_xsnap_ep06000_07_class, t_alien_xsnap_ep06000_07, x_f);
    class_addmethod(alien_xsnap_ep06000_07_class, (t_method)alien_xsnap_ep06000_07_dsp, gensym("dsp"), A_CANT, 0);
}
