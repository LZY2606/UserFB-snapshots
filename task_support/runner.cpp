#include "checks.hpp"
int main(){try {
{ Check c; test_optics(c); if(!c.assertions) throw std::runtime_error("empty optics"); std::cout<<"PASS optics "<<c.assertions<<"\n"; }
{ Check c; test_frames(c); if(!c.assertions) throw std::runtime_error("empty frames"); std::cout<<"PASS frames "<<c.assertions<<"\n"; }
{ Check c; test_transport(c); if(!c.assertions) throw std::runtime_error("empty transport"); std::cout<<"PASS transport "<<c.assertions<<"\n"; }
} catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;} }
