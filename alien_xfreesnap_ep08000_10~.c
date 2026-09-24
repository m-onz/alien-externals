/* alien_xfreesnap_ep08000_10~ -- auto-bred specimen (recipe genome; do not hand-edit)
 * source=bytebeat  stages=['fshift', 'bpf', 'modalbody', 'chorus', 'reson']
 * inlet0 freq Hz | inlet1 param 0..100 (wraps) | outlet0 signal
 */
#include "m_pd.h"
#include "alien.h"
#include <string.h>

static t_class *alien_xfreesnap_ep08000_10_class;
#ifndef BB_SAFE
#define BB_SAFE 1
#define BB_DIV(a,b) ((b)?(a)/(b):0)
#define BB_MOD(a,b) ((b)?(a)%(b):0)
#endif

typedef struct _alien_xfreesnap_ep08000_10 {
    t_object x_obj;
    t_float  x_f;
    double   x_sr;
    al_rng    rng;
    al_wander wnd;
    al_dc     dc;
    al_phasor _lfo; al_sh _sh; al_env _env; al_v3 _cha; double _chaacc, _chav, _fbprev; double _vbus[3]; double _ofbus; double _seqph, _seqenv; int _seqi;
    long v0_src0_t; unsigned v0_src0_rng; double v0_src0_ph,v0_src0_cur,v0_src0_prev;
    al_hilbert v0_st0_h; double v0_st0_op, v0_st0_fb;
    al_svf v0_st1;
    al_modbank v0_st2; double v0_st2_r[6];
    al_chorus v0_st3;
    al_bq v0_st4;
    al_phasor v1_src0;
    al_ladder v1_st0;
} t_alien_xfreesnap_ep08000_10;

static t_int *alien_xfreesnap_ep08000_10_perform(t_int *w)
{
    t_alien_xfreesnap_ep08000_10 *x = (t_alien_xfreesnap_ep08000_10 *)(w[1]);
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
        al_wander_tick(&x->wnd, &x->rng, 4.404, sr);
        double w0=al_wander_val(&x->wnd,0), w1=al_wander_val(&x->wnd,1), w2=al_wander_val(&x->wnd,2), w3=al_wander_val(&x->wnd,3), w4=al_wander_val(&x->wnd,4), w5=al_wander_val(&x->wnd,5), w6=al_wander_val(&x->wnd,6), w7=al_wander_val(&x->wnd,7);
        double lfo = al_sin01(al_phasor_tick(&x->_lfo, 2.2353, sr))*0.5+0.5;
        double shv = al_sh_tick(&x->_sh, &x->rng, 1.88375, sr)*0.5+0.5;
        x->_chaacc += 0.01242; if (x->_chaacc>=1.0){ x->_chaacc-=floor(x->_chaacc); x->_chav=al_map_logistic(x->_chav, 3.9038); }
        double cha = x->_chav;
        double env = x->_env.e;
        double fb  = x->_fbprev;
        static const double _seqtab[8] = {0.402,0.268,0.868,0.729,0.022,0.01,0.751,0.359};
        x->_seqph += (14.877)/sr; if (x->_seqph >= 8.0) x->_seqph -= 8.0;
        int _si = (int)x->_seqph; if(_si<0)_si=0; if(_si>=8)_si=7;
        if (_si != x->_seqi){ x->_seqi=_si; x->_seqenv=1.0; }
        x->_seqenv *= exp(-1000.0/((113.335)*sr));
        double seq = _seqtab[_si];
        double seqenv = x->_seqenv;
        (void)(lfo+shv+cha+env+fb+seq+seqenv);
        double v0=x->_vbus[0], v1=x->_vbus[1], v2=x->_vbus[2], ofb=x->_ofbus; (void)(v0+v1+v2+ofb);
        /* voice 0 source: bytebeat */
        s = 0.0;
        { double _cl = al_clamp(f0*al_clamp((442.214)+(w1)*(223.802),(16),(2048)), 20.0, sr*0.48); x->v0_src0_ph += _cl/sr;
          while (x->v0_src0_ph >= 1.0) { x->v0_src0_ph -= 1.0; x->v0_src0_t++;
            x->v0_src0_rng ^= x->v0_src0_rng<<13; x->v0_src0_rng ^= x->v0_src0_rng>>17; x->v0_src0_rng ^= x->v0_src0_rng<<5;
            { long t=x->v0_src0_t, k=(long)(pn*2.55), r=(long)(x->v0_src0_rng & 0xffffL);
              long _v = (((4)<<((((r)*(r)))&31)));
              x->v0_src0_prev=x->v0_src0_cur; x->v0_src0_cur=((double)((_v & 127L)-64L)/64.0); } }
          s = x->v0_src0_prev + (x->v0_src0_cur - x->v0_src0_prev)*x->v0_src0_ph; }
        /* voice 0 stage 0: fshift */
        { double _I,_Q; al_hilbert_tick(&x->v0_st0_h, s + x->v0_st0_fb*al_clamp((0.2324)+(pn)*(0.1644),(0),(0.85)), &_I,&_Q);
          x->v0_st0_op += (al_clamp((368.564)+(w1)*(323.262),(-1500),(1500)))/sr; x->v0_st0_op -= floor(x->v0_st0_op);
          double _c=cos(AL_2PI*x->v0_st0_op), _sq=sin(AL_2PI*x->v0_st0_op);
          double _ssb=_I*_c - _Q*_sq; x->v0_st0_fb=al_clamp(_ssb,-1.0,1.0);
          s = al_lerp(s, _ssb, al_clamp((0.4273)+(env)*(0.3308),(0.2),(1))); }
        /* voice 0 stage 1: bpf */
        s = al_lerp(s, al_svf_bp(&x->v0_st1, s, al_clamp((80)*pow(2.0,(4.976)+(cha)*(0.4636)),(80),(15000)), al_clamp((0.3891)+(fb)*(0.8541),(0.1),(0.95)), sr), al_clamp((0.9546)+(w6)*(0.7724),(0.2),(1)));
        /* voice 0 stage 2: modalbody */
        { double _b = al_modbank_tick(&x->v0_st2, s, al_clamp(f0*al_clamp((1.9768)+(pn)*(0.3909),(0.25),(4)),20.0,sr*0.45), al_clamp((29.9137)+(env)*(6.2882),(4),(60)), sr) * al_clamp((2.2784)+(v1)*(2.6683),(1),(8));
          s = al_lerp(s, al_softclip(_b), al_clamp((0.6671)+(lfo)*(0.2898),(0.2),(1))); }
        /* voice 0 stage 3: chorus */
        s = al_chorus_tick(&x->v0_st3, s, al_clamp((2.9928)+(w5)*(0.5255),(0),(9)), al_clamp((0.8033)+(env)*(0.5375),(0.05),(2.5)), sr);
        /* voice 0 stage 4: reson */
        s = al_lerp(s, al_reson(&x->v0_st4, s, al_clamp(f0*al_clamp((3.4712)+(pn)*(3.9228),(0.5),(8)),20.0,sr*0.45), al_clamp((17.8985)+(cha)*(5.2929),(2),(45)), sr), al_clamp((0.3996)+(lfo)*(0.2762),(0.2),(1)));
        double _vo0 = al_softclip(s);
        /* voice 1 source: shape */
        s = 0.0;
        { double _p=al_phasor_tick(&x->v1_src0,f0,sr); double _dt=f0/sr;
          s = al_lerp(al_saw_bl(_p,_dt), al_squ_bl(_p,_dt), al_clamp((0.4165)+(v1)*(0.7977),(0),(1)));
          s += al_clamp((0.1168)+(pn)*(0.4637),(0),(0.7))*al_sin01(al_wrap01(_p*0.5)); }
        /* voice 1 stage 0: ladder */
        s = al_ladder_tick(&x->v1_st0, s, al_clamp((80)*pow(2.0,(5.0503)+(0.0)*(0)),(60),(14000)), al_clamp((0.3843)+(pn)*(0.3332),(0),(0.85)), sr);
        double _vo1 = al_softclip(s);
        s = (_vo0 + _vo1) * 0.5;
        x->_vbus[0] = _vo0;
        x->_vbus[1] = _vo1;
        x->_vbus[2] = 0.0;
        s += al_clamp(ofb * (al_clamp((0.3493)+(pn)*(0.2948),(0),(0.6))), -0.9, 0.9);
        x->_ofbus = al_softclip(s);
        s = al_dc_tick(&x->dc, s);
        if (!(s==s)) s = 0.0;
        al_env_tick(&x->_env, s, 0.0367, 0.01587);
        { double _ov = al_softclip(s); x->_fbprev = fabs(_ov);
           *out++ = (t_sample)(_ov * 0.6); }
    }
    return (w + 6);
}

