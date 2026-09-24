/* alien_xsnap_ep05000_05~ -- auto-bred specimen (recipe genome; do not hand-edit)
 * source=bytebeat  stages=['modalbody', 'ring', 'bitcrush', 'chorus', 'chorus']
 * inlet0 freq Hz | inlet1 param 0..100 (wraps) | outlet0 signal
 */
#include "m_pd.h"
#include "alien.h"
#include <string.h>

static t_class *alien_xsnap_ep05000_05_class;
#ifndef BB_SAFE
#define BB_SAFE 1
#define BB_DIV(a,b) ((b)?(a)/(b):0)
#define BB_MOD(a,b) ((b)?(a)%(b):0)
#endif

typedef struct _alien_xsnap_ep05000_05 {
    t_object x_obj;
    t_float  x_f;
    double   x_sr;
    al_rng    rng;
    al_wander wnd;
    al_dc     dc;
    al_phasor _lfo; al_sh _sh; al_env _env; al_v3 _cha; double _chaacc, _chav, _fbprev; double _vbus[3]; double _ofbus; double _seqph, _seqenv; int _seqi;
    long v0_src0_t; unsigned v0_src0_rng; double v0_src0_ph,v0_src0_cur,v0_src0_prev;
    al_modbank v0_st0; double v0_st0_r[6];
    al_phasor v0_st1;
    al_chorus v0_st3;
    al_chorus v0_st4;
} t_alien_xsnap_ep05000_05;

static t_int *alien_xsnap_ep05000_05_perform(t_int *w)
{
    t_alien_xsnap_ep05000_05 *x = (t_alien_xsnap_ep05000_05 *)(w[1]);
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
        al_wander_tick(&x->wnd, &x->rng, 1.431, sr);
        double w0=al_wander_val(&x->wnd,0), w1=al_wander_val(&x->wnd,1), w2=al_wander_val(&x->wnd,2), w3=al_wander_val(&x->wnd,3), w4=al_wander_val(&x->wnd,4), w5=al_wander_val(&x->wnd,5), w6=al_wander_val(&x->wnd,6), w7=al_wander_val(&x->wnd,7);
        double lfo = al_sin01(al_phasor_tick(&x->_lfo, 2.33196, sr))*0.5+0.5;
        double shv = al_sh_tick(&x->_sh, &x->rng, 5.39194, sr)*0.5+0.5;
        x->_chaacc += 0.01961; if (x->_chaacc>=1.0){ x->_chaacc-=floor(x->_chaacc); x->_chav=al_map_logistic(x->_chav, 3.7661); }
        double cha = x->_chav;
        double env = x->_env.e;
        double fb  = x->_fbprev;
        static const double _seqtab[15] = {0.221,0.133,0.113,0.766,0.487,0.153,0.812,0.35,0.681,0.715,0.204,0.067,0.571,0.641,0.855};
        x->_seqph += (4.786)/sr; if (x->_seqph >= 15.0) x->_seqph -= 15.0;
        int _si = (int)x->_seqph; if(_si<0)_si=0; if(_si>=15)_si=14;
        if (_si != x->_seqi){ x->_seqi=_si; x->_seqenv=1.0; }
        x->_seqenv *= exp(-1000.0/((587.571)*sr));
        double seq = _seqtab[_si];
        double seqenv = x->_seqenv;
        (void)(lfo+shv+cha+env+fb+seq+seqenv);
        double v0=x->_vbus[0], v1=x->_vbus[1], v2=x->_vbus[2], ofb=x->_ofbus; (void)(v0+v1+v2+ofb);
        /* voice 0 source: bytebeat */
        s = 0.0;
        { double _cl = al_clamp(f0*al_clamp((325.721)+(0.0)*(0),(16),(2048)), 20.0, sr*0.48); x->v0_src0_ph += _cl/sr;
          while (x->v0_src0_ph >= 1.0) { x->v0_src0_ph -= 1.0; x->v0_src0_t++;
            x->v0_src0_rng ^= x->v0_src0_rng<<13; x->v0_src0_rng ^= x->v0_src0_rng>>17; x->v0_src0_rng ^= x->v0_src0_rng<<5;
            { long t=x->v0_src0_t, k=(long)(pn*2.55), r=(long)(x->v0_src0_rng & 0xffffL);
              long _v = (((((((((t)>>((10)&31)))*(255)))+((~(((t)^(t)))))))-(t)));
              x->v0_src0_prev=x->v0_src0_cur; x->v0_src0_cur=((double)((_v & 4095L)-2048L)/2048.0); } }
          s = x->v0_src0_prev + (x->v0_src0_cur - x->v0_src0_prev)*x->v0_src0_ph; }
        /* voice 0 stage 0: modalbody */
        { double _b = al_modbank_tick(&x->v0_st0, s, al_clamp(f0*al_clamp((1.3034)+(w6)*(1.8091),(0.25),(4)),20.0,sr*0.45), al_clamp((31.0202)+(env)*(0.8511),(4),(60)), sr) * al_clamp((3.7587)+(fb)*(0.7199),(1),(8));
          s = al_lerp(s, al_softclip(_b), al_clamp((0.2749)+(v1)*(0.2709),(0.2),(1))); }
        /* voice 0 stage 1: ring */
        { double _m = al_sin01(al_phasor_tick(&x->v0_st1, al_clamp(f0*al_clamp((1.3673)+(lfo)*(2.2536),(0.125),(14)),20.0,sr*0.45), sr));
          s = al_ringmod(s, _m, al_clamp((0.5239)+(w0)*(0.5502),(0),(1))); }
        /* voice 0 stage 2: bitcrush */
        s = al_bitcrush(s, al_clamp((3.9912)+(cha)*(0.8709),(3),(16)));
        /* voice 0 stage 3: chorus */
        s = al_chorus_tick(&x->v0_st3, s, al_clamp((3.6141)+(w5)*(2.0529),(0),(9)), al_clamp((2.159)+(v1)*(0.2536),(0.05),(2.5)), sr);
        /* voice 0 stage 4: chorus */
        s = al_chorus_tick(&x->v0_st4, s, al_clamp((2.6279)+(seqenv)*(0.6079),(0),(9)), al_clamp((0.129)+(pn)*(0.8622),(0.05),(2.5)), sr);
        double _vo0 = al_softclip(s);
        s = _vo0;
        x->_vbus[0] = _vo0;
        x->_vbus[1] = 0.0;
        x->_vbus[2] = 0.0;
        s += al_clamp(ofb * (al_clamp((0.0959)+(cha)*(0.3856),(0),(0.6))), -0.9, 0.9);
        x->_ofbus = al_softclip(s);
        s = al_dc_tick(&x->dc, s);
        if (!(s==s)) s = 0.0;
        al_env_tick(&x->_env, s, 0.0587, 0.00959);
        { double _ov = al_softclip(s); x->_fbprev = fabs(_ov);
           *out++ = (t_sample)(_ov * 0.6); }
    }
    return (w + 6);
}

