/* alien_xfreesnap_ep05000_08~ -- auto-bred specimen (recipe genome; do not hand-edit)
 * source=noise  stages=['ring', 'bitcrush', 'svf']
 * inlet0 freq Hz | inlet1 param 0..100 (wraps) | outlet0 signal
 */
#include "m_pd.h"
#include "alien.h"
#include <string.h>

static t_class *alien_xfreesnap_ep05000_08_class;


typedef struct _alien_xfreesnap_ep05000_08 {
    t_object x_obj;
    t_float  x_f;
    double   x_sr;
    al_rng    rng;
    al_wander wnd;
    al_dc     dc;
    al_phasor _lfo; al_sh _sh; al_env _env; al_v3 _cha; double _chaacc, _chav, _fbprev; double _vbus[3]; double _ofbus; double _seqph, _seqenv; int _seqi;
    al_pink v0_src0_p; al_bq v0_src0_b;
    al_phasor v0_st0;
    al_svf v0_st2;
    unsigned v1_src0_reg; double v1_src0_ck;
    double v1_st0_b[4096]; int v1_st0_wr;
    al_svf v1_st1;
    al_formants v1_st2;
    al_svf v1_st3;
} t_alien_xfreesnap_ep05000_08;

static t_int *alien_xfreesnap_ep05000_08_perform(t_int *w)
{
    t_alien_xfreesnap_ep05000_08 *x = (t_alien_xfreesnap_ep05000_08 *)(w[1]);
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
        al_wander_tick(&x->wnd, &x->rng, 0.906, sr);
        double w0=al_wander_val(&x->wnd,0), w1=al_wander_val(&x->wnd,1), w2=al_wander_val(&x->wnd,2), w3=al_wander_val(&x->wnd,3), w4=al_wander_val(&x->wnd,4);
        double lfo = al_sin01(al_phasor_tick(&x->_lfo, 6.36144, sr))*0.5+0.5;
        double shv = al_sh_tick(&x->_sh, &x->rng, 6.7586, sr)*0.5+0.5;
        x->_chaacc += 0.00626; if (x->_chaacc>=1.0){ x->_chaacc-=floor(x->_chaacc); x->_chav=al_map_logistic(x->_chav, 4); }
        double cha = x->_chav;
        double env = x->_env.e;
        double fb  = x->_fbprev;
        static const double _seqtab[6] = {0.048,0.828,0.156,0.67,0.754,0.115};
        x->_seqph += (0.652)/sr; if (x->_seqph >= 6.0) x->_seqph -= 6.0;
        int _si = (int)x->_seqph; if(_si<0)_si=0; if(_si>=6)_si=5;
        if (_si != x->_seqi){ x->_seqi=_si; x->_seqenv=1.0; }
        x->_seqenv *= exp(-1000.0/((111.1)*sr));
        double seq = _seqtab[_si];
        double seqenv = x->_seqenv;
        (void)(lfo+shv+cha+env+fb+seq+seqenv);
        double v0=x->_vbus[0], v1=x->_vbus[1], v2=x->_vbus[2], ofb=x->_ofbus; (void)(v0+v1+v2+ofb);
        /* voice 0 source: noise */
        s = 0.0;
        { double _n = al_white(&x->rng);
          double _bp = al_reson(&x->v0_src0_b, _n, al_clamp(f0*al_clamp((3.2284)+(w2)*(2.9032),(0.25),(10)),20.0,sr*0.45), al_clamp((19.2451)+(w4)*(2.7412),(1.5),(45)), sr);
          s = al_lerp(_n*0.7, _bp*3.5, al_clamp((0.4867)+(env)*(0.5398),(0),(0.82))); }
        /* voice 0 stage 0: ring */
        { double _m = al_sin01(al_phasor_tick(&x->v0_st0, al_clamp(f0*al_clamp((2.7263)+(v1)*(7.0768),(0.125),(14)),20.0,sr*0.45), sr));
          s = al_ringmod(s, _m, al_clamp((0.8608)+(v0)*(0.3896),(0),(1))); }
        /* voice 0 stage 1: bitcrush */
        s = al_bitcrush(s, al_clamp((4.228)+(shv)*(1.1243),(3),(16)));
        /* voice 0 stage 2: svf */
        s = al_svf_lp(&x->v0_st2, s, al_clamp((80)*pow(2.0,(5.2537)+(pn)*(0.1749)),(60),(15000)), al_clamp((0.1426)+(seqenv)*(0.2866),(0),(0.9)), sr);
        double _vo0 = al_softclip(s);
        /* voice 1 source: logic */
        s = 0.0;
        { x->v1_src0_ck += al_clamp(f0*al_clamp((21.6472)+(pn)*(19.7574),(2),(400)),1.0,sr*0.45)/sr;
          while(x->v1_src0_ck>=1.0){ x->v1_src0_ck-=1.0; unsigned _b=__builtin_parity(x->v1_src0_reg & 53256u);
            x->v1_src0_reg=((x->v1_src0_reg>>1)|(_b<<15)) & 0xFFFFu; }
          s = ((double)(x->v1_src0_reg & 0xFF)/127.5 - 1.0); }
        /* voice 1 stage 0: comb */
        s = al_comb(x->v1_st0_b, 4095, &x->v1_st0_wr, s, al_clamp((681.393)+(w3)*(50.9382),(20),(2000)), al_clamp((0.4689)+(0.0)*(0),(0),(0.9)));
        /* voice 1 stage 1: multiband */
        { double _lo = al_svf_lp(&x->v1_st1, s, al_clamp((120)*pow(2.0,(4.9043)+(pn)*(2.1304)),(200),(6000)), 0.3, sr); double _hi = s - _lo;
          _lo = al_drive(_lo, al_clamp((0.2031)+(pn)*(0.1665),(0),(0.9)))*al_clamp((1.158)+(pn)*(0.2596),(0.2),(1.6)); _hi = al_fold(_hi, al_clamp((0.4616)+(w1)*(0.7404),(0),(1.2)))*al_clamp((0.8223)+(pn)*(0.2209),(0.2),(1.5));
          s = al_softclip(_lo + _hi); }
        /* voice 1 stage 2: formants */
        s = al_lerp(s, al_formants_tick(&x->v1_st2, s, al_clamp((0.6179)+(shv)*(0.2866),(0),(1)), al_clamp((8.1371)+(w3)*(4.9752),(2),(22)), sr), al_clamp((0.8292)+(env)*(0.4889),(0.2),(1)));
        /* voice 1 stage 3: bpf */
        s = al_lerp(s, al_svf_bp(&x->v1_st3, s, al_clamp((80)*pow(2.0,(4.4129)+(w2)*(0.5653)),(80),(15000)), al_clamp((0.2697)+(v2)*(0.2008),(0.1),(0.95)), sr), al_clamp((0.7953)+(w2)*(0.2355),(0.2),(1)));
        double _vo1 = al_softclip(s);
        s = (_vo0 + _vo1) * 0.5;
        x->_vbus[0] = _vo0;
        x->_vbus[1] = _vo1;
        x->_vbus[2] = 0.0;
        s += al_clamp(ofb * (al_clamp((0.023)+(seqenv)*(0.378),(0),(0.6))), -0.9, 0.9);
        x->_ofbus = al_softclip(s);
        s = al_dc_tick(&x->dc, s);
        if (!(s==s)) s = 0.0;
        al_env_tick(&x->_env, s, 0.03946, 0.04488);
        { double _ov = al_softclip(s); x->_fbprev = fabs(_ov);
           *out++ = (t_sample)(_ov * 0.6); }
    }
    return (w + 6);
}

