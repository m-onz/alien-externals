/* alien_xfreesnap_ep07000_09~ -- auto-bred specimen (recipe genome; do not hand-edit)
 * source=modal  stages=['fshift', 'pmshape', 'ladder', 'pmshape']
 * inlet0 freq Hz | inlet1 param 0..100 (wraps) | outlet0 signal
 */
#include "m_pd.h"
#include "alien.h"
#include <string.h>

static t_class *alien_xfreesnap_ep07000_09_class;
#ifndef BB_SAFE
#define BB_SAFE 1
#define BB_DIV(a,b) ((b)?(a)/(b):0)
#define BB_MOD(a,b) ((b)?(a)%(b):0)
#endif

typedef struct _alien_xfreesnap_ep07000_09 {
    t_object x_obj;
    t_float  x_f;
    double   x_sr;
    al_rng    rng;
    al_wander wnd;
    al_dc     dc;
    al_phasor _lfo; al_sh _sh; al_env _env; al_v3 _cha; double _chaacc, _chav, _fbprev; double _vbus[3]; double _ofbus; double _seqph, _seqenv; int _seqi;
    al_modbank v0_src0; double v0_src0_r[6]; al_phasor v0_src0_x; double v0_src0_pv;
    al_hilbert v0_st0_h; double v0_st0_op, v0_st0_fb;
    al_phasor v0_st1;
    al_ladder v0_st2;
    al_phasor v0_st3;
    long v1_src0_t; unsigned v1_src0_rng; double v1_src0_ph,v1_src0_cur,v1_src0_prev;
    al_hilbert v1_st0_h; double v1_st0_op, v1_st0_fb;
    al_fdnrev v1_st1;
    al_chorus v1_st2;
} t_alien_xfreesnap_ep07000_09;

