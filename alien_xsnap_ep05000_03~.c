/* alien_xsnap_ep05000_03~ -- auto-bred specimen (recipe genome; do not hand-edit)
 * source=fmnet  stages=['comb', 'drive', 'formants', 'modalbody']
 * inlet0 freq Hz | inlet1 param 0..100 (wraps) | outlet0 signal
 */
#include "m_pd.h"
#include "alien.h"
#include <string.h>

static t_class *alien_xsnap_ep05000_03_class;


typedef struct _alien_xsnap_ep05000_03 {
    t_object x_obj;
    t_float  x_f;
    double   x_sr;
    al_rng    rng;
    al_wander wnd;
    al_dc     dc;
    al_phasor _lfo; al_sh _sh; al_env _env; al_v3 _cha; double _chaacc, _chav, _fbprev; double _vbus[3]; double _ofbus; double _seqph, _seqenv; int _seqi;
    double v0_src0_ph[4]; double v0_src0_prev[4];
    double v0_st0_b[4096]; int v0_st0_wr;
    al_formants v0_st2;
    al_modbank v0_st3; double v0_st3_r[6];
    al_modbank v1_src0; double v1_src0_r[6]; al_phasor v1_src0_x; double v1_src0_pv;
    al_ladder v1_st1;
} t_alien_xsnap_ep05000_03;

static t_int *alien_xsnap_ep05000_03_perform(t_int *w)
{
    t_alien_xsnap_ep05000_03 *x = (t_alien_xsnap_ep05000_03 *)(w[1]);
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
        al_wander_tick(&x->wnd, &x->rng, 1.787, sr);
        double w0=al_wander_val(&x->wnd,0), w1=al_wander_val(&x->wnd,1), w2=al_wander_val(&x->wnd,2), w3=al_wander_val(&x->wnd,3), w4=al_wander_val(&x->wnd,4);
        double lfo = al_sin01(al_phasor_tick(&x->_lfo, 5.823, sr))*0.5+0.5;
        double shv = al_sh_tick(&x->_sh, &x->rng, 21.5, sr)*0.5+0.5;
        x->_chaacc += 0.01342; if (x->_chaacc>=1.0){ x->_chaacc-=floor(x->_chaacc); x->_chav=al_map_logistic(x->_chav, 3.7148); }
        double cha = x->_chav;
        double env = x->_env.e;
        double fb  = x->_fbprev;
        static const double _seqtab[15] = {0.87,0.164,0.749,0.99,0.265,0.373,0.762,0.102,0.515,0.511,0.706,0.923,0.979,0.623,0.492};
        x->_seqph += (5.59348)/sr; if (x->_seqph >= 15.0) x->_seqph -= 15.0;
        int _si = (int)x->_seqph; if(_si<0)_si=0; if(_si>=15)_si=14;
        if (_si != x->_seqi){ x->_seqi=_si; x->_seqenv=1.0; }
        x->_seqenv *= exp(-1000.0/((33.262)*sr));
        double seq = _seqtab[_si];
        double seqenv = x->_seqenv;
        (void)(lfo+shv+cha+env+fb+seq+seqenv);
        double v0=x->_vbus[0], v1=x->_vbus[1], v2=x->_vbus[2], ofb=x->_ofbus; (void)(v0+v1+v2+ofb);
        /* voice 0 source: fmnet */
        s = 0.0;
        { double _op[3];
          x->v0_src0_ph[0] += al_clamp(7*f0,20.0,sr*0.45)/sr; if(x->v0_src0_ph[0]>=1.0) x->v0_src0_ph[0]-=1.0;
          _op[0] = sin(AL_2PI*(x->v0_src0_ph[0] + (0.49)*(0.11*x->v0_src0_prev[0])));
          x->v0_src0_ph[1] += al_clamp(5*f0,20.0,sr*0.45)/sr; if(x->v0_src0_ph[1]>=1.0) x->v0_src0_ph[1]-=1.0;
          _op[1] = sin(AL_2PI*(x->v0_src0_ph[1] + (1.61)*(-0.54*x->v0_src0_prev[0] + -0.96*x->v0_src0_prev[1])));
          x->v0_src0_ph[2] += al_clamp(2*f0,20.0,sr*0.45)/sr; if(x->v0_src0_ph[2]>=1.0) x->v0_src0_ph[2]-=1.0;
          _op[2] = sin(AL_2PI*(x->v0_src0_ph[2] + (1.19)*(0.84*x->v0_src0_prev[1])));
          x->v0_src0_prev[0] = _op[0];
          x->v0_src0_prev[1] = _op[1];
          x->v0_src0_prev[2] = _op[2];
          s = (0.32*_op[0] + 0.42*_op[1] + 0.72*_op[2]) * 0.684931; }
        /* voice 0 stage 0: comb */
        s = al_comb(x->v0_st0_b, 4095, &x->v0_st0_wr, s, al_clamp((835.383)+(v2)*(320.784),(20),(2000)), al_clamp((0.3407)+(lfo)*(0.3394),(0),(0.9)));
        /* voice 0 stage 1: drive */
        s = al_drive(s, al_clamp((0.1075)+(pn)*(0.3022),(0),(0.85)));
        /* voice 0 stage 2: formants */
        s = al_lerp(s, al_formants_tick(&x->v0_st2, s, al_clamp((0.22)+(lfo)*(0.2195),(0),(1)), al_clamp((13.8905)+(pn)*(7.7984),(2),(22)), sr), al_clamp((0.7454)+(w4)*(0.4858),(0.2),(1)));
        /* voice 0 stage 3: modalbody */
        { double _b = al_modbank_tick(&x->v0_st3, s, al_clamp(f0*al_clamp((1.135)+(w3)*(0.975),(0.25),(4)),20.0,sr*0.45), al_clamp((32.8317)+(seqenv)*(2.7286),(4),(60)), sr) * al_clamp((3.8322)+(w0)*(1.3351),(1),(8));
          s = al_lerp(s, al_softclip(_b), al_clamp((0.7737)+(pn)*(0.4419),(0.2),(1))); }
        double _vo0 = al_softclip(s);
        /* voice 1 source: modal */
        s = 0.0;
        { double _ph=al_phasor_tick(&x->v1_src0_x, f0*0.25, sr);
          double _exc=(_ph < x->v1_src0_pv)?1.0:0.0; x->v1_src0_pv=_ph;
          s = al_modbank_tick(&x->v1_src0, _exc, f0, al_clamp((22.4258)+(pn)*(18.8863),(10),(70)), sr) * 1.5;
          s += al_clamp((0.4993)+(v1)*(0.2817),(0.12),(0.5))*al_sin01(al_wrap01(_ph)); }
        /* voice 1 stage 0: drive */
        s = al_drive(s, al_clamp((0.405)+(pn)*(0.4847),(0),(0.85)));
        /* voice 1 stage 1: ladder */
        s = al_ladder_tick(&x->v1_st1, s, al_clamp((80)*pow(2.0,(3.7877)+(w4)*(3.0555)),(60),(14000)), al_clamp((0.5083)+(pn)*(0.3029),(0),(0.85)), sr);
        double _vo1 = al_softclip(s);
        s = al_lerp(_vo0, _vo1, al_clamp((0.6181)+(lfo)*(0.3677),(0),(1)));
        x->_vbus[0] = _vo0;
        x->_vbus[1] = _vo1;
        x->_vbus[2] = 0.0;
        s += al_clamp(ofb * (al_clamp((0.2601)+(seq)*(0.3848),(0),(0.6))), -0.9, 0.9);
        x->_ofbus = al_softclip(s);
        s = al_dc_tick(&x->dc, s);
        if (!(s==s)) s = 0.0;
        al_env_tick(&x->_env, s, 0.2542, 0.05168);
        { double _ov = al_softclip(s); x->_fbprev = fabs(_ov);
           *out++ = (t_sample)(_ov * 0.6); }
    }
    return (w + 6);
}

