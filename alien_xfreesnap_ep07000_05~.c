/* alien_xfreesnap_ep07000_05~ -- auto-bred specimen (recipe genome; do not hand-edit)
 * source=crackle  stages=['bitcrush', 'fshift', 'cheby', 'modalbody', 'svf']
 * inlet0 freq Hz | inlet1 param 0..100 (wraps) | outlet0 signal
 */
#include "m_pd.h"
#include "alien.h"
#include <string.h>

static t_class *alien_xfreesnap_ep07000_05_class;
static const double v0_st2_w[5]={-0.325,-0.091,-0.316,0.695,-0.547};

typedef struct _alien_xfreesnap_ep07000_05 {
    t_object x_obj;
    t_float  x_f;
    double   x_sr;
    al_rng    rng;
    al_wander wnd;
    al_dc     dc;
    al_phasor _lfo; al_sh _sh; al_env _env; al_v3 _cha; double _chaacc, _chav, _fbprev; double _vbus[3]; double _ofbus; double _seqph, _seqenv; int _seqi;
    al_bq v0_src0_b;
    al_hilbert v0_st1_h; double v0_st1_op, v0_st1_fb;
    al_modbank v0_st3; double v0_st3_r[6];
    al_svf v0_st4;
    al_modbank v1_src0; double v1_src0_r[6]; al_phasor v1_src0_x; double v1_src0_pv;
    al_bq v1_st1;
    al_fdnrev v1_st2;
} t_alien_xfreesnap_ep07000_05;

static t_int *alien_xfreesnap_ep07000_05_perform(t_int *w)
{
    t_alien_xfreesnap_ep07000_05 *x = (t_alien_xfreesnap_ep07000_05 *)(w[1]);
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
        al_wander_tick(&x->wnd, &x->rng, 0.759, sr);
        double w0=al_wander_val(&x->wnd,0), w1=al_wander_val(&x->wnd,1), w2=al_wander_val(&x->wnd,2), w3=al_wander_val(&x->wnd,3), w4=al_wander_val(&x->wnd,4), w5=al_wander_val(&x->wnd,5), w6=al_wander_val(&x->wnd,6), w7=al_wander_val(&x->wnd,7);
        double lfo = al_sin01(al_phasor_tick(&x->_lfo, 4.2669, sr))*0.5+0.5;
        double shv = al_sh_tick(&x->_sh, &x->rng, 24.5628, sr)*0.5+0.5;
        x->_chaacc += 0.00455; if (x->_chaacc>=1.0){ x->_chaacc-=floor(x->_chaacc); x->_chav=al_map_logistic(x->_chav, 4); }
        double cha = x->_chav;
        double env = x->_env.e;
        double fb  = x->_fbprev;
        static const double _seqtab[12] = {0.389,0.083,0.818,0.441,0.349,0.927,0.709,0.728,0.436,0.267,0.151,0.936};
        x->_seqph += (0.99)/sr; if (x->_seqph >= 12.0) x->_seqph -= 12.0;
        int _si = (int)x->_seqph; if(_si<0)_si=0; if(_si>=12)_si=11;
        if (_si != x->_seqi){ x->_seqi=_si; x->_seqenv=1.0; }
        x->_seqenv *= exp(-1000.0/((1095.54)*sr));
        double seq = _seqtab[_si];
        double seqenv = x->_seqenv;
        (void)(lfo+shv+cha+env+fb+seq+seqenv);
        double v0=x->_vbus[0], v1=x->_vbus[1], v2=x->_vbus[2], ofb=x->_ofbus; (void)(v0+v1+v2+ofb);
        /* voice 0 source: crackle */
        s = 0.0;
        { double _d = al_dust(&x->rng, al_clamp((209.676)+(w2)*(136.337),(45),(700)), sr);
          s = al_reson(&x->v0_src0_b, _d, al_clamp(f0*al_clamp((1.8128)+(0.0)*(0),(0.5),(12)),20.0,sr*0.45), al_clamp((30.6218)+(pn)*(17.5574),(5),(55)), sr) * 6.0 + _d*0.35; }
        /* voice 0 stage 0: bitcrush */
        s = al_bitcrush(s, al_clamp((14.3783)+(pn)*(3.875),(3),(16)));
        /* voice 0 stage 1: fshift */
        { double _I,_Q; al_hilbert_tick(&x->v0_st1_h, s + x->v0_st1_fb*al_clamp((0.2444)+(w1)*(0.1792),(0),(0.85)), &_I,&_Q);
          x->v0_st1_op += (al_clamp((-30.8905)+(ofb)*(464.788),(-1500),(1500)))/sr; x->v0_st1_op -= floor(x->v0_st1_op);
          double _c=cos(AL_2PI*x->v0_st1_op), _sq=sin(AL_2PI*x->v0_st1_op);
          double _ssb=_I*_c - _Q*_sq; x->v0_st1_fb=al_clamp(_ssb,-1.0,1.0);
          s = al_lerp(s, _ssb, al_clamp((0.2628)+(lfo)*(0.1627),(0.2),(1))); }
        /* voice 0 stage 2: cheby */
        s = al_cheby(al_softclip(s*al_clamp((0.7908)+(w4)*(0.5446),(0.1),(1.2))), v0_st2_w, 5);
        /* voice 0 stage 3: modalbody */
        { double _b = al_modbank_tick(&x->v0_st3, s, al_clamp(f0*al_clamp((0.8973)+(w7)*(0.184),(0.25),(4)),20.0,sr*0.45), al_clamp((39.7408)+(w2)*(18.8118),(4),(60)), sr) * al_clamp((4.8112)+(env)*(0.5103),(1),(8));
          s = al_lerp(s, al_softclip(_b), al_clamp((0.6792)+(w5)*(0.3864),(0.2),(1))); }
        /* voice 0 stage 4: svf */
        s = al_svf_lp(&x->v0_st4, s, al_clamp((80)*pow(2.0,(4.0483)+(w3)*(1.2721)),(60),(15000)), al_clamp((0.1628)+(0.0)*(0),(0),(0.9)), sr);
        double _vo0 = al_softclip(s);
        /* voice 1 source: modal */
        s = 0.0;
        { double _ph=al_phasor_tick(&x->v1_src0_x, f0*0.25, sr);
          double _exc=(_ph < x->v1_src0_pv)?1.0:0.0; x->v1_src0_pv=_ph;
          s = al_modbank_tick(&x->v1_src0, _exc, f0, al_clamp((19.2491)+(pn)*(6.6884),(10),(70)), sr) * 1.5;
          s += al_clamp((0.5442)+(w2)*(0.2649),(0.12),(0.5))*al_sin01(al_wrap01(_ph)); }
        /* voice 1 stage 0: drive */
        s = al_drive(s, al_clamp((0.3537)+(seqenv)*(0.3801),(0),(0.85)));
        /* voice 1 stage 1: reson */
        s = al_lerp(s, al_reson(&x->v1_st1, s, al_clamp(f0*al_clamp((1.3132)+(cha)*(3.3538),(0.5),(8)),20.0,sr*0.45), al_clamp((22.8441)+(cha)*(2.5795),(2),(45)), sr), al_clamp((0.4336)+(0.0)*(0),(0.2),(1)));
        /* voice 1 stage 2: fdn */
        s = al_lerp(s, al_fdnrev_tick(&x->v1_st2, s, al_clamp((0.5084)+(pn)*(0.4684),(0.1),(1)), al_clamp((0.3506)+(env)*(0.21),(0),(0.8)), al_clamp((0.4059)+(w3)*(0.3631),(0),(0.85))), al_clamp((0.4127)+(v0)*(0.2475),(0.1),(0.6)));
        double _vo1 = al_softclip(s);
        s = al_lerp(_vo0, _vo1, al_clamp((0.3821)+(0.0)*(0),(0),(1)));
        x->_vbus[0] = _vo0;
        x->_vbus[1] = _vo1;
        x->_vbus[2] = 0.0;
        s += al_clamp(ofb * (al_clamp((0.1704)+(v2)*(0.1137),(0),(0.6))), -0.9, 0.9);
        x->_ofbus = al_softclip(s);
        s = al_dc_tick(&x->dc, s);
        if (!(s==s)) s = 0.0;
        al_env_tick(&x->_env, s, 0.08188, 0.06595);
        { double _ov = al_softclip(s); x->_fbprev = fabs(_ov);
           *out++ = (t_sample)(_ov * 0.6); }
    }
    return (w + 6);
}