static t_int *alien_xfreesnap_ep07000_09_perform(t_int *w)
{
    t_alien_xfreesnap_ep07000_09 *x = (t_alien_xfreesnap_ep07000_09 *)(w[1]);
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
        al_wander_tick(&x->wnd, &x->rng, 2.355, sr);
        double w0=al_wander_val(&x->wnd,0), w1=al_wander_val(&x->wnd,1), w2=al_wander_val(&x->wnd,2), w3=al_wander_val(&x->wnd,3), w4=al_wander_val(&x->wnd,4), w5=al_wander_val(&x->wnd,5);
        double lfo = al_sin01(al_phasor_tick(&x->_lfo, 7.8074, sr))*0.5+0.5;
        double shv = al_sh_tick(&x->_sh, &x->rng, 6.1738, sr)*0.5+0.5;
        x->_chaacc += 0.00855; if (x->_chaacc>=1.0){ x->_chaacc-=floor(x->_chaacc); x->_chav=al_map_logistic(x->_chav, 4); }
        double cha = x->_chav;
        double env = x->_env.e;
        double fb  = x->_fbprev;
        static const double _seqtab[13] = {0.667,0.143,0.371,0.349,0.751,0.411,0.368,0.794,0.206,0.065,0.238,0.021,0.669};
        x->_seqph += (4.14)/sr; if (x->_seqph >= 13.0) x->_seqph -= 13.0;
        int _si = (int)x->_seqph; if(_si<0)_si=0; if(_si>=13)_si=12;
        if (_si != x->_seqi){ x->_seqi=_si; x->_seqenv=1.0; }
        x->_seqenv *= exp(-1000.0/((465.375)*sr));
        double seq = _seqtab[_si];
        double seqenv = x->_seqenv;
        (void)(lfo+shv+cha+env+fb+seq+seqenv);
        double v0=x->_vbus[0], v1=x->_vbus[1], v2=x->_vbus[2], ofb=x->_ofbus; (void)(v0+v1+v2+ofb);
        /* voice 0 source: modal */
        s = 0.0;
        { double _ph=al_phasor_tick(&x->v0_src0_x, f0*0.25, sr);
          double _exc=(_ph < x->v0_src0_pv)?1.0:0.0; x->v0_src0_pv=_ph;
          s = al_modbank_tick(&x->v0_src0, _exc, f0, al_clamp((37.4153)+(w1)*(6.3032),(10),(70)), sr) * 1.5;
          s += al_clamp((0.3015)+(0.0)*(0),(0.12),(0.5))*al_sin01(al_wrap01(_ph)); }
        /* voice 0 stage 0: fshift */
        { double _I,_Q; al_hilbert_tick(&x->v0_st0_h, s + x->v0_st0_fb*al_clamp((0.1801)+(pn)*(0.2803),(0),(0.85)), &_I,&_Q);
          x->v0_st0_op += (al_clamp((-384.141)+(pn)*(189.168),(-1500),(1500)))/sr; x->v0_st0_op -= floor(x->v0_st0_op);
          double _c=cos(AL_2PI*x->v0_st0_op), _sq=sin(AL_2PI*x->v0_st0_op);
          double _ssb=_I*_c - _Q*_sq; x->v0_st0_fb=al_clamp(_ssb,-1.0,1.0);
          s = al_lerp(s, _ssb, al_clamp((0.9411)+(seqenv)*(0.3873),(0.2),(1))); }
        /* voice 0 stage 1: pmshape */
        { double _c = al_phasor_tick(&x->v0_st1, al_clamp(f0*al_clamp((3.2271)+(pn)*(1.0213),(0.125),(8)),20.0,sr*0.45), sr);
          double _y = sin(AL_2PI*(_c + al_clamp((1.6196)+(v2)*(3.7021),(0),(5))*s));
          s = al_lerp(s, _y, al_clamp((0.7197)+(pn)*(0.4042),(0),(1))); }
        /* voice 0 stage 2: ladder */
        s = al_ladder_tick(&x->v0_st2, s, al_clamp((80)*pow(2.0,(4.8353)+(pn)*(1.0041)),(60),(14000)), al_clamp((0.2918)+(shv)*(0.1604),(0),(0.85)), sr);
        /* voice 0 stage 3: pmshape */
        { double _c = al_phasor_tick(&x->v0_st3, al_clamp(f0*al_clamp((3.7572)+(env)*(2.7715),(0.125),(8)),20.0,sr*0.45), sr);
          double _y = sin(AL_2PI*(_c + al_clamp((2.4048)+(env)*(3.3221),(0),(5))*s));
          s = al_lerp(s, _y, al_clamp((0.8545)+(pn)*(0.1913),(0),(1))); }
        double _vo0 = al_softclip(s);
        /* voice 1 source: bytebeat */
        s = 0.0;
        { double _cl = al_clamp(f0*al_clamp((205.481)+(ofb)*(54.1206),(16),(2048)), 20.0, sr*0.48); x->v1_src0_ph += _cl/sr;
          while (x->v1_src0_ph >= 1.0) { x->v1_src0_ph -= 1.0; x->v1_src0_t++;
            x->v1_src0_rng ^= x->v1_src0_rng<<13; x->v1_src0_rng ^= x->v1_src0_rng>>17; x->v1_src0_rng ^= x->v1_src0_rng<<5;
            { long t=x->v1_src0_t, k=(long)(pn*2.55), r=(long)(x->v1_src0_rng & 0xffffL);
              long _v = (BB_MOD(t,((((t)<<((1)&31)))<<((2)&31))));
              x->v1_src0_prev=x->v1_src0_cur; x->v1_src0_cur=((double)((_v & 2047L)-1024L)/1024.0); } }
          s = x->v1_src0_prev + (x->v1_src0_cur - x->v1_src0_prev)*x->v1_src0_ph; }
        /* voice 1 stage 0: fshift */
        { double _I,_Q; al_hilbert_tick(&x->v1_st0_h, s + x->v1_st0_fb*al_clamp((0.2208)+(0.0)*(0),(0),(0.85)), &_I,&_Q);
          x->v1_st0_op += (al_clamp((84.5778)+(seqenv)*(561.382),(-1500),(1500)))/sr; x->v1_st0_op -= floor(x->v1_st0_op);
          double _c=cos(AL_2PI*x->v1_st0_op), _sq=sin(AL_2PI*x->v1_st0_op);
          double _ssb=_I*_c - _Q*_sq; x->v1_st0_fb=al_clamp(_ssb,-1.0,1.0);
          s = al_lerp(s, _ssb, al_clamp((0.9586)+(0.0)*(0),(0.2),(1))); }
        /* voice 1 stage 1: fdn */
        s = al_lerp(s, al_fdnrev_tick(&x->v1_st1, s, al_clamp((0.3356)+(v1)*(0.4553),(0.1),(1)), al_clamp((0.3692)+(v0)*(0.1644),(0),(0.8)), al_clamp((0.4052)+(cha)*(0.3217),(0),(0.85))), al_clamp((0.4091)+(v0)*(0.1921),(0.1),(0.6)));
        /* voice 1 stage 2: chorus */
        s = al_chorus_tick(&x->v1_st2, s, al_clamp((4.2656)+(0.0)*(0),(0),(9)), al_clamp((0.2495)+(w4)*(0.9682),(0.05),(2.5)), sr);
        double _vo1 = al_softclip(s);
        s = (_vo0 + _vo1) * 0.5;
        x->_vbus[0] = _vo0;
        x->_vbus[1] = _vo1;
        x->_vbus[2] = 0.0;
        s += al_clamp(ofb * (al_clamp((0.0571)+(pn)*(0.2948),(0),(0.6))), -0.9, 0.9);
        x->_ofbus = al_softclip(s);
        s = al_dc_tick(&x->dc, s);
        if (!(s==s)) s = 0.0;
        al_env_tick(&x->_env, s, 0.32712, 0.03562);
        { double _ov = al_softclip(s); x->_fbprev = fabs(_ov);
           *out++ = (t_sample)(_ov * 0.6); }
    }
    return (w + 6);
}

