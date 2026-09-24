/* alien_xsnap_ep00018_01~ -- auto-bred specimen (recipe genome; do not hand-edit)
 * source=fof  stages=['fshift', 'comb', 'ladder', 'cheby']
 * inlet0 freq Hz | inlet1 param 0..100 (wraps) | outlet0 signal
 */
#include "m_pd.h"
#include "alien.h"
#include <string.h>

static t_class *alien_xsnap_ep00018_01_class;
static const double st3_w[5]={-0.204,-0.198,0.225,-0.533,-0.985};

typedef struct _alien_xsnap_ep00018_01 {
    t_object x_obj;
    t_float  x_f;
    double   x_sr;
    al_rng    rng;
    al_wander wnd;
    al_dc     dc;
    al_phasor _lfo; al_sh _sh; al_env _env; al_v3 _cha; double _chaacc, _chav, _fbprev;
    al_phasor src0_f; double src0_gt, src0_pp;
    al_hilbert st0_h; double st0_op, st0_fb;
    double st1_b[4096]; int st1_wr;
    al_ladder st2;
} t_alien_xsnap_ep00018_01;

static t_int *alien_xsnap_ep00018_01_perform(t_int *w)
{
    t_alien_xsnap_ep00018_01 *x = (t_alien_xsnap_ep00018_01 *)(w[1]);
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
        al_wander_tick(&x->wnd, &x->rng, 1.472, sr);
        double w0=al_wander_val(&x->wnd,0), w1=al_wander_val(&x->wnd,1), w2=al_wander_val(&x->wnd,2), w3=al_wander_val(&x->wnd,3), w4=al_wander_val(&x->wnd,4), w5=al_wander_val(&x->wnd,5), w6=al_wander_val(&x->wnd,6), w7=al_wander_val(&x->wnd,7);
        double lfo = al_sin01(al_phasor_tick(&x->_lfo, 4.5231, sr))*0.5+0.5;
        double shv = al_sh_tick(&x->_sh, &x->rng, 11.7845, sr)*0.5+0.5;
        x->_chaacc += 0.00905; if (x->_chaacc>=1.0){ x->_chaacc-=floor(x->_chaacc); x->_chav=al_map_logistic(x->_chav, 3.76398); }
        double cha = x->_chav;
        double env = x->_env.e;
        double fb  = x->_fbprev;
        (void)(lfo+shv+cha+env+fb);
        /* source: fof */
        { double _ph=al_phasor_tick(&x->src0_f,f0,sr); if(_ph<x->src0_pp) x->src0_gt=0.0; x->src0_pp=_ph;
          double _ff=al_clamp(f0*al_clamp((5.7827)+(w7)*(4.3201),(0.5),(12)),20.0,sr*0.45); double _env=exp(-al_clamp((147.387)+(w4)*(54.9844),(5),(400))*x->src0_gt/sr);
          s = _env*sin(AL_2PI*_ff*x->src0_gt/sr); x->src0_gt+=1.0; }
        /* stage 0: fshift */
        { double _I,_Q; al_hilbert_tick(&x->st0_h, s + x->st0_fb*al_clamp((0.3574)+(pn)*(0.3837),(0),(0.85)), &_I,&_Q);
          x->st0_op += (al_clamp((-206.383)+(w3)*(302.932),(-1500),(1500)))/sr; x->st0_op -= floor(x->st0_op);
          double _c=cos(AL_2PI*x->st0_op), _sq=sin(AL_2PI*x->st0_op);
          double _ssb=_I*_c - _Q*_sq; x->st0_fb=al_clamp(_ssb,-1.0,1.0);
          s = al_lerp(s, _ssb, al_clamp((0.4386)+(pn)*(0.4872),(0.2),(1))); }
        /* stage 1: comb */
        s = al_comb(x->st1_b, 4095, &x->st1_wr, s, al_clamp((661.833)+(w4)*(766.186),(20),(2000)), al_clamp((0.66)+(w6)*(0.21),(0),(0.9)));
        /* stage 2: ladder */
        s = al_ladder_tick(&x->st2, s, al_clamp((80)*pow(2.0,(4.5858)+(pn)*(1.4209)),(60),(14000)), al_clamp((0.4459)+(w0)*(0.353),(0),(0.85)), sr);
        /* stage 3: cheby */
        s = al_cheby(al_softclip(s*al_clamp((0.4814)+(pn)*(0.6059),(0.1),(1.2))), st3_w, 5);
        s = al_dc_tick(&x->dc, s);
        if (!(s==s)) s = 0.0;
        al_env_tick(&x->_env, s, 0.2221, 0.01222);
        { double _ov = al_softclip(s); x->_fbprev = fabs(_ov);
           *out++ = (t_sample)(_ov * 0.6); }
    }
    return (w + 6);
}

static void alien_xsnap_ep00018_01_dsp(t_alien_xsnap_ep00018_01 *x, t_signal **sp)
{
    x->x_sr = sp[0]->s_sr;
    dsp_add(alien_xsnap_ep00018_01_perform, 5, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, (t_int)sp[0]->s_n);
}

static void *alien_xsnap_ep00018_01_new(t_symbol *s, int argc, t_atom *argv)
{
    t_alien_xsnap_ep00018_01 *x = (t_alien_xsnap_ep00018_01 *)pd_new(alien_xsnap_ep00018_01_class);
    (void)s; (void)argc; (void)argv;
    x->x_sr = sys_getsr(); if (x->x_sr < 1000) x->x_sr = 44100;
    al_rng_seed(&x->rng, 49626149u);
    al_wander_init(&x->wnd, &x->rng, 8);
    al_dc_init(&x->dc);
    al_phasor_init(&x->_lfo); al_sh_init(&x->_sh); al_env_init(&x->_env);
    x->_cha.x=x->_cha.y=x->_cha.z=0.1; x->_chaacc=0.0; x->_chav=0.5; x->_fbprev=0.0;
    al_phasor_init(&x->src0_f); x->src0_gt=1e9; x->src0_pp=0.0;
    al_hilbert_init(&x->st0_h); x->st0_op=0.0; x->st0_fb=0.0;
    memset(x->st1_b,0,sizeof(x->st1_b)); x->st1_wr=0;
    al_ladder_init(&x->st2);
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->x_obj, &s_signal);
    return (void *)x;
}

void alien_xsnap_ep00018_01_tilde_setup(void)
{
    alien_xsnap_ep00018_01_class = class_new(gensym("alien_xsnap_ep00018_01~"),
        (t_newmethod)alien_xsnap_ep00018_01_new, 0, sizeof(t_alien_xsnap_ep00018_01), CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(alien_xsnap_ep00018_01_class, t_alien_xsnap_ep00018_01, x_f);
    class_addmethod(alien_xsnap_ep00018_01_class, (t_method)alien_xsnap_ep00018_01_dsp, gensym("dsp"), A_CANT, 0);
}
