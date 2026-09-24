/* alien_xsnap_ep05000_09~ -- auto-bred specimen (recipe genome; do not hand-edit)
 * source=dsf  stages=['comb', 'modalbody', 'pmshape', 'ring', 'ladder']
 * inlet0 freq Hz | inlet1 param 0..100 (wraps) | outlet0 signal
 */
#include "m_pd.h"
#include "alien.h"
#include <string.h>

static t_class *alien_xsnap_ep05000_09_class;


typedef struct _alien_xsnap_ep05000_09 {
    t_object x_obj;
    t_float  x_f;
    double   x_sr;
    al_rng    rng;
    al_wander wnd;
    al_dc     dc;
    al_phasor _lfo; al_sh _sh; al_env _env; al_v3 _cha; double _chaacc, _chav, _fbprev; double _vbus[3]; double _ofbus; double _seqph, _seqenv; int _seqi;
    al_phasor v0_src0;
    double v0_st0_b[4096]; int v0_st0_wr;
    al_modbank v0_st1; double v0_st1_r[6];
    al_phasor v0_st2;
    al_phasor v0_st3;
    al_ladder v0_st4;
    al_cml v1_src0;
    al_bq v1_st0;
    double v1_st1_b[4096]; int v1_st1_wr;
    al_formants v1_st3;
} t_alien_xsnap_ep05000_09;

static t_int *alien_xsnap_ep05000_09_perform(t_int *w)
{
    t_alien_xsnap_ep05000_09 *x = (t_alien_xsnap_ep05000_09 *)(w[1]);
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
        al_wander_tick(&x->wnd, &x->rng, 4.938, sr);
        double w0=al_wander_val(&x->wnd,0), w1=al_wander_val(&x->wnd,1), w2=al_wander_val(&x->wnd,2), w3=al_wander_val(&x->wnd,3), w4=al_wander_val(&x->wnd,4);
        double lfo = al_sin01(al_phasor_tick(&x->_lfo, 5.823, sr))*0.5+0.5;
        double shv = al_sh_tick(&x->_sh, &x->rng, 12.3554, sr)*0.5+0.5;
        x->_chaacc += 0.02003; if (x->_chaacc>=1.0){ x->_chaacc-=floor(x->_chaacc); x->_chav=al_map_logistic(x->_chav, 3.7148); }
        double cha = x->_chav;
        double env = x->_env.e;
        double fb  = x->_fbprev;
        static const double _seqtab[16] = {0.87,0.961,0.749,0.537,0.265,0.373,0.231,0.102,0.515,0.135,0.706,0.956,0.979,0.068,0.492,0.304};
        x->_seqph += (2.348)/sr; if (x->_seqph >= 16.0) x->_seqph -= 16.0;
        int _si = (int)x->_seqph; if(_si<0)_si=0; if(_si>=16)_si=15;
        if (_si != x->_seqi){ x->_seqi=_si; x->_seqenv=1.0; }
        x->_seqenv *= exp(-1000.0/((78.9942)*sr));
        double seq = _seqtab[_si];
        double seqenv = x->_seqenv;
        (void)(lfo+shv+cha+env+fb+seq+seqenv);
        double v0=x->_vbus[0], v1=x->_vbus[1], v2=x->_vbus[2], ofb=x->_ofbus; (void)(v0+v1+v2+ofb);
        /* voice 0 source: dsf */
        s = 0.0;
        s = al_dsf(al_phasor_tick(&x->v0_src0,f0,sr), al_clamp((1.9564)+(v1)*(1.7918),(0.5),(6)), al_clamp((0.6116)+(shv)*(0.2151),(0),(0.92)), 20);
        /* voice 0 stage 0: comb */
        s = al_comb(x->v0_st0_b, 4095, &x->v0_st0_wr, s, al_clamp((835.383)+(v2)*(320.784),(20),(2000)), al_clamp((0.3407)+(lfo)*(0.3394),(0),(0.9)));
        /* voice 0 stage 1: modalbody */
        { double _b = al_modbank_tick(&x->v0_st1, s, al_clamp(f0*al_clamp((1.2349)+(pn)*(1.428),(0.25),(4)),20.0,sr*0.45), al_clamp((38.0515)+(w1)*(15.6235),(4),(60)), sr) * al_clamp((2.6084)+(pn)*(2.9648),(1),(8));
          s = al_lerp(s, al_softclip(_b), al_clamp((0.8033)+(pn)*(0.4867),(0.2),(1))); }
        /* voice 0 stage 2: pmshape */
        { double _c = al_phasor_tick(&x->v0_st2, al_clamp(f0*al_clamp((3.6663)+(fb)*(3.6245),(0.125),(8)),20.0,sr*0.45), sr);
          double _y = sin(AL_2PI*(_c + al_clamp((1.8401)+(w3)*(0.529),(0),(5))*s));
          s = al_lerp(s, _y, al_clamp((0.5493)+(pn)*(0.5839),(0),(1))); }
        /* voice 0 stage 3: ring */
        { double _m = al_sin01(al_phasor_tick(&x->v0_st3, al_clamp(f0*al_clamp((0.6184)+(pn)*(1.0298),(0.125),(14)),20.0,sr*0.45), sr));
          s = al_ringmod(s, _m, al_clamp((0.5313)+(ofb)*(0.4323),(0),(1))); }
        /* voice 0 stage 4: ladder */
        s = al_ladder_tick(&x->v0_st4, s, al_clamp((80)*pow(2.0,(4.5004)+(0.0)*(0)),(60),(14000)), al_clamp((0.3814)+(pn)*(0.4899),(0),(0.85)), sr);
        double _vo0 = al_softclip(s);
        /* voice 1 source: cml */
        s = 0.0;
        s = al_cml_tick(&x->v1_src0,f0,sr,al_clamp((3.6848)+(v0)*(0.1705),(3.5),(4)),al_clamp((0.369)+(w4)*(0.4288),(0),(0.6)));
        /* voice 1 stage 0: reson */
        s = al_lerp(s, al_reson(&x->v1_st0, s, al_clamp(f0*al_clamp((3.488)+(w2)*(0.9935),(0.5),(8)),20.0,sr*0.45), al_clamp((7.7917)+(seqenv)*(15.5336),(2),(45)), sr), al_clamp((0.5132)+(w3)*(0.3499),(0.2),(1)));
        /* voice 1 stage 1: comb */
        s = al_comb(x->v1_st1_b, 4095, &x->v1_st1_wr, s, al_clamp((870.636)+(w0)*(368.559),(20),(2000)), al_clamp((0.3749)+(ofb)*(0.4989),(0),(0.9)));
        /* voice 1 stage 2: drive */
        s = al_drive(s, al_clamp((0.3662)+(w3)*(0.5904),(0),(0.85)));
        /* voice 1 stage 3: formants */
        s = al_lerp(s, al_formants_tick(&x->v1_st3, s, al_clamp((0.6391)+(w1)*(0.7685),(0),(1)), al_clamp((10.9343)+(w0)*(5.5772),(2),(22)), sr), al_clamp((0.8013)+(0.0)*(0),(0.2),(1)));
        double _vo1 = al_softclip(s);
        s = al_lerp(_vo0, _vo1, al_clamp((0.0469)+(lfo)*(0.4258),(0),(1)));
        x->_vbus[0] = _vo0;
        x->_vbus[1] = _vo1;
        x->_vbus[2] = 0.0;
        s += al_clamp(ofb * (al_clamp((0.4177)+(cha)*(0.583),(0),(0.6))), -0.9, 0.9);
        x->_ofbus = al_softclip(s);
        s = al_dc_tick(&x->dc, s);
        if (!(s==s)) s = 0.0;
        al_env_tick(&x->_env, s, 0.616, 0.05168);
        { double _ov = al_softclip(s); x->_fbprev = fabs(_ov);
           *out++ = (t_sample)(_ov * 0.6); }
    }
    return (w + 6);
}