static void alien_xsnap_ep05000_05_dsp(t_alien_xsnap_ep05000_05 *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    dsp_add(alien_xsnap_ep05000_05_perform, 5, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

static void *alien_xsnap_ep05000_05_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_xsnap_ep05000_05 *x = (t_alien_xsnap_ep05000_05 *)pd_new(alien_xsnap_ep05000_05_class);
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    al_rng_seed(&x->rng, 515748843u);
    al_wander_init(&x->wnd, &x->rng, 8);
    al_dc_init(&x->dc);
    al_phasor_init(&x->_lfo); al_sh_init(&x->_sh); al_env_init(&x->_env);
    x->_cha.x=x->_cha.y=x->_cha.z=0.1; x->_chaacc=0.0; x->_chav=0.5; x->_fbprev=0.0;
    x->_vbus[0]=x->_vbus[1]=x->_vbus[2]=0.0; x->_ofbus=0.0;
    x->_seqph=0.0; x->_seqenv=0.0; x->_seqi=-1;
    x->v0_src0_t=0; x->v0_src0_rng=2463534242u; x->v0_src0_ph=0.0; x->v0_src0_cur=0.0; x->v0_src0_prev=0.0;
    { static const double _mr[6]={1.0,2.76,5.40,8.93,13.34,18.64}; int _i; for(_i=0;_i<6;_i++) x->v0_st0_r[_i]=_mr[_i]; al_modbank_init(&x->v0_st0,6,x->v0_st0_r); }
    al_phasor_init(&x->v0_st1);
    al_chorus_init(&x->v0_st3);
    al_chorus_init(&x->v0_st4);
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_xsnap_ep05000_05_tilde_setup(void)
{
    alien_xsnap_ep05000_05_class = class_new(gensym("alien_xsnap_ep05000_05~"),
        (t_newmethod)alien_xsnap_ep05000_05_new, 0, sizeof(t_alien_xsnap_ep05000_05), CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_xsnap_ep05000_05_class, t_alien_xsnap_ep05000_05, x_f);
    class_addmethod(alien_xsnap_ep05000_05_class, (t_method)alien_xsnap_ep05000_05_dsp, gensym("dsp"), A_CANT, 0);
}
