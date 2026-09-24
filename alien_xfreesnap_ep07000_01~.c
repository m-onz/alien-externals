/* alien_xfreesnap_ep07000_01~ -- auto-bred specimen (recipe genome; do not hand-edit)
 * source=logic  stages=['formants']
 * inlet0 freq Hz | inlet1 param 0..100 (wraps) | outlet0 signal
 */
#include "m_pd.h"
#include "alien.h"
#include <string.h>

static t_class *alien_xfreesnap_ep07000_01_class;


typedef struct _alien_xfreesnap_ep07000_01 {
    t_object x_obj;
    t_float  x_f;
    double   x_sr;
    al_rng    rng;
    al_wander wnd;
    al_dc     dc;
    al_phasor _lfo; al_sh _sh; al_env _env; al_v3 _cha; double _chaacc, _chav, _fbprev; double _vbus[3]; double _ofbus; double _seqph, _seqenv; int _seqi;
    unsigned v0_src0_reg; double v0_src0_ck;
    al_formants v0_st0;
    al_neuron v1_src0_n; al_modbank v1_src0_b; double v1_src0_r[6];
    al_chorus v1_st0;
    al_modbank v1_st1; double v1_st1_r[6];
    al_fdnrev v1_st2;
} t_alien_xfreesnap_ep07000_01;

static t_int *alien_xfreesnap_ep07000_01_perform(t_int *w)
{
    t_alien_xfreesnap_ep07000_01 *x = (t_alien_xfreesnap_ep07000_01 *)(w[1]);
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
        al_wander_tick(&x->wnd, &x->rng, 3.276, sr);
        double w0=al_wander_val(&x->wnd,0), w1=al_wander_val(&x->wnd,1), w2=al_wander_val(&x->wnd,2), w3=al_wander_val(&x->wnd,3), w4=al_wander_val(&x->wnd,4);
        double lfo = al_sin01(al_phasor_tick(&x->_lfo, 1.263, sr))*0.5+0.5;
        double shv = al_sh_tick(&x->_sh, &x->rng, 10.8652, sr)*0.5+0.5;
        x->_chaacc += 0.01362; if (x->_chaacc>=1.0){ x->_chaacc-=floor(x->_chaacc); x->_chav=al_map_logistic(x->_chav, 3.7732); }
        double cha = x->_chav;
        double env = x->_env.e;
        double fb  = x->_fbprev;
        static const double _seqtab[9] = {0.286,0.276,0.987,0.643,0.839,0.225,0.458,0.37,0.438};
        x->_seqph += (6.018)/sr; if (x->_seqph >= 9.0) x->_seqph -= 9.0;
        int _si = (int)x->_seqph; if(_si<0)_si=0; if(_si>=9)_si=8;
        if (_si != x->_seqi){ x->_seqi=_si; x->_seqenv=1.0; }
        x->_seqenv *= exp(-1000.0/((42.8131)*sr));
        double seq = _seqtab[_si];
        double seqenv = x->_seqenv;
        (void)(lfo+shv+cha+env+fb+seq+seqenv);
        double v0=x->_vbus[0], v1=x->_vbus[1], v2=x->_vbus[2], ofb=x->_ofbus; (void)(v0+v1+v2+ofb);
        /* voice 0 source: logic */
        s = 0.0;
        { x->v0_src0_ck += al_clamp(f0*al_clamp((293.184)+(w1)*(83.1541),(2),(400)),1.0,sr*0.45)/sr;
          while(x->v0_src0_ck>=1.0){ x->v0_src0_ck-=1.0; unsigned _b=__builtin_parity(x->v0_src0_reg & 51004u);
            x->v0_src0_reg=((x->v0_src0_reg>>1)|(_b<<15)) & 0xFFFFu; }
          s = ((double)(x->v0_src0_reg & 0xFF)/127.5 - 1.0); }
        /* voice 0 stage 0: formants */
        s = al_lerp(s, al_formants_tick(&x->v0_st0, s, al_clamp((0.8458)+(seqenv)*(0.3644),(0),(1)), al_clamp((8.6776)+(seqenv)*(1.916),(2),(22)), sr), al_clamp((0.5264)+(w1)*(0.2188),(0.2),(1)));
        double _vo0 = al_softclip(s);
        /* voice 1 source: nseq */
        s = 0.0;
        { double _fire = al_neuron_tick(&x->v1_src0_n, al_clamp((0.013)+(shv)*(0.0473),(0.002),(0.03)), al_clamp((0.0001)+(env)*(0.0466),(1e-05),(0.0009)), 0.0);
          s = al_modbank_tick(&x->v1_src0_b, _fire, f0, al_clamp((17.3054)+(cha)*(2.2155),(8),(32)), sr) * 14.0; }
        /* voice 1 stage 0: chorus */
        s = al_chorus_tick(&x->v1_st0, s, al_clamp((2.4674)+(v0)*(0.5449),(0),(9)), al_clamp((0.801)+(0.0)*(0),(0.05),(2.5)), sr);
        /* voice 1 stage 1: modalbody */
        { double _b = al_modbank_tick(&x->v1_st1, s, al_clamp(f0*al_clamp((1.9007)+(fb)*(1.9207),(0.25),(4)),20.0,sr*0.45), al_clamp((22.4146)+(w0)*(16.1502),(4),(60)), sr) * al_clamp((4.4159)+(w1)*(1.9313),(1),(8));
          s = al_lerp(s, al_softclip(_b), al_clamp((0.3165)+(pn)*(0.4458),(0.2),(1))); }
        /* voice 1 stage 2: fdn */
        s = al_lerp(s, al_fdnrev_tick(&x->v1_st2, s, al_clamp((0.8347)+(lfo)*(0.2169),(0.1),(1)), al_clamp((0.7302)+(w0)*(0.245),(0),(0.8)), al_clamp((0.7536)+(seqenv)*(0.31),(0),(0.85))), al_clamp((0.2039)+(ofb)*(0.289),(0.1),(0.6)));
        double _vo1 = al_softclip(s);
        s = al_lerp(_vo0, _vo1, al_clamp((0.4626)+(shv)*(0.3008),(0),(1)));
        x->_vbus[0] = _vo0;
        x->_vbus[1] = _vo1;
        x->_vbus[2] = 0.0;
        s += al_clamp(ofb * (al_clamp((-0.0169)+(lfo)*(0.2513),(0),(0.6))), -0.9, 0.9);
        x->_ofbus = al_softclip(s);
        s = al_dc_tick(&x->dc, s);
        if (!(s==s)) s = 0.0;
        al_env_tick(&x->_env, s, 0.90643, 0.1098);
        { double _ov = al_softclip(s); x->_fbprev = fabs(_ov);
           *out++ = (t_sample)(_ov * 0.6); }
    }
    return (w + 6);
}

