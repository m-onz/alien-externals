/* alien_xfreesnap_ep06000_07~ -- auto-bred specimen (recipe genome; do not hand-edit)
 * source=logic  stages=['multiband', 'cheby', 'bpf', 'multiband', 'formants']
 * inlet0 freq Hz | inlet1 param 0..100 (wraps) | outlet0 signal
 */
#include "m_pd.h"
#include "alien.h"
#include <string.h>

static t_class *alien_xfreesnap_ep06000_07_class;
static const double v0_st1_w[5]={-0.101,-0.785,-0.688,-0.368,0.37};

typedef struct _alien_xfreesnap_ep06000_07 {
    t_object x_obj;
    t_float  x_f;
    double   x_sr;
    al_rng    rng;
    al_wander wnd;
    al_dc     dc;
    al_phasor _lfo; al_sh _sh; al_env _env; al_v3 _cha; double _chaacc, _chav, _fbprev; double _vbus[3]; double _ofbus; double _seqph, _seqenv; int _seqi;
    unsigned v0_src0_reg; double v0_src0_ck;
    al_svf v0_st0;
    al_svf v0_st2;
    al_svf v0_st3;
    al_formants v0_st4;
    al_bq v1_src0_b;
    al_phasor v1_st0;
    al_chorus v1_st1;
} t_alien_xfreesnap_ep06000_07;

static t_int *alien_xfreesnap_ep06000_07_perform(t_int *w)
{
    t_alien_xfreesnap_ep06000_07 *x = (t_alien_xfreesnap_ep06000_07 *)(w[1]);
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
        al_wander_tick(&x->wnd, &x->rng, 0.964, sr);
        double w0=al_wander_val(&x->wnd,0), w1=al_wander_val(&x->wnd,1), w2=al_wander_val(&x->wnd,2), w3=al_wander_val(&x->wnd,3), w4=al_wander_val(&x->wnd,4), w5=al_wander_val(&x->wnd,5), w6=al_wander_val(&x->wnd,6);
        double lfo = al_sin01(al_phasor_tick(&x->_lfo, 3.65006, sr))*0.5+0.5;
        double shv = al_sh_tick(&x->_sh, &x->rng, 8.7071, sr)*0.5+0.5;
        x->_chaacc += 0.0209; if (x->_chaacc>=1.0){ x->_chaacc-=floor(x->_chaacc); x->_chav=al_map_logistic(x->_chav, 4); }
        double cha = x->_chav;
        double env = x->_env.e;
        double fb  = x->_fbprev;
        static const double _seqtab[9] = {0.142,0.773,0.021,0.486,0.718,0.783,0.123,0.237,0.229};
        x->_seqph += (8.382)/sr; if (x->_seqph >= 9.0) x->_seqph -= 9.0;
        int _si = (int)x->_seqph; if(_si<0)_si=0; if(_si>=9)_si=8;
        if (_si != x->_seqi){ x->_seqi=_si; x->_seqenv=1.0; }
        x->_seqenv *= exp(-1000.0/((199.122)*sr));
        double seq = _seqtab[_si];
        double seqenv = x->_seqenv;
        (void)(lfo+shv+cha+env+fb+seq+seqenv);
        double v0=x->_vbus[0], v1=x->_vbus[1], v2=x->_vbus[2], ofb=x->_ofbus; (void)(v0+v1+v2+ofb);
        /* voice 0 source: logic */
        s = 0.0;
        { x->v0_src0_ck += al_clamp(f0*al_clamp((78.7586)+(w1)*(102.188),(2),(400)),1.0,sr*0.45)/sr;
          while(x->v0_src0_ck>=1.0){ x->v0_src0_ck-=1.0; unsigned _b=__builtin_parity(x->v0_src0_reg & 47348u);
            x->v0_src0_reg=((x->v0_src0_reg>>1)|(_b<<15)) & 0xFFFFu; }
          s = ((double)(x->v0_src0_reg & 0xFF)/127.5 - 1.0); }
        /* voice 0 stage 0: multiband */
        { double _lo = al_svf_lp(&x->v0_st0, s, al_clamp((120)*pow(2.0,(4.382)+(v2)*(0.6288)),(200),(6000)), 0.3, sr); double _hi = s - _lo;
          _lo = al_drive(_lo, al_clamp((0.3387)+(w6)*(0.4771),(0),(0.9)))*al_clamp((1.0876)+(pn)*(0.3719),(0.2),(1.6)); _hi = al_fold(_hi, al_clamp((0.5514)+(w6)*(0.249),(0),(1.2)))*al_clamp((0.6029)+(ofb)*(0.5352),(0.2),(1.5));
          s = al_softclip(_lo + _hi); }
        /* voice 0 stage 1: cheby */
        s = al_cheby(al_softclip(s*al_clamp((0.3857)+(w1)*(0.222),(0.1),(1.2))), v0_st1_w, 5);
        /* voice 0 stage 2: bpf */
        s = al_lerp(s, al_svf_bp(&x->v0_st2, s, al_clamp((80)*pow(2.0,(3.4936)+(pn)*(1.7755)),(80),(15000)), al_clamp((0.5623)+(0.0)*(0),(0.1),(0.95)), sr), al_clamp((0.9705)+(pn)*(0.3294),(0.2),(1)));
        /* voice 0 stage 3: multiband */
        { double _lo = al_svf_lp(&x->v0_st3, s, al_clamp((120)*pow(2.0,(6.1784)+(w1)*(0.6962)),(200),(6000)), 0.3, sr); double _hi = s - _lo;
          _lo = al_drive(_lo, al_clamp((0.3539)+(w2)*(0.5416),(0),(0.9)))*al_clamp((0.7108)+(w0)*(0.319),(0.2),(1.6)); _hi = al_fold(_hi, al_clamp((0.5389)+(v1)*(0.5828),(0),(1.2)))*al_clamp((1.0312)+(0.0)*(0),(0.2),(1.5));
          s = al_softclip(_lo + _hi); }
        /* voice 0 stage 4: formants */
        s = al_lerp(s, al_formants_tick(&x->v0_st4, s, al_clamp((0.2951)+(seq)*(0.5281),(0),(1)), al_clamp((7.597)+(w2)*(9.4731),(2),(22)), sr), al_clamp((0.8568)+(pn)*(0.3674),(0.2),(1)));
        double _vo0 = al_softclip(s);
        /* voice 1 source: crackle */
        s = 0.0;
        { double _d = al_dust(&x->rng, al_clamp((392.925)+(pn)*(201.563),(45),(700)), sr);
          s = al_reson(&x->v1_src0_b, _d, al_clamp(f0*al_clamp((4.0615)+(w0)*(4.0398),(0.5),(12)),20.0,sr*0.45), al_clamp((19.8582)+(w5)*(22.278),(5),(55)), sr) * 6.0 + _d*0.35; }
        /* voice 1 stage 0: ring */
        { double _m = al_sin01(al_phasor_tick(&x->v1_st0, al_clamp(f0*al_clamp((3.8872)+(pn)*(6.2095),(0.125),(14)),20.0,sr*0.45), sr));
          s = al_ringmod(s, _m, al_clamp((0.7995)+(0.0)*(0),(0),(1))); }
        /* voice 1 stage 1: chorus */
        s = al_chorus_tick(&x->v1_st1, s, al_clamp((5.7471)+(0.0)*(0),(0),(9)), al_clamp((0.1527)+(w2)*(0.6414),(0.05),(2.5)), sr);
        double _vo1 = al_softclip(s);
        s = (_vo0 + _vo1) * 0.5;
        x->_vbus[0] = _vo0;
        x->_vbus[1] = _vo1;
        x->_vbus[2] = 0.0;
        s += al_clamp(ofb * (al_clamp((0.3325)+(w1)*(0.258),(0),(0.6))), -0.9, 0.9);
        x->_ofbus = al_softclip(s);
        s = al_dc_tick(&x->dc, s);
        if (!(s==s)) s = 0.0;
        al_env_tick(&x->_env, s, 0.1356, 0.00351);
        { double _ov = al_softclip(s); x->_fbprev = fabs(_ov);
           *out++ = (t_sample)(_ov * 0.6); }
    }
    return (w + 6);
}

