#include "checks.hpp"
int main(){try {
{ Check c; test_accumulation(c); if(!c.assertions) throw std::runtime_error("empty accumulation"); std::cout<<"PASS accumulation "<<c.assertions<<"\n"; }
{ Check c; test_depth(c); if(!c.assertions) throw std::runtime_error("empty depth"); std::cout<<"PASS depth "<<c.assertions<<"\n"; }
{ Check c; test_frames(c); if(!c.assertions) throw std::runtime_error("empty frames"); std::cout<<"PASS frames "<<c.assertions<<"\n"; }
} catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;} }
