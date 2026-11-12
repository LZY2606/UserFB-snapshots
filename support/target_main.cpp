#include "checks.h"
void grid_transmittance(Checks&);
void single_scatter_light(Checks&);
void recursive_segments(Checks&);
void empty_medium_compatibility(Checks&);
int main(){try{
 { Checks c; grid_transmittance(c); if(c.count==0) return 3; std::cout<<"PASS grid_transmittance "<<c.count<<"\n"; }
 { Checks c; single_scatter_light(c); if(c.count==0) return 3; std::cout<<"PASS single_scatter_light "<<c.count<<"\n"; }
 { Checks c; recursive_segments(c); if(c.count==0) return 3; std::cout<<"PASS recursive_segments "<<c.count<<"\n"; }
 { Checks c; empty_medium_compatibility(c); if(c.count==0) return 3; std::cout<<"PASS empty_medium_compatibility "<<c.count<<"\n"; }
}catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}return 0;}
