#include "simdjson.h"
#include <cassert>
#include <iostream>
using namespace simdjson;
int main(){
 ondemand::parser parser;
 auto input=R"({"capture":{"items":[{"id":7},{"id":9}]},"ok":true})"_padded;
 ondemand::document doc; assert(!parser.iterate(input).get(doc));
 uint64_t id=0;assert(!doc.at_pointer("/capture/items/1/id").get_uint64().get(id));assert(id==9);
 std::string_view raw;assert(!doc.at_pointer("/capture/items/0").raw_json().get(raw));assert(raw.find("7")!=raw.npos);
 assert(!doc.at_pointer("/capture/items/0/id").get_uint64().get(id));assert(id==7);
 std::cout<<"SIMDJSON_BASELINE_OK pointer-rewind-and-raw\n";
}
