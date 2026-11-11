#include <type_traits>
#include "fg/FrameGraph.hpp"
#include <cassert>
#include <iostream>
struct Texture {
 struct Desc { int size; };
 int value=0; static inline int creates=0,destroys=0;
 void create(const Desc&,void*) { ++creates; value=0; }
 void destroy(const Desc&,void*) { ++destroys; }
};
struct Data { FrameGraphResource h; };
int main() {
 FrameGraph fg; bool dead=false; int output=0;
 const auto &a=fg.addCallbackPass<Data>("fill",[](auto& b,auto& d){d.h=b.template create<Texture>("color",{4}); d.h=b.write(d.h);},[](auto& d,auto& r,void*){r.template get<Texture>(d.h).value=7;});
 const auto &b=fg.addCallbackPass<Data>("shade",[&](auto& builder,auto& d){d.h=builder.write(builder.read(a.h));},[](auto& d,auto& r,void*){r.template get<Texture>(d.h).value*=3;});
 fg.addCallbackPass("present",[&](auto& builder,auto&){builder.read(b.h); builder.setSideEffect();},[&](auto&,auto& r,void*){output=r.template get<Texture>(b.h).value;});
 fg.addCallbackPass<Data>("unconsumed",[](auto& builder,auto& d){d.h=builder.template create<Texture>("unused",{2}); d.h=builder.write(d.h);},[&](auto&,auto&,void*){dead=true;});
 fg.compile(); fg.execute();
 assert(output==21 && !dead && Texture::creates==1 && Texture::destroys==1);
 assert(!fg.isValid(a.h) && fg.isValid(b.h));
 std::cout<<"FRAMEGRAPH_BASELINE_OK output=21 live=3 culled=1 create=1 destroy=1\n";
}
