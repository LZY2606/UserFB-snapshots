#include "../src/pugixml.hpp"
#include <cassert>
#include <iostream>
int main() {
 pugi::xml_document d;
 assert(d.load_string("<r xmlns:a='urn:one' xmlns:b='urn:one'><a:item a:k='7'/><b:item b:k='8'/></r>"));
 assert(d.select_nodes("/r/a:item").size()==1);
 assert(d.select_nodes("/r/*[local-name()='item' and namespace-uri()='urn:one']").size()==2);
 assert(d.select_nodes("/r/*[@a:k='7']").size()==1);
 std::cout<<"CURRENT_NAMESPACE_AXIS="<<d.select_nodes("/r/*/namespace::*").size()<<"\n";
 std::cout<<"BASELINE_XPATH_LITERAL=1 EXPANDED_REFERENCE=2 ATTR=1\n";
}
