/* alien_xsnap_ep02000_10~ -- auto-bred specimen (recipe genome; do not hand-edit)
 * source=undertone  stages=['bpf', 'fdn', 'bitcrush', 'chorus', 'chorus']
 * inlet0 freq Hz | inlet1 param 0..100 (wraps) | outlet0 signal
 */
#include "m_pd.h"
#include "alien.h"
#include <string.h>

static t_class *alien_xsnap_ep02000_10_class;


typedef struct _alien_xsnap_ep02000_10 {
    t_object x_obj;
    t_float  x_f;
    double   x_sr;
    al_rng    rng;
    al_wander wnd;
    al_dc     dc;
    al_phasor _lfo; al_sh _sh; al_env _env; al_v3 _cha; double _chaacc, _chav, _fbprev; double _vbus[3]; double _ofbus; double _seqph, _seqenv; int _seqi;
    double v0_src0_up[8];
    al_svf v0_st0;
    al_fdnrev v0_st1;
    al_chorus v0_st3;
    al_chorus v0_st4;
} t_alien_xsnap_ep02000_10;

static t_int *alien_xsnap_ep02000_10_perform(t_int *w)
{
    t_alien_xsnap_ep02000_10 *x = (t_alien_xsnap_ep02000_10 *)(w[1]);
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
        al_wander_tick(&x->wnd, &x->rng, 3.739, sr);
        double w0=al_wander_val(&x->wnd,0), w1=al_wander_val(&x->wnd,1), w2=al_wander_val(&x->wnd,2), w3=al_wander_val(&x->wnd,3), w4=al_wander_val(&x->wnd,4), w5=al_wander_val(&x->wnd,5), w6=al_wander_val(&x->wnd,6), w7=al_wander_val(&x->wnd,7);
        double lfo = al_sin01(al_phasor_tick(&x->_lfo, 3.6411, sr))*0.5+0.5;
        double shv = al_sh_tick(&x->_sh, &x->rng, 8.5229, sr)*0.5+0.5;
        x->_chaacc += 0.01961; if (x->_chaacc>=1.0){ x->_chaacc-=floor(x->_chaacc); x->_chav=al_map_logistic(x->_chav, 4); }
        double cha = x->_chav;
        double env = x->_env.e;
        double fb  = x->_fbprev;
        static const double _seqtab[15] = {0.113,0.766,0.939,0.62,0.812,0.979,0.681,0.715,0.204,0.067,0.571,0.641,0.855,0.221,0.133};
        x->_seqph += (2.133)/sr; if (x->_seqph >= 15.0) x->_seqph -= 15.0;
        int _si = (int)x->_seqph; if(_si<0)_si=0; if(_si>=15)_si=14;
        if (_si != x->_seqi){ x->_seqi=_si; x->_seqenv=1.0; }
        x->_seqenv *= exp(-1000.0/((609.568)*sr));
        double seq = _seqtab[_si];
        double seqenv = x->_seqenv;
        (void)(lfo+shv+cha+env+fb+seq+seqenv);
        double v0=x->_vbus[0], v1=x->_vbus[1], v2=x->_vbus[2], ofb=x->_ofbus; (void)(v0+v1+v2+ofb);
        /* voice 0 source: undertone */
        s = 0.0;
        { double _acc=0.0,_wsum=0.0; int _i; double _tl=al_clamp((0.6071)+(w0)*(0.6991),(0),(2.5));
          for(_i=0;_i<3;_i++){ double _f=f0/(_i+1.0); x->v0_src0_up[_i]+=_f/sr;
            if(x->v0_src0_up[_i]>=1.0)x->v0_src0_up[_i]-=1.0; double _wt=pow(_i+1.0,-_tl);
            _acc+=_wt*al_sin01(x->v0_src0_up[_i]); _wsum+=_wt; }
          s = (_wsum>0.0)? _acc/_wsum : 0.0; }
        /* voice 0 stage 0: bpf */
        s = al_lerp(s, al_svf_bp(&x->v0_st0, s, al_clamp((80)*pow(2.0,(4.0609)+(w6)*(1.1773)),(80),(15000)), al_clamp((0.2807)+(pn)*(0.5268),(0.1),(0.95)), sr), al_clamp((0.526)+(0.0)*(0),(0.2),(1)));
        /* voice 0 stage 1: fdn */
        s = al_lerp(s, al_fdnrev_tick(&x->v0_st1, s, al_clamp((0.309)+(ofb)*(0.2841),(0.1),(1)), al_clamp((0.3303)+(pn)*(0.3362),(0),(0.8)), al_clamp((0.6115)+(seq)*(0.229),(0),(0.85))), al_clamp((0.4005)+(0.0)*(0),(0.1),(0.6)));
        /* voice 0 stage 2: bitcrush */
        s = al_bitcrush(s, al_clamp((7.7442)+(lfo)*(4.2306),(3),(16)));
        /* voice 0 stage 3: chorus */
        s = al_chorus_tick(&x->v0_st3, s, al_clamp((2.7631)+(w5)*(2.0529),(0),(9)), al_clamp((0.1095)+(v1)*(0.2793),(0.05),(2.5)), sr);
        /* voice 0 stage 4: chorus */
        s = al_chorus_tick(&x->v0_st4, s, al_clamp((2.6279)+(0.0)*(0),(0),(9)), al_clamp((0.1757)+(pn)*(0.9825),(0.05),(2.5)), sr);
        double _vo0 = al_softclip(s);
        s = _vo0;
        x->_vbus[0] = _vo0;
        x->_vbus[1] = 0.0;
        x->_vbus[2] = 0.0;
        s += al_clamp(ofb * (al_clamp((0.348)+(seq)*(0.3856),(0),(0.6))), -0.9, 0.9);
        x->_ofbus = al_softclip(s);
        s = al_dc_tick(&x->dc, s);
        if (!(s==s)) s = 0.0;
        al_env_tick(&x->_env, s, 0.10393, 0.01463);
        { double _ov = al_softclip(s); x->_fbprev = fabs(_ov);
           *out++ = (t_sample)(_ov * 0.6); }
    }
    return (w + 6);
}

