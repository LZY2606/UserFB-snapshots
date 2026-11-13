#include "checks.hpp"
int main(){try {
{ Check c; test_gradients(c); if(!c.assertions) throw std::runtime_error("empty gradients"); std::cout<<"PASS gradients "<<c.assertions<<"\n"; }
{ Check c; test_clipping(c); if(!c.assertions) throw std::runtime_error("empty clipping"); std::cout<<"PASS clipping "<<c.assertions<<"\n"; }
{ Check c; test_sampling(c); if(!c.assertions) throw std::runtime_error("empty sampling"); std::cout<<"PASS sampling "<<c.assertions<<"\n"; }
} catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;} }
