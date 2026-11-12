#include "checks.h"
void cascade_fit(Checks&);
void light_depth_clip(Checks&);
void stable_transition(Checks&);
void camera_shadow_integration(Checks&);
int main(){try{
 { Checks c; cascade_fit(c); if(c.count==0) return 3; std::cout<<"PASS cascade_fit "<<c.count<<"\n"; }
 { Checks c; light_depth_clip(c); if(c.count==0) return 3; std::cout<<"PASS light_depth_clip "<<c.count<<"\n"; }
 { Checks c; stable_transition(c); if(c.count==0) return 3; std::cout<<"PASS stable_transition "<<c.count<<"\n"; }
 { Checks c; camera_shadow_integration(c); if(c.count==0) return 3; std::cout<<"PASS camera_shadow_integration "<<c.count<<"\n"; }
}catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}return 0;}