static void alien_xfreesnap_ep08000_10_dsp(t_alien_xfreesnap_ep08000_10 *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    dsp_add(alien_xfreesnap_ep08000_10_perform, 5, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

static void *alien_xfreesnap_ep08000_10_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_xfreesnap_ep08000_10 *x = (t_alien_xfreesnap_ep08000_10 *)pd_new(alien_xfreesnap_ep08000_10_class);
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    al_rng_seed(&x->rng, 137205842u);
    al_wander_init(&x->wnd, &x->rng, 8);
    al_dc_init(&x->dc);
    al_phasor_init(&x->_lfo); al_sh_init(&x->_sh); al_env_init(&x->_env);
    x->_cha.x=x->_cha.y=x->_cha.z=0.1; x->_chaacc=0.0; x->_chav=0.5; x->_fbprev=0.0;
    x->_vbus[0]=x->_vbus[1]=x->_vbus[2]=0.0; x->_ofbus=0.0;
    x->_seqph=0.0; x->_seqenv=0.0; x->_seqi=-1;
    x->v0_src0_t=0; x->v0_src0_rng=2463534242u; x->v0_src0_ph=0.0; x->v0_src0_cur=0.0; x->v0_src0_prev=0.0;
    al_hilbert_init(&x->v0_st0_h); x->v0_st0_op=0.0; x->v0_st0_fb=0.0;
    al_svf_init(&x->v0_st1);
    { static const double _mr[6]={1.0,2.76,5.40,8.93,13.34,18.64}; int _i; for(_i=0;_i<6;_i++) x->v0_st2_r[_i]=_mr[_i]; al_modbank_init(&x->v0_st2,6,x->v0_st2_r); }
    al_chorus_init(&x->v0_st3);
    al_bq_init(&x->v0_st4);
    al_phasor_init(&x->v1_src0);
    al_ladder_init(&x->v1_st0);
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_xfreesnap_ep08000_10_tilde_setup(void)
{
    alien_xfreesnap_ep08000_10_class = class_new(gensym("alien_xfreesnap_ep08000_10~"),
        (t_newmethod)alien_xfreesnap_ep08000_10_new, 0, sizeof(t_alien_xfreesnap_ep08000_10), CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_xfreesnap_ep08000_10_class, t_alien_xfreesnap_ep08000_10, x_f);
    class_addmethod(alien_xfreesnap_ep08000_10_class, (t_method)alien_xfreesnap_ep08000_10_dsp, gensym("dsp"), A_CANT, 0);
}