static void alien_xsnap_ep02000_10_dsp(t_alien_xsnap_ep02000_10 *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    dsp_add(alien_xsnap_ep02000_10_perform, 5, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

static void *alien_xsnap_ep02000_10_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_xsnap_ep02000_10 *x = (t_alien_xsnap_ep02000_10 *)pd_new(alien_xsnap_ep02000_10_class);
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    al_rng_seed(&x->rng, 457252624u);
    al_wander_init(&x->wnd, &x->rng, 8);
    al_dc_init(&x->dc);
    al_phasor_init(&x->_lfo); al_sh_init(&x->_sh); al_env_init(&x->_env);
    x->_cha.x=x->_cha.y=x->_cha.z=0.1; x->_chaacc=0.0; x->_chav=0.5; x->_fbprev=0.0;
    x->_vbus[0]=x->_vbus[1]=x->_vbus[2]=0.0; x->_ofbus=0.0;
    x->_seqph=0.0; x->_seqenv=0.0; x->_seqi=-1;
    {int _i; for(_i=0;_i<8;_i++) x->v0_src0_up[_i]=0.0;}
    al_svf_init(&x->v0_st0);
    al_fdnrev_init(&x->v0_st1);
    al_chorus_init(&x->v0_st3);
    al_chorus_init(&x->v0_st4);
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_xsnap_ep02000_10_tilde_setup(void)
{
    alien_xsnap_ep02000_10_class = class_new(gensym("alien_xsnap_ep02000_10~"),
        (t_newmethod)alien_xsnap_ep02000_10_new, 0, sizeof(t_alien_xsnap_ep02000_10), CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_xsnap_ep02000_10_class, t_alien_xsnap_ep02000_10, x_f);
    class_addmethod(alien_xsnap_ep02000_10_class, (t_method)alien_xsnap_ep02000_10_dsp, gensym("dsp"), A_CANT, 0);
}
