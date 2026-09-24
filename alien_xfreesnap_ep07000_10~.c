/* alien_xfreesnap_ep07000_10~ -- auto-bred specimen (recipe genome; do not hand-edit)
 * source=imap  stages=['fshift', 'bpf', 'chorus']
 * inlet0 freq Hz | inlet1 param 0..100 (wraps) | outlet0 signal
 */
#include "m_pd.h"
#include "alien.h"
#include <string.h>

static t_class *alien_xfreesnap_ep07000_10_class;


typedef struct _alien_xfreesnap_ep07000_10 {
    t_object x_obj;
    t_float  x_f;
    double   x_sr;
    al_rng    rng;
    al_wander wnd;
    al_dc     dc;
    al_phasor _lfo; al_sh _sh; al_env _env; al_v3 _cha; double _chaacc, _chav, _fbprev; double _vbus[3]; double _ofbus; double _seqph, _seqenv; int _seqi;
    double v0_src0_x,v0_src0_y,v0_src0_ph,v0_src0_v;
    al_hilbert v0_st0_h; double v0_st0_op, v0_st0_fb;
    al_svf v0_st1;
    al_chorus v0_st2;
} t_alien_xfreesnap_ep07000_10;

static t_int *alien_xfreesnap_ep07000_10_perform(t_int *w)
{
    t_alien_xfreesnap_ep07000_10 *x = (t_alien_xfreesnap_ep07000_10 *)(w[1]);
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
        al_wander_tick(&x->wnd, &x->rng, 0.74, sr);
        double w0=al_wander_val(&x->wnd,0), w1=al_wander_val(&x->wnd,1), w2=al_wander_val(&x->wnd,2), w3=al_wander_val(&x->wnd,3), w4=al_wander_val(&x->wnd,4), w5=al_wander_val(&x->wnd,5);
        double lfo = al_sin01(al_phasor_tick(&x->_lfo, 6.32805, sr))*0.5+0.5;
        double shv = al_sh_tick(&x->_sh, &x->rng, 0.80774, sr)*0.5+0.5;
        x->_chaacc += 0.00128; if (x->_chaacc>=1.0){ x->_chaacc-=floor(x->_chaacc); x->_chav=al_map_logistic(x->_chav, 3.5); }
        double cha = x->_chav;
        double env = x->_env.e;
        double fb  = x->_fbprev;
        static const double _seqtab[6] = {0.594,0.078,0.272,0.456,0.64,0.266};
        x->_seqph += (33.724)/sr; if (x->_seqph >= 6.0) x->_seqph -= 6.0;
        int _si = (int)x->_seqph; if(_si<0)_si=0; if(_si>=6)_si=5;
        if (_si != x->_seqi){ x->_seqi=_si; x->_seqenv=1.0; }
        x->_seqenv *= exp(-1000.0/((688.258)*sr));
        double seq = _seqtab[_si];
        double seqenv = x->_seqenv;
        (void)(lfo+shv+cha+env+fb+seq+seqenv);
        double v0=x->_vbus[0], v1=x->_vbus[1], v2=x->_vbus[2], ofb=x->_ofbus; (void)(v0+v1+v2+ofb);
        /* voice 0 source: imap */
        s = 0.0;
        { x->v0_src0_ph += al_clamp(f0*al_clamp((48.0083)+(cha)*(7.3067),(1.5),(90)),1.0,sr*0.45)/sr;
          while(x->v0_src0_ph>=1.0){ x->v0_src0_ph-=1.0; double _b=al_clamp((0.7929)+(env)*(0.2512),(0),(1));
            x->v0_src0_x=al_map_logistic(x->v0_src0_x,3.4+_b*0.599); x->v0_src0_v=x->v0_src0_x*2.0-1.0;
          }
          s = al_clamp(x->v0_src0_v,-1.0,1.0); }
        /* voice 0 stage 0: fshift */
        { double _I,_Q; al_hilbert_tick(&x->v0_st0_h, s + x->v0_st0_fb*al_clamp((0.8081)+(w5)*(0.207),(0),(0.85)), &_I,&_Q);
          x->v0_st0_op += (al_clamp((-204.805)+(lfo)*(73.814),(-1500),(1500)))/sr; x->v0_st0_op -= floor(x->v0_st0_op);
          double _c=cos(AL_2PI*x->v0_st0_op), _sq=sin(AL_2PI*x->v0_st0_op);
          double _ssb=_I*_c - _Q*_sq; x->v0_st0_fb=al_clamp(_ssb,-1.0,1.0);
          s = al_lerp(s, _ssb, al_clamp((0.4734)+(seqenv)*(0.4866),(0.2),(1))); }
        /* voice 0 stage 1: bpf */
        s = al_lerp(s, al_svf_bp(&x->v0_st1, s, al_clamp((80)*pow(2.0,(3.6812)+(fb)*(1.4906)),(80),(15000)), al_clamp((0.6206)+(pn)*(0.3948),(0.1),(0.95)), sr), al_clamp((0.9888)+(pn)*(0.2361),(0.2),(1)));
        /* voice 0 stage 2: chorus */
        s = al_chorus_tick(&x->v0_st2, s, al_clamp((4.7645)+(0.0)*(0),(0),(9)), al_clamp((0.4341)+(0.0)*(0),(0.05),(2.5)), sr);
        double _vo0 = al_softclip(s);
        s = _vo0;
        x->_vbus[0] = _vo0;
        x->_vbus[1] = 0.0;
        x->_vbus[2] = 0.0;
        s += al_clamp(ofb * (al_clamp((0.1875)+(v1)*(0.3164),(0),(0.6))), -0.9, 0.9);
        x->_ofbus = al_softclip(s);
        s = al_dc_tick(&x->dc, s);
        if (!(s==s)) s = 0.0;
        al_env_tick(&x->_env, s, 0.1441, 0.04831);
        { double _ov = al_softclip(s); x->_fbprev = fabs(_ov);
           *out++ = (t_sample)(_ov * 0.6); }
    }
    return (w + 6);
}

