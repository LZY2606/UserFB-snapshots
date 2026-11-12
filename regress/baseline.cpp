#include <jsoncons/json.hpp>
#include <jsoncons_ext/jsonpath/jsonpath.hpp>
#include <cassert>
#include <iostream>
int main(){
 using jsoncons::json;
 auto a=json::parse(R"([{"a":[1,2,3]},{"a":[]},{"a":null},{}])");
 auto expr=jsoncons::jsonpath::make_expression<json>("$[?count(@.a) == 3]");
 auto p=expr.evaluate(a);assert(p.size()==1 && p[0]["a"].size()==3);
 auto q=jsoncons::jsonpath::json_query(a,"$[?length(@.a) == 0]");assert(q.size()==1 && q[0]["a"].is_array());
 std::cout<<"JSONCONS_LEGACY_FUNCTIONS_OK count-array-as-length=3 empty-array-length=0\n";
}
