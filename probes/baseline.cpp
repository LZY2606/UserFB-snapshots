#include "pandaresampler.hh"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>
#include <algorithm>
using PandaResampler::Resampler2;
static void check(Resampler2::Mode mode, unsigned ratio) {
    const unsigned n=17, input_count=mode==Resampler2::UP?n:n*ratio;
    const unsigned output_count=mode==Resampler2::UP?n*ratio:n;
    std::vector<float> in(input_count),out(output_count),first(output_count),previous(output_count),negative_in(input_count),negative_out(output_count);
    for(unsigned i=0;i<input_count;i++){unsigned k=i%n;in[i]=0.4f*std::cos(6.283185307179586*k/n)+(k==0?0.1f:0);negative_in[i]=-in[i];}
    Resampler2 a(mode,ratio,Resampler2::PREC_96DB,false,Resampler2::FILTER_IIR);
    Resampler2 b(mode,ratio,Resampler2::PREC_96DB,false,Resampler2::FILTER_IIR);
    for(unsigned turn=0;turn<4096;turn++) {
        previous=out;
        a.process_block(in.data(),input_count,out.data());
        b.process_block(negative_in.data(),input_count,negative_out.data());
        if(turn==0)first=out;
    }
    double cold_gap=0,closure=0,channel_error=0,energy=0;
    for(unsigned i=0;i<output_count;i++){
        assert(std::isfinite(out[i]));
        cold_gap=std::max(cold_gap,double(std::fabs(first[i]-out[i])));
        closure=std::max(closure,double(std::fabs(previous[i]-out[i])));
        channel_error=std::max(channel_error,double(std::fabs(out[i]+negative_out[i])));
        energy+=out[i]*out[i];
    }
    assert(energy>0.001 && cold_gap>1e-4 && closure<1e-6 && channel_error<1e-6);
    a.reset();a.process_block(in.data(),input_count,out.data());
    for(unsigned i=0;i<output_count;i++)assert(out[i]==first[i]);
    std::printf("IIR_LOOP mode=%s ratio=%u source_period=17 output_period=%u cold_gap=%.9g repeat_residual=%.9g channel_error=%.9g\n",mode==Resampler2::UP?"UP":"DOWN",ratio,output_count,cold_gap,closure,channel_error);
}
int main(){check(Resampler2::UP,4);check(Resampler2::DOWN,8);std::puts("ZERO_RESET_REPRODUCES_COLD_START PERIODIC_PLAN_ABSENT");return 0;}
