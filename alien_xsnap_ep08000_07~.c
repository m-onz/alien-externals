/* alien_xsnap_ep08000_07~ -- auto-bred specimen (recipe genome; do not hand-edit)
 * source=bytebeat  stages=['multiband', 'chorus', 'multiband', 'svf']
 * inlet0 freq Hz | inlet1 param 0..100 (wraps) | outlet0 signal
 */
#include "m_pd.h"
#include "alien.h"
#include <string.h>

static t_class *alien_xsnap_ep08000_07_class;
#ifndef BB_SAFE
#define BB_SAFE 1
#define BB_DIV(a,b) ((b)?(a)/(b):0)
#define BB_MOD(a,b) ((b)?(a)%(b):0)
#endif

typedef struct _alien_xsnap_ep08000_07 {
    t_object x_obj;
    t_float  x_f;
    double   x_sr;
    al_rng    rng;
    al_wander wnd;
    al_dc     dc;
    al_phasor _lfo; al_sh _sh; al_env _env; al_v3 _cha; double _chaacc, _chav, _fbprev; double _vbus[3]; double _ofbus; double _seqph, _seqenv; int _seqi;
    long v0_src0_t; unsigned v0_src0_rng; double v0_src0_ph,v0_src0_cur,v0_src0_prev;
    al_svf v0_st0;
    al_chorus v0_st1;
    al_svf v0_st2;
    al_svf v0_st3;
    al_cloud v1_src0;
    al_chorus v1_st0;
    al_phasor v1_st1;
} t_alien_xsnap_ep08000_07;

static t_int *alien_xsnap_ep08000_07_perform(t_int *w)
{
    t_alien_xsnap_ep08000_07 *x = (t_alien_xsnap_ep08000_07 *)(w[1]);
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
        al_wander_tick(&x->wnd, &x->rng, 5.349, sr);
        double w0=al_wander_val(&x->wnd,0), w1=al_wander_val(&x->wnd,1), w2=al_wander_val(&x->wnd,2), w3=al_wander_val(&x->wnd,3), w4=al_wander_val(&x->wnd,4), w5=al_wander_val(&x->wnd,5), w6=al_wander_val(&x->wnd,6);
        double lfo = al_sin01(al_phasor_tick(&x->_lfo, 4.78759, sr))*0.5+0.5;
        double shv = al_sh_tick(&x->_sh, &x->rng, 20.6334, sr)*0.5+0.5;
        x->_chaacc += 0.01049; if (x->_chaacc>=1.0){ x->_chaacc-=floor(x->_chaacc); x->_chav=al_map_logistic(x->_chav, 3.5); }
        double cha = x->_chav;
        double env = x->_env.e;
        double fb  = x->_fbprev;
        static const double _seqtab[9] = {0.142,0.773,0.021,0.486,0.184,0.783,0.077,0.95,0.965};
        x->_seqph += (7.218)/sr; if (x->_seqph >= 9.0) x->_seqph -= 9.0;
        int _si = (int)x->_seqph; if(_si<0)_si=0; if(_si>=9)_si=8;
        if (_si != x->_seqi){ x->_seqi=_si; x->_seqenv=1.0; }
        x->_seqenv *= exp(-1000.0/((100.3)*sr));
        double seq = _seqtab[_si];
        double seqenv = x->_seqenv;
        (void)(lfo+shv+cha+env+fb+seq+seqenv);
        double v0=x->_vbus[0], v1=x->_vbus[1], v2=x->_vbus[2], ofb=x->_ofbus; (void)(v0+v1+v2+ofb);
        /* voice 0 source: bytebeat */
        s = 0.0;
        { double _cl = al_clamp(f0*al_clamp((315.433)+(pn)*(190.931),(16),(2048)), 20.0, sr*0.48); x->v0_src0_ph += _cl/sr;
          while (x->v0_src0_ph >= 1.0) { x->v0_src0_ph -= 1.0; x->v0_src0_t++;
            x->v0_src0_rng ^= x->v0_src0_rng<<13; x->v0_src0_rng ^= x->v0_src0_rng>>17; x->v0_src0_rng ^= x->v0_src0_rng<<5;
            { long t=x->v0_src0_t, k=(long)(pn*2.55), r=(long)(x->v0_src0_rng & 0xffffL);
              long _v = (BB_MOD(((((t)&(((t)<<((14)&31)))))|(((t)|(t)))),(((~(((k)|(t)))))*(((13)&(((k)+(t))))))));
              x->v0_src0_prev=x->v0_src0_cur; x->v0_src0_cur=((double)((_v & 4095L)-2048L)/2048.0); } }
          s = x->v0_src0_prev + (x->v0_src0_cur - x->v0_src0_prev)*x->v0_src0_ph; }
        /* voice 0 stage 0: multiband */
        { double _lo = al_svf_lp(&x->v0_st0, s, al_clamp((120)*pow(2.0,(4.382)+(w2)*(0.6288)),(200),(6000)), 0.3, sr); double _hi = s - _lo;
          _lo = al_drive(_lo, al_clamp((0.3743)+(w6)*(0.4771),(0),(0.9)))*al_clamp((1.0876)+(pn)*(0.2642),(0.2),(1.6)); _hi = al_fold(_hi, al_clamp((0.7401)+(pn)*(0.249),(0),(1.2)))*al_clamp((0.6029)+(w2)*(0.5352),(0.2),(1.5));
          s = al_softclip(_lo + _hi); }
        /* voice 0 stage 1: chorus */
        s = al_chorus_tick(&x->v0_st1, s, al_clamp((4.7736)+(ofb)*(0.2899),(0),(9)), al_clamp((0.7025)+(w5)*(0.5035),(0.05),(2.5)), sr);
        /* voice 0 stage 2: multiband */
        { double _lo = al_svf_lp(&x->v0_st2, s, al_clamp((120)*pow(2.0,(6.1784)+(w0)*(0.6962)),(200),(6000)), 0.3, sr); double _hi = s - _lo;
          _lo = al_drive(_lo, al_clamp((0.1771)+(w2)*(0.5416),(0),(0.9)))*al_clamp((0.7108)+(w0)*(0.319),(0.2),(1.6)); _hi = al_fold(_hi, al_clamp((0.5389)+(v1)*(0.5828),(0),(1.2)))*al_clamp((1.0312)+(0.0)*(0),(0.2),(1.5));
          s = al_softclip(_lo + _hi); }
        /* voice 0 stage 3: svf */
        s = al_svf_lp(&x->v0_st3, s, al_clamp((80)*pow(2.0,(5.351)+(pn)*(1.0176)),(60),(15000)), al_clamp((0.2799)+(w2)*(0.2625),(0),(0.9)), sr);
        double _vo0 = al_softclip(s);
        /* voice 1 source: cloud */
        s = 0.0;
        s = al_cloud_tick(&x->v1_src0, &x->rng, al_clamp((210.194)+(env)*(68.4009),(15),(500)), al_clamp((54.1357)+(w1)*(27.1431),(6),(90)), f0, al_clamp((0.0108)+(w4)*(0.3784),(0),(0.6)), sr);
        /* voice 1 stage 0: chorus */
        s = al_chorus_tick(&x->v1_st0, s, al_clamp((3.3019)+(cha)*(1.4266),(0),(9)), al_clamp((0.8215)+(seqenv)*(0.9237),(0.05),(2.5)), sr);
        /* voice 1 stage 1: ring */
        { double _m = al_sin01(al_phasor_tick(&x->v1_st1, al_clamp(f0*al_clamp((3.0605)+(shv)*(4.1085),(0.125),(14)),20.0,sr*0.45), sr));
          s = al_ringmod(s, _m, al_clamp((0.5396)+(0.0)*(0),(0),(1))); }
        double _vo1 = al_softclip(s);
        s = (_vo0 + _vo1) * 0.5;
        x->_vbus[0] = _vo0;
        x->_vbus[1] = _vo1;
        x->_vbus[2] = 0.0;
        s += al_clamp(ofb * (al_clamp((0.4195)+(lfo)*(0.2756),(0),(0.6))), -0.9, 0.9);
        x->_ofbus = al_softclip(s);
        s = al_dc_tick(&x->dc, s);
        if (!(s==s)) s = 0.0;
        al_env_tick(&x->_env, s, 0.22169, 0.00408);
        { double _ov = al_softclip(s); x->_fbprev = fabs(_ov);
           *out++ = (t_sample)(_ov * 0.6); }
    }
    return (w + 6);
}