static void alien_xsnap_ep05000_03_dsp(t_alien_xsnap_ep05000_03 *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    dsp_add(alien_xsnap_ep05000_03_perform, 5, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

static void *alien_xsnap_ep05000_03_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_xsnap_ep05000_03 *x = (t_alien_xsnap_ep05000_03 *)pd_new(alien_xsnap_ep05000_03_class);
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    al_rng_seed(&x->rng, 767352494u);
    al_wander_init(&x->wnd, &x->rng, 5);
    al_dc_init(&x->dc);
    al_phasor_init(&x->_lfo); al_sh_init(&x->_sh); al_env_init(&x->_env);
    x->_cha.x=x->_cha.y=x->_cha.z=0.1; x->_chaacc=0.0; x->_chav=0.5; x->_fbprev=0.0;
    x->_vbus[0]=x->_vbus[1]=x->_vbus[2]=0.0; x->_ofbus=0.0;
    x->_seqph=0.0; x->_seqenv=0.0; x->_seqi=-1;
    {int _i; for(_i=0;_i<4;_i++){x->v0_src0_ph[_i]=0.0; x->v0_src0_prev[_i]=0.0;}}
    memset(x->v0_st0_b,0,sizeof(x->v0_st0_b)); x->v0_st0_wr=0;
    al_formants_init(&x->v0_st2);
    { static const double _mr[6]={1.0,2.76,5.40,8.93,13.34,18.64}; int _i; for(_i=0;_i<6;_i++) x->v0_st3_r[_i]=_mr[_i]; al_modbank_init(&x->v0_st3,6,x->v0_st3_r); }
    { static const double _mr[6]={1.0,2.76,5.40,8.93,13.34,18.64}; int _i; for(_i=0;_i<6;_i++) x->v1_src0_r[_i]=_mr[_i];
      al_modbank_init(&x->v1_src0, 6, x->v1_src0_r); al_phasor_init(&x->v1_src0_x); x->v1_src0_pv=0.0; }
    al_ladder_init(&x->v1_st1);
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_xsnap_ep05000_03_tilde_setup(void)
{
    alien_xsnap_ep05000_03_class = class_new(gensym("alien_xsnap_ep05000_03~"),
        (t_newmethod)alien_xsnap_ep05000_03_new, 0, sizeof(t_alien_xsnap_ep05000_03), CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_xsnap_ep05000_03_class, t_alien_xsnap_ep05000_03, x_f);
    class_addmethod(alien_xsnap_ep05000_03_class, (t_method)alien_xsnap_ep05000_03_dsp, gensym("dsp"), A_CANT, 0);
}
