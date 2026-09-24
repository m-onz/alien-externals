/* alien_xfreesnap_ep10000_09~ -- auto-bred specimen (recipe genome; do not hand-edit)
 * source=undertone  stages=['bitcrush', 'chorus', 'bpf', 'chorus']
 * inlet0 freq Hz | inlet1 param 0..100 (wraps) | outlet0 signal
 */
#include "m_pd.h"
#include "alien.h"
#include <string.h>

static t_class *alien_xfreesnap_ep10000_09_class;


typedef struct _alien_xfreesnap_ep10000_09 {
    t_object x_obj;
    t_float  x_f;
    double   x_sr;
    al_rng    rng;
    al_wander wnd;
    al_dc     dc;
    al_phasor _lfo; al_sh _sh; al_env _env; al_v3 _cha; double _chaacc, _chav, _fbprev; double _vbus[3]; double _ofbus; double _seqph, _seqenv; int _seqi;
    double v0_src0_up[8];
    al_chorus v0_st1;
    al_svf v0_st2;
    al_chorus v0_st3;
    double v1_src0_ph[4]; double v1_src0_prev[4];
    al_svf v1_st0;
    double v1_st1_b[4096]; int v1_st1_wr;
    al_ladder v1_st2;
} t_alien_xfreesnap_ep10000_09;