static void alien_xfreesnap_ep05000_08_dsp(t_alien_xfreesnap_ep05000_08 *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    dsp_add(alien_xfreesnap_ep05000_08_perform, 5, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

static void *alien_xfreesnap_ep05000_08_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_xfreesnap_ep05000_08 *x = (t_alien_xfreesnap_ep05000_08 *)pd_new(alien_xfreesnap_ep05000_08_class);
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    al_rng_seed(&x->rng, 382633713u);
    al_wander_init(&x->wnd, &x->rng, 5);
    al_dc_init(&x->dc);
    al_phasor_init(&x->_lfo); al_sh_init(&x->_sh); al_env_init(&x->_env);
    x->_cha.x=x->_cha.y=x->_cha.z=0.1; x->_chaacc=0.0; x->_chav=0.5; x->_fbprev=0.0;
    x->_vbus[0]=x->_vbus[1]=x->_vbus[2]=0.0; x->_ofbus=0.0;
    x->_seqph=0.0; x->_seqenv=0.0; x->_seqi=-1;
    al_pink_init(&x->v0_src0_p); al_bq_init(&x->v0_src0_b);
    al_phasor_init(&x->v0_st0);
    al_svf_init(&x->v0_st2);
    x->v1_src0_reg=0xACE1u; x->v1_src0_ck=0.0;
    memset(x->v1_st0_b,0,sizeof(x->v1_st0_b)); x->v1_st0_wr=0;
    al_svf_init(&x->v1_st1);
    al_formants_init(&x->v1_st2);
    al_svf_init(&x->v1_st3);
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_xfreesnap_ep05000_08_tilde_setup(void)
{
    alien_xfreesnap_ep05000_08_class = class_new(gensym("alien_xfreesnap_ep05000_08~"),
        (t_newmethod)alien_xfreesnap_ep05000_08_new, 0, sizeof(t_alien_xfreesnap_ep05000_08), CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_xfreesnap_ep05000_08_class, t_alien_xfreesnap_ep05000_08, x_f);
    class_addmethod(alien_xfreesnap_ep05000_08_class, (t_method)alien_xfreesnap_ep05000_08_dsp, gensym("dsp"), A_CANT, 0);
}
