#include "checks.h"
void gbuffer_capture(Checks&);
void hemisphere_visibility(Checks&);
void bilateral_boundaries(Checks&);
void lighting_integration(Checks&);
int main(){try{
 { Checks c; gbuffer_capture(c); if(c.count==0) return 3; std::cout<<"PASS gbuffer_capture "<<c.count<<"\n"; }
 { Checks c; hemisphere_visibility(c); if(c.count==0) return 3; std::cout<<"PASS hemisphere_visibility "<<c.count<<"\n"; }
 { Checks c; bilateral_boundaries(c); if(c.count==0) return 3; std::cout<<"PASS bilateral_boundaries "<<c.count<<"\n"; }
 { Checks c; lighting_integration(c); if(c.count==0) return 3; std::cout<<"PASS lighting_integration "<<c.count<<"\n"; }
}catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}return 0;}
