#include "checks.hpp"
int main(){try {
{ Check c; test_classification(c); if(!c.assertions) throw std::runtime_error("empty classification"); std::cout<<"PASS classification "<<c.assertions<<"\n"; }
{ Check c; test_visibility(c); if(!c.assertions) throw std::runtime_error("empty visibility"); std::cout<<"PASS visibility "<<c.assertions<<"\n"; }
{ Check c; test_render(c); if(!c.assertions) throw std::runtime_error("empty render"); std::cout<<"PASS render "<<c.assertions<<"\n"; }
} catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;} }
