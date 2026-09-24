/* alien_xfreesnap_ep07000_02~ -- auto-bred specimen (recipe genome; do not hand-edit)
 * source=bytebeat  stages=['svf', 'svf', 'formants']
 * inlet0 freq Hz | inlet1 param 0..100 (wraps) | outlet0 signal
 */
#include "m_pd.h"
#include "alien.h"
#include <string.h>

static t_class *alien_xfreesnap_ep07000_02_class;
#ifndef BB_SAFE
#define BB_SAFE 1
#define BB_DIV(a,b) ((b)?(a)/(b):0)
#define BB_MOD(a,b) ((b)?(a)%(b):0)
#endif

typedef struct _alien_xfreesnap_ep07000_02 {
    t_object x_obj;
    t_float  x_f;
    double   x_sr;
    al_rng    rng;
    al_wander wnd;
    al_dc     dc;
    al_phasor _lfo; al_sh _sh; al_env _env; al_v3 _cha; double _chaacc, _chav, _fbprev; double _vbus[3]; double _ofbus; double _seqph, _seqenv; int _seqi;
    long v0_src0_t; unsigned v0_src0_rng; double v0_src0_ph,v0_src0_cur,v0_src0_prev;
    al_svf v0_st0;
    al_svf v0_st1;
    al_formants v0_st2;
} t_alien_xfreesnap_ep07000_02;

static t_int *alien_xfreesnap_ep07000_02_perform(t_int *w)
{
    t_alien_xfreesnap_ep07000_02 *x = (t_alien_xfreesnap_ep07000_02 *)(w[1]);
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
        al_wander_tick(&x->wnd, &x->rng, 2.627, sr);
        double w0=al_wander_val(&x->wnd,0), w1=al_wander_val(&x->wnd,1), w2=al_wander_val(&x->wnd,2), w3=al_wander_val(&x->wnd,3), w4=al_wander_val(&x->wnd,4);
        double lfo = al_sin01(al_phasor_tick(&x->_lfo, 4.59009, sr))*0.5+0.5;
        double shv = al_sh_tick(&x->_sh, &x->rng, 18.0016, sr)*0.5+0.5;
        x->_chaacc += 0.01342; if (x->_chaacc>=1.0){ x->_chaacc-=floor(x->_chaacc); x->_chav=al_map_logistic(x->_chav, 3.7148); }
        double cha = x->_chav;
        double env = x->_env.e;
        double fb  = x->_fbprev;
        static const double _seqtab[16] = {0.492,0.501,0.209,0.125,0.749,0.99,0.265,0.373,0.231,0.102,0.515,0.511,0.13,0.923,0.979,0.068};
        x->_seqph += (0.497)/sr; if (x->_seqph >= 16.0) x->_seqph -= 16.0;
        int _si = (int)x->_seqph; if(_si<0)_si=0; if(_si>=16)_si=15;
        if (_si != x->_seqi){ x->_seqi=_si; x->_seqenv=1.0; }
        x->_seqenv *= exp(-1000.0/((43.5)*sr));
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
              long _v = (((BB_MOD(((((t)&(((t)<<((14)&31)))))|(((t)|(t)))),(((~(((k)|(t)))))*(((13)&(((k)+(t))))))))+(k)));
              x->v0_src0_prev=x->v0_src0_cur; x->v0_src0_cur=((double)((_v & 1023L)-512L)/512.0); } }
          s = x->v0_src0_prev + (x->v0_src0_cur - x->v0_src0_prev)*x->v0_src0_ph; }
        /* voice 0 stage 0: svf */
        s = al_svf_lp(&x->v0_st0, s, al_clamp((80)*pow(2.0,(4.92)+(pn)*(3.7254)),(60),(15000)), al_clamp((0.4313)+(lfo)*(0.3812),(0),(0.9)), sr);
        /* voice 0 stage 1: svf */
        s = al_svf_lp(&x->v0_st1, s, al_clamp((80)*pow(2.0,(3.2622)+(w0)*(0.9784)),(60),(15000)), al_clamp((0.2999)+(w0)*(0.2099),(0),(0.9)), sr);
        /* voice 0 stage 2: formants */
        s = al_lerp(s, al_formants_tick(&x->v0_st2, s, al_clamp((0.2086)+(lfo)*(0.2195),(0),(1)), al_clamp((13.8905)+(pn)*(7.7984),(2),(22)), sr), al_clamp((0.9716)+(pn)*(0.4858),(0.2),(1)));
        double _vo0 = al_softclip(s);
        s = _vo0;
        x->_vbus[0] = _vo0;
        x->_vbus[1] = 0.0;
        x->_vbus[2] = 0.0;
        s += al_clamp(ofb * (al_clamp((0.1469)+(cha)*(0.6528),(0),(0.6))), -0.9, 0.9);
        x->_ofbus = al_softclip(s);
        s = al_dc_tick(&x->dc, s);
        if (!(s==s)) s = 0.0;
        al_env_tick(&x->_env, s, 0.2542, 0.03771);
        { double _ov = al_softclip(s); x->_fbprev = fabs(_ov);
           *out++ = (t_sample)(_ov * 0.6); }
    }
    return (w + 6);
}

static void alien_xfreesnap_ep07000_02_dsp(t_alien_xfreesnap_ep07000_02 *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    dsp_add(alien_xfreesnap_ep07000_02_perform, 5, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

static void *alien_xfreesnap_ep07000_02_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_xfreesnap_ep07000_02 *x = (t_alien_xfreesnap_ep07000_02 *)pd_new(alien_xfreesnap_ep07000_02_class);
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    al_rng_seed(&x->rng, 874175083u);
    al_wander_init(&x->wnd, &x->rng, 5);
    al_dc_init(&x->dc);
    al_phasor_init(&x->_lfo); al_sh_init(&x->_sh); al_env_init(&x->_env);
    x->_cha.x=x->_cha.y=x->_cha.z=0.1; x->_chaacc=0.0; x->_chav=0.5; x->_fbprev=0.0;
    x->_vbus[0]=x->_vbus[1]=x->_vbus[2]=0.0; x->_ofbus=0.0;
    x->_seqph=0.0; x->_seqenv=0.0; x->_seqi=-1;
    x->v0_src0_t=0; x->v0_src0_rng=2463534242u; x->v0_src0_ph=0.0; x->v0_src0_cur=0.0; x->v0_src0_prev=0.0;
    al_svf_init(&x->v0_st0);
    al_svf_init(&x->v0_st1);
    al_formants_init(&x->v0_st2);
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_xfreesnap_ep07000_02_tilde_setup(void)
{
    alien_xfreesnap_ep07000_02_class = class_new(gensym("alien_xfreesnap_ep07000_02~"),
        (t_newmethod)alien_xfreesnap_ep07000_02_new, 0, sizeof(t_alien_xfreesnap_ep07000_02), CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_xfreesnap_ep07000_02_class, t_alien_xfreesnap_ep07000_02, x_f);
    class_addmethod(alien_xfreesnap_ep07000_02_class, (t_method)alien_xfreesnap_ep07000_02_dsp, gensym("dsp"), A_CANT, 0);
}
