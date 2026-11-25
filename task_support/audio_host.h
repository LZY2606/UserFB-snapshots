#ifndef TASK_AUDIO_HOST_H
#define TASK_AUDIO_HOST_H
#include "chuck.h"
#include <algorithm>
#include <vector>
#include <string>
#include <stdexcept>
inline std::vector<float> task_render(const std::string &code, int frames, int block=113, bool adaptive=true) {
 ChucK c;
 c.setParam(CHUCK_PARAM_SAMPLE_RATE,48000);
 c.setParam(CHUCK_PARAM_INPUT_CHANNELS,1);
 c.setParam(CHUCK_PARAM_OUTPUT_CHANNELS,1);
 c.setParam(CHUCK_PARAM_VM_HALT,FALSE);
 c.setParam(CHUCK_PARAM_CHUGIN_ENABLE,FALSE);
 c.setParam(CHUCK_PARAM_IS_REALTIME_AUDIO_HINT,FALSE);
 c.setParam(CHUCK_PARAM_VM_ADAPTIVE,adaptive?512:0);
 if(!c.init()||!c.start())throw std::runtime_error("host init failed");
 if(!c.compileCode(code))throw std::runtime_error("script compilation failed");
 std::vector<float> result; result.reserve(frames);
 std::vector<SAMPLE> in(block,0),out(block,0);
 for(int at=0;at<frames;){int n=std::min(block,frames-at);std::fill(out.begin(),out.end(),0);c.run(in.data(),out.data(),n);result.insert(result.end(),out.begin(),out.begin()+n);at+=n;}
 return result;
}
#endif
