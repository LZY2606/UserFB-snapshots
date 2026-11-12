#include <marisa.h>
#include <cassert>
#include <iostream>
#include <string>
int main(){
 marisa::Keyset ks;ks.push_back("cafe");ks.push_back("cafeteria");ks.push_back("咖啡");
 const char binary[]={'a','\0','b'};ks.push_back(binary,3);
 marisa::Trie t;t.build(ks,3|MARISA_BINARY_TAIL|MARISA_LABEL_ORDER);
 marisa::Agent a;a.set_query("caf");unsigned n=0;
 while(t.predictive_search(a)){std::string k(a.key().ptr(),a.key().length());assert(k=="cafe"||k=="cafeteria");++n;}assert(n==2);
 a.set_query(binary,3);assert(t.lookup(a));auto id=a.key().id();
 a.set_query(id);t.reverse_lookup(a);assert(std::string(a.key().ptr(),a.key().length())==std::string(binary,3));
 a.set_query("咖啡");assert(t.lookup(a));
 std::cout<<"MARISA_BASELINE_OK keys=4 predictive=2 binary-roundtrip=1\n";
}
