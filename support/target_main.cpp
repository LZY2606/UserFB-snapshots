#include "checks.h"
void nearest_identity(Checks&);
void simd_leaf_tail(Checks&);
void animated_refit(Checks&);
void render_acceleration(Checks&);
int main(){try{
 { Checks c; nearest_identity(c); if(c.count==0) return 3; std::cout<<"PASS nearest_identity "<<c.count<<"\n"; }
 { Checks c; simd_leaf_tail(c); if(c.count==0) return 3; std::cout<<"PASS simd_leaf_tail "<<c.count<<"\n"; }
 { Checks c; animated_refit(c); if(c.count==0) return 3; std::cout<<"PASS animated_refit "<<c.count<<"\n"; }
 { Checks c; render_acceleration(c); if(c.count==0) return 3; std::cout<<"PASS render_acceleration "<<c.count<<"\n"; }
}catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}return 0;}