static void alien_xfreesnap_ep07000_10_dsp(t_alien_xfreesnap_ep07000_10 *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    dsp_add(alien_xfreesnap_ep07000_10_perform, 5, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

static void *alien_xfreesnap_ep07000_10_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_xfreesnap_ep07000_10 *x = (t_alien_xfreesnap_ep07000_10 *)pd_new(alien_xfreesnap_ep07000_10_class);
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    al_rng_seed(&x->rng, 159189538u);
    al_wander_init(&x->wnd, &x->rng, 6);
    al_dc_init(&x->dc);
    al_phasor_init(&x->_lfo); al_sh_init(&x->_sh); al_env_init(&x->_env);
    x->_cha.x=x->_cha.y=x->_cha.z=0.1; x->_chaacc=0.0; x->_chav=0.5; x->_fbprev=0.0;
    x->_vbus[0]=x->_vbus[1]=x->_vbus[2]=0.0; x->_ofbus=0.0;
    x->_seqph=0.0; x->_seqenv=0.0; x->_seqi=-1;
    x->v0_src0_x=0.5; x->v0_src0_y=0.1; x->v0_src0_ph=0.0; x->v0_src0_v=0.0;
    al_hilbert_init(&x->v0_st0_h); x->v0_st0_op=0.0; x->v0_st0_fb=0.0;
    al_svf_init(&x->v0_st1);
    al_chorus_init(&x->v0_st2);
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_xfreesnap_ep07000_10_tilde_setup(void)
{
    alien_xfreesnap_ep07000_10_class = class_new(gensym("alien_xfreesnap_ep07000_10~"),
        (t_newmethod)alien_xfreesnap_ep07000_10_new, 0, sizeof(t_alien_xfreesnap_ep07000_10), CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_xfreesnap_ep07000_10_class, t_alien_xfreesnap_ep07000_10, x_f);
    class_addmethod(alien_xfreesnap_ep07000_10_class, (t_method)alien_xfreesnap_ep07000_10_dsp, gensym("dsp"), A_CANT, 0);
}