static void alien_xfreesnap_ep06000_07_dsp(t_alien_xfreesnap_ep06000_07 *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    dsp_add(alien_xfreesnap_ep06000_07_perform, 5, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

static void *alien_xfreesnap_ep06000_07_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_xfreesnap_ep06000_07 *x = (t_alien_xfreesnap_ep06000_07 *)pd_new(alien_xfreesnap_ep06000_07_class);
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    al_rng_seed(&x->rng, 69488405u);
    al_wander_init(&x->wnd, &x->rng, 7);
    al_dc_init(&x->dc);
    al_phasor_init(&x->_lfo); al_sh_init(&x->_sh); al_env_init(&x->_env);
    x->_cha.x=x->_cha.y=x->_cha.z=0.1; x->_chaacc=0.0; x->_chav=0.5; x->_fbprev=0.0;
    x->_vbus[0]=x->_vbus[1]=x->_vbus[2]=0.0; x->_ofbus=0.0;
    x->_seqph=0.0; x->_seqenv=0.0; x->_seqi=-1;
    x->v0_src0_reg=0xACE1u; x->v0_src0_ck=0.0;
    al_svf_init(&x->v0_st0);
    al_svf_init(&x->v0_st2);
    al_svf_init(&x->v0_st3);
    al_formants_init(&x->v0_st4);
    al_bq_init(&x->v1_src0_b);
    al_phasor_init(&x->v1_st0);
    al_chorus_init(&x->v1_st1);
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_xfreesnap_ep06000_07_tilde_setup(void)
{
    alien_xfreesnap_ep06000_07_class = class_new(gensym("alien_xfreesnap_ep06000_07~"),
        (t_newmethod)alien_xfreesnap_ep06000_07_new, 0, sizeof(t_alien_xfreesnap_ep06000_07), CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_xfreesnap_ep06000_07_class, t_alien_xfreesnap_ep06000_07, x_f);
    class_addmethod(alien_xfreesnap_ep06000_07_class, (t_method)alien_xfreesnap_ep06000_07_dsp, gensym("dsp"), A_CANT, 0);
}