static void alien_xsnap_ep08000_07_dsp(t_alien_xsnap_ep08000_07 *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    dsp_add(alien_xsnap_ep08000_07_perform, 5, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

static void *alien_xsnap_ep08000_07_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_xsnap_ep08000_07 *x = (t_alien_xsnap_ep08000_07 *)pd_new(alien_xsnap_ep08000_07_class);
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    al_rng_seed(&x->rng, 983936054u);
    al_wander_init(&x->wnd, &x->rng, 7);
    al_dc_init(&x->dc);
    al_phasor_init(&x->_lfo); al_sh_init(&x->_sh); al_env_init(&x->_env);
    x->_cha.x=x->_cha.y=x->_cha.z=0.1; x->_chaacc=0.0; x->_chav=0.5; x->_fbprev=0.0;
    x->_vbus[0]=x->_vbus[1]=x->_vbus[2]=0.0; x->_ofbus=0.0;
    x->_seqph=0.0; x->_seqenv=0.0; x->_seqi=-1;
    x->v0_src0_t=0; x->v0_src0_rng=2463534242u; x->v0_src0_ph=0.0; x->v0_src0_cur=0.0; x->v0_src0_prev=0.0;
    al_svf_init(&x->v0_st0);
    al_chorus_init(&x->v0_st1);
    al_svf_init(&x->v0_st2);
    al_svf_init(&x->v0_st3);
    al_cloud_init(&x->v1_src0);
    al_chorus_init(&x->v1_st0);
    al_phasor_init(&x->v1_st1);
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_xsnap_ep08000_07_tilde_setup(void)
{
    alien_xsnap_ep08000_07_class = class_new(gensym("alien_xsnap_ep08000_07~"),
        (t_newmethod)alien_xsnap_ep08000_07_new, 0, sizeof(t_alien_xsnap_ep08000_07), CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_xsnap_ep08000_07_class, t_alien_xsnap_ep08000_07, x_f);
    class_addmethod(alien_xsnap_ep08000_07_class, (t_method)alien_xsnap_ep08000_07_dsp, gensym("dsp"), A_CANT, 0);
}