static void alien_xsnap_ep05000_09_dsp(t_alien_xsnap_ep05000_09 *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    dsp_add(alien_xsnap_ep05000_09_perform, 5, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

static void *alien_xsnap_ep05000_09_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_xsnap_ep05000_09 *x = (t_alien_xsnap_ep05000_09 *)pd_new(alien_xsnap_ep05000_09_class);
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    al_rng_seed(&x->rng, 634887161u);
    al_wander_init(&x->wnd, &x->rng, 5);
    al_dc_init(&x->dc);
    al_phasor_init(&x->_lfo); al_sh_init(&x->_sh); al_env_init(&x->_env);
    x->_cha.x=x->_cha.y=x->_cha.z=0.1; x->_chaacc=0.0; x->_chav=0.5; x->_fbprev=0.0;
    x->_vbus[0]=x->_vbus[1]=x->_vbus[2]=0.0; x->_ofbus=0.0;
    x->_seqph=0.0; x->_seqenv=0.0; x->_seqi=-1;
    al_phasor_init(&x->v0_src0);
    memset(x->v0_st0_b,0,sizeof(x->v0_st0_b)); x->v0_st0_wr=0;
    { static const double _mr[6]={1.0,2.76,5.40,8.93,13.34,18.64}; int _i; for(_i=0;_i<6;_i++) x->v0_st1_r[_i]=_mr[_i]; al_modbank_init(&x->v0_st1,6,x->v0_st1_r); }
    al_phasor_init(&x->v0_st2);
    al_phasor_init(&x->v0_st3);
    al_ladder_init(&x->v0_st4);
    al_cml_init(&x->v1_src0,&x->rng);
    al_bq_init(&x->v1_st0);
    memset(x->v1_st1_b,0,sizeof(x->v1_st1_b)); x->v1_st1_wr=0;
    al_formants_init(&x->v1_st3);
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_xsnap_ep05000_09_tilde_setup(void)
{
    alien_xsnap_ep05000_09_class = class_new(gensym("alien_xsnap_ep05000_09~"),
        (t_newmethod)alien_xsnap_ep05000_09_new, 0, sizeof(t_alien_xsnap_ep05000_09), CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_xsnap_ep05000_09_class, t_alien_xsnap_ep05000_09, x_f);
    class_addmethod(alien_xsnap_ep05000_09_class, (t_method)alien_xsnap_ep05000_09_dsp, gensym("dsp"), A_CANT, 0);
}