static void alien_xfreesnap_ep07000_09_dsp(t_alien_xfreesnap_ep07000_09 *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    dsp_add(alien_xfreesnap_ep07000_09_perform, 5, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

static void *alien_xfreesnap_ep07000_09_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_xfreesnap_ep07000_09 *x = (t_alien_xfreesnap_ep07000_09 *)pd_new(alien_xfreesnap_ep07000_09_class);
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    al_rng_seed(&x->rng, 318817711u);
    al_wander_init(&x->wnd, &x->rng, 6);
    al_dc_init(&x->dc);
    al_phasor_init(&x->_lfo); al_sh_init(&x->_sh); al_env_init(&x->_env);
    x->_cha.x=x->_cha.y=x->_cha.z=0.1; x->_chaacc=0.0; x->_chav=0.5; x->_fbprev=0.0;
    x->_vbus[0]=x->_vbus[1]=x->_vbus[2]=0.0; x->_ofbus=0.0;
    x->_seqph=0.0; x->_seqenv=0.0; x->_seqi=-1;
    { static const double _mr[6]={1.0,2.76,5.40,8.93,13.34,18.64}; int _i; for(_i=0;_i<6;_i++) x->v0_src0_r[_i]=_mr[_i];
      al_modbank_init(&x->v0_src0, 6, x->v0_src0_r); al_phasor_init(&x->v0_src0_x); x->v0_src0_pv=0.0; }
    al_hilbert_init(&x->v0_st0_h); x->v0_st0_op=0.0; x->v0_st0_fb=0.0;
    al_phasor_init(&x->v0_st1);
    al_ladder_init(&x->v0_st2);
    al_phasor_init(&x->v0_st3);
    x->v1_src0_t=0; x->v1_src0_rng=2463534242u; x->v1_src0_ph=0.0; x->v1_src0_cur=0.0; x->v1_src0_prev=0.0;
    al_hilbert_init(&x->v1_st0_h); x->v1_st0_op=0.0; x->v1_st0_fb=0.0;
    al_fdnrev_init(&x->v1_st1);
    al_chorus_init(&x->v1_st2);
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_xfreesnap_ep07000_09_tilde_setup(void)
{
    alien_xfreesnap_ep07000_09_class = class_new(gensym("alien_xfreesnap_ep07000_09~"),
        (t_newmethod)alien_xfreesnap_ep07000_09_new, 0, sizeof(t_alien_xfreesnap_ep07000_09), CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_xfreesnap_ep07000_09_class, t_alien_xfreesnap_ep07000_09, x_f);
    class_addmethod(alien_xfreesnap_ep07000_09_class, (t_method)alien_xfreesnap_ep07000_09_dsp, gensym("dsp"), A_CANT, 0);
}