static void alien_xfreesnap_ep07000_05_dsp(t_alien_xfreesnap_ep07000_05 *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    dsp_add(alien_xfreesnap_ep07000_05_perform, 5, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

static void *alien_xfreesnap_ep07000_05_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_xfreesnap_ep07000_05 *x = (t_alien_xfreesnap_ep07000_05 *)pd_new(alien_xfreesnap_ep07000_05_class);
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    al_rng_seed(&x->rng, 376813324u);
    al_wander_init(&x->wnd, &x->rng, 8);
    al_dc_init(&x->dc);
    al_phasor_init(&x->_lfo); al_sh_init(&x->_sh); al_env_init(&x->_env);
    x->_cha.x=x->_cha.y=x->_cha.z=0.1; x->_chaacc=0.0; x->_chav=0.5; x->_fbprev=0.0;
    x->_vbus[0]=x->_vbus[1]=x->_vbus[2]=0.0; x->_ofbus=0.0;
    x->_seqph=0.0; x->_seqenv=0.0; x->_seqi=-1;
    al_bq_init(&x->v0_src0_b);
    al_hilbert_init(&x->v0_st1_h); x->v0_st1_op=0.0; x->v0_st1_fb=0.0;
    { static const double _mr[6]={1.0,2.76,5.40,8.93,13.34,18.64}; int _i; for(_i=0;_i<6;_i++) x->v0_st3_r[_i]=_mr[_i]; al_modbank_init(&x->v0_st3,6,x->v0_st3_r); }
    al_svf_init(&x->v0_st4);
    { static const double _mr[6]={1.0,2.76,5.40,8.93,13.34,18.64}; int _i; for(_i=0;_i<6;_i++) x->v1_src0_r[_i]=_mr[_i];
      al_modbank_init(&x->v1_src0, 6, x->v1_src0_r); al_phasor_init(&x->v1_src0_x); x->v1_src0_pv=0.0; }
    al_bq_init(&x->v1_st1);
    al_fdnrev_init(&x->v1_st2);
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_xfreesnap_ep07000_05_tilde_setup(void)
{
    alien_xfreesnap_ep07000_05_class = class_new(gensym("alien_xfreesnap_ep07000_05~"),
        (t_newmethod)alien_xfreesnap_ep07000_05_new, 0, sizeof(t_alien_xfreesnap_ep07000_05), CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_xfreesnap_ep07000_05_class, t_alien_xfreesnap_ep07000_05, x_f);
    class_addmethod(alien_xfreesnap_ep07000_05_class, (t_method)alien_xfreesnap_ep07000_05_dsp, gensym("dsp"), A_CANT, 0);
}