static void alien_xfreesnap_ep07000_01_dsp(t_alien_xfreesnap_ep07000_01 *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    dsp_add(alien_xfreesnap_ep07000_01_perform, 5, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

static void *alien_xfreesnap_ep07000_01_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_xfreesnap_ep07000_01 *x = (t_alien_xfreesnap_ep07000_01 *)pd_new(alien_xfreesnap_ep07000_01_class);
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    al_rng_seed(&x->rng, 1039861179u);
    al_wander_init(&x->wnd, &x->rng, 5);
    al_dc_init(&x->dc);
    al_phasor_init(&x->_lfo); al_sh_init(&x->_sh); al_env_init(&x->_env);
    x->_cha.x=x->_cha.y=x->_cha.z=0.1; x->_chaacc=0.0; x->_chav=0.5; x->_fbprev=0.0;
    x->_vbus[0]=x->_vbus[1]=x->_vbus[2]=0.0; x->_ofbus=0.0;
    x->_seqph=0.0; x->_seqenv=0.0; x->_seqi=-1;
    x->v0_src0_reg=0xACE1u; x->v0_src0_ck=0.0;
    al_formants_init(&x->v0_st0);
    { static const double _nr[6]={1.0,2.32,4.25,6.63,9.38,12.44}; int _i; for(_i=0;_i<6;_i++) x->v1_src0_r[_i]=_nr[_i];
      al_neuron_init(&x->v1_src0_n); al_modbank_init(&x->v1_src0_b, 6, x->v1_src0_r); }
    al_chorus_init(&x->v1_st0);
    { static const double _mr[6]={1.0,2.76,5.40,8.93,13.34,18.64}; int _i; for(_i=0;_i<6;_i++) x->v1_st1_r[_i]=_mr[_i]; al_modbank_init(&x->v1_st1,6,x->v1_st1_r); }
    al_fdnrev_init(&x->v1_st2);
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_xfreesnap_ep07000_01_tilde_setup(void)
{
    alien_xfreesnap_ep07000_01_class = class_new(gensym("alien_xfreesnap_ep07000_01~"),
        (t_newmethod)alien_xfreesnap_ep07000_01_new, 0, sizeof(t_alien_xfreesnap_ep07000_01), CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_xfreesnap_ep07000_01_class, t_alien_xfreesnap_ep07000_01, x_f);
    class_addmethod(alien_xfreesnap_ep07000_01_class, (t_method)alien_xfreesnap_ep07000_01_dsp, gensym("dsp"), A_CANT, 0);
}