static t_int *alien_xfreesnap_ep10000_09_perform(t_int *w)
{
    t_alien_xfreesnap_ep10000_09 *x = (t_alien_xfreesnap_ep10000_09 *)(w[1]);
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
        al_wander_tick(&x->wnd, &x->rng, 1.489, sr);
        double w0=al_wander_val(&x->wnd,0), w1=al_wander_val(&x->wnd,1), w2=al_wander_val(&x->wnd,2), w3=al_wander_val(&x->wnd,3), w4=al_wander_val(&x->wnd,4), w5=al_wander_val(&x->wnd,5), w6=al_wander_val(&x->wnd,6), w7=al_wander_val(&x->wnd,7);
        double lfo = al_sin01(al_phasor_tick(&x->_lfo, 6.61451, sr))*0.5+0.5;
        double shv = al_sh_tick(&x->_sh, &x->rng, 7.9006, sr)*0.5+0.5;
        x->_chaacc += 0.01327; if (x->_chaacc>=1.0){ x->_chaacc-=floor(x->_chaacc); x->_chav=al_map_logistic(x->_chav, 3.5); }
        double cha = x->_chav;
        double env = x->_env.e;
        double fb  = x->_fbprev;
        static const double _seqtab[9] = {0.254,0.688,0.407,0.686,0.999,0.613,0.347,0.506,0.377};
        x->_seqph += (10.882)/sr; if (x->_seqph >= 9.0) x->_seqph -= 9.0;
        int _si = (int)x->_seqph; if(_si<0)_si=0; if(_si>=9)_si=8;
        if (_si != x->_seqi){ x->_seqi=_si; x->_seqenv=1.0; }
        x->_seqenv *= exp(-1000.0/((506.295)*sr));
        double seq = _seqtab[_si];
        double seqenv = x->_seqenv;
        (void)(lfo+shv+cha+env+fb+seq+seqenv);
        double v0=x->_vbus[0], v1=x->_vbus[1], v2=x->_vbus[2], ofb=x->_ofbus; (void)(v0+v1+v2+ofb);
        /* voice 0 source: undertone */
        s = 0.0;
        { double _acc=0.0,_wsum=0.0; int _i; double _tl=al_clamp((1.1172)+(cha)*(0.9062),(0),(2.5));
          for(_i=0;_i<6;_i++){ double _f=f0/(_i+1.0); x->v0_src0_up[_i]+=_f/sr;
            if(x->v0_src0_up[_i]>=1.0)x->v0_src0_up[_i]-=1.0; double _wt=pow(_i+1.0,-_tl);
            _acc+=_wt*al_sin01(x->v0_src0_up[_i]); _wsum+=_wt; }
          s = (_wsum>0.0)? _acc/_wsum : 0.0; }
        /* voice 0 stage 0: bitcrush */
        s = al_bitcrush(s, al_clamp((9.456)+(w1)*(1.3678),(3),(16)));
        /* voice 0 stage 1: chorus */
        s = al_chorus_tick(&x->v0_st1, s, al_clamp((4.0226)+(pn)*(0.2512),(0),(9)), al_clamp((0.6449)+(fb)*(0.5167),(0.05),(2.5)), sr);
        /* voice 0 stage 2: bpf */
        s = al_lerp(s, al_svf_bp(&x->v0_st2, s, al_clamp((80)*pow(2.0,(2.3892)+(w2)*(0.5978)),(80),(15000)), al_clamp((0.6504)+(seq)*(0.4294),(0.1),(0.95)), sr), al_clamp((0.7879)+(fb)*(0.3533),(0.2),(1)));
        /* voice 0 stage 3: chorus */
        s = al_chorus_tick(&x->v0_st3, s, al_clamp((5.7664)+(v1)*(0.696),(0),(9)), al_clamp((1.115)+(w7)*(0.8708),(0.05),(2.5)), sr);
        double _vo0 = al_softclip(s);
        /* voice 1 source: fmnet */
        s = 0.0;
        { double _op[4];
          x->v1_src0_ph[0] += al_clamp(0.25*f0,20.0,sr*0.45)/sr; if(x->v1_src0_ph[0]>=1.0) x->v1_src0_ph[0]-=1.0;
          _op[0] = sin(AL_2PI*(x->v1_src0_ph[0] + (0.64)*(-0.46*x->v1_src0_prev[0] + -0.06*x->v1_src0_prev[2] + -0.88*x->v1_src0_prev[3])));
          x->v1_src0_ph[1] += al_clamp(0.25*f0,20.0,sr*0.45)/sr; if(x->v1_src0_ph[1]>=1.0) x->v1_src0_ph[1]-=1.0;
          _op[1] = sin(AL_2PI*(x->v1_src0_ph[1] + (2.04)*(-1*x->v1_src0_prev[1] + 0.8*x->v1_src0_prev[3])));
          x->v1_src0_ph[2] += al_clamp(4*f0,20.0,sr*0.45)/sr; if(x->v1_src0_ph[2]>=1.0) x->v1_src0_ph[2]-=1.0;
          _op[2] = sin(AL_2PI*(x->v1_src0_ph[2] + (0.87)*(-0.87*x->v1_src0_prev[2])));
          x->v1_src0_ph[3] += al_clamp(3.5*f0,20.0,sr*0.45)/sr; if(x->v1_src0_ph[3]>=1.0) x->v1_src0_ph[3]-=1.0;
          _op[3] = sin(AL_2PI*(x->v1_src0_ph[3] + (2.9)*(-0.19*x->v1_src0_prev[0] + 0.34*x->v1_src0_prev[1])));
          x->v1_src0_prev[0] = _op[0];
          x->v1_src0_prev[1] = _op[1];
          x->v1_src0_prev[2] = _op[2];
          x->v1_src0_prev[3] = _op[3];
          s = (0.22*_op[0] + 0.64*_op[1] + 0.41*_op[2] + 0.39*_op[3]) * 0.602409; }
        /* voice 1 stage 0: multiband */
        { double _lo = al_svf_lp(&x->v1_st0, s, al_clamp((120)*pow(2.0,(6.1733)+(shv)*(1.7351)),(200),(6000)), 0.3, sr); double _hi = s - _lo;
          _lo = al_drive(_lo, al_clamp((0.3353)+(v0)*(0.3896),(0),(0.9)))*al_clamp((0.5979)+(lfo)*(0.481),(0.2),(1.6)); _hi = al_fold(_hi, al_clamp((0.7739)+(pn)*(0.5112),(0),(1.2)))*al_clamp((0.9265)+(pn)*(0.5104),(0.2),(1.5));
          s = al_softclip(_lo + _hi); }
        /* voice 1 stage 1: comb */
        s = al_comb(x->v1_st1_b, 4095, &x->v1_st1_wr, s, al_clamp((976.794)+(w2)*(119.003),(20),(2000)), al_clamp((0.3667)+(v1)*(0.3989),(0),(0.9)));
        /* voice 1 stage 2: ladder */
        s = al_ladder_tick(&x->v1_st2, s, al_clamp((80)*pow(2.0,(4.7907)+(seqenv)*(0.7698)),(60),(14000)), al_clamp((0.5818)+(w6)*(0.4726),(0),(0.85)), sr);
        double _vo1 = al_softclip(s);
        s = (_vo0 + _vo1) * 0.5;
        x->_vbus[0] = _vo0;
        x->_vbus[1] = _vo1;
        x->_vbus[2] = 0.0;
        s += al_clamp(ofb * (al_clamp((0.1915)+(w6)*(0.1903),(0),(0.6))), -0.9, 0.9);
        x->_ofbus = al_softclip(s);
        s = al_dc_tick(&x->dc, s);
        if (!(s==s)) s = 0.0;
        al_env_tick(&x->_env, s, 0.1192, 0.03604);
        { double _ov = al_softclip(s); x->_fbprev = fabs(_ov);
           *out++ = (t_sample)(_ov * 0.6); }
    }
    return (w + 6);
}

static void alien_xfreesnap_ep10000_09_dsp(t_alien_xfreesnap_ep10000_09 *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    dsp_add(alien_xfreesnap_ep10000_09_perform, 5, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

static void *alien_xfreesnap_ep10000_09_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_xfreesnap_ep10000_09 *x = (t_alien_xfreesnap_ep10000_09 *)pd_new(alien_xfreesnap_ep10000_09_class);
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    al_rng_seed(&x->rng, 1044227080u);
    al_wander_init(&x->wnd, &x->rng, 8);
    al_dc_init(&x->dc);
    al_phasor_init(&x->_lfo); al_sh_init(&x->_sh); al_env_init(&x->_env);
    x->_cha.x=x->_cha.y=x->_cha.z=0.1; x->_chaacc=0.0; x->_chav=0.5; x->_fbprev=0.0;
    x->_vbus[0]=x->_vbus[1]=x->_vbus[2]=0.0; x->_ofbus=0.0;
    x->_seqph=0.0; x->_seqenv=0.0; x->_seqi=-1;
    {int _i; for(_i=0;_i<8;_i++) x->v0_src0_up[_i]=0.0;}
    al_chorus_init(&x->v0_st1);
    al_svf_init(&x->v0_st2);
    al_chorus_init(&x->v0_st3);
    {int _i; for(_i=0;_i<4;_i++){x->v1_src0_ph[_i]=0.0; x->v1_src0_prev[_i]=0.0;}}
    al_svf_init(&x->v1_st0);
    memset(x->v1_st1_b,0,sizeof(x->v1_st1_b)); x->v1_st1_wr=0;
    al_ladder_init(&x->v1_st2);
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_xfreesnap_ep10000_09_tilde_setup(void)
{
    alien_xfreesnap_ep10000_09_class = class_new(gensym("alien_xfreesnap_ep10000_09~"),
        (t_newmethod)alien_xfreesnap_ep10000_09_new, 0, sizeof(t_alien_xfreesnap_ep10000_09), CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_xfreesnap_ep10000_09_class, t_alien_xfreesnap_ep10000_09, x_f);
    class_addmethod(alien_xfreesnap_ep10000_09_class, (t_method)alien_xfreesnap_ep10000_09_dsp, gensym("dsp"), A_CANT, 0);
}
