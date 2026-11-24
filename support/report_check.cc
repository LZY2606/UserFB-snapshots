#include "common/dwarf/elf_reader.h"
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
using namespace google_breakpad;
static std::vector<std::string> split(const std::string& s,char delimiter) {
 std::vector<std::string> out;std::stringstream stream(s);std::string item;
 while(std::getline(stream,item,delimiter))out.push_back(item);
 return out;
}
static uint64_t number(const std::string& s,bool hex=true) {
 if(s.empty() || (hex && (s.size()<3 || s.substr(0,2)!="0x")))throw std::runtime_error("bad numeric identity");
 size_t n=0;uint64_t v=std::stoull(s,&n,hex?16:10);
 if(n!=s.size())throw std::runtime_error("bad number suffix");return v;
}
static bool offset_field(const std::string& s) {
 try {number(s);return true;}catch(...){return false;}
}
static std::pair<std::string,uint64_t> source(const std::string& s) {
 auto p=s.find('@');if(p==std::string::npos)throw std::runtime_error("missing source");
 auto section=s.substr(0,p);
 if(section!=".debug_info" && section!=".debug_loc" && section!=".debug_loclists")throw std::runtime_error("wrong source section");
 return {section,number(s.substr(p+1))};
}
int main(int argc,char** argv) {
 try {
  if(argc!=3)throw std::runtime_error("queries and report required");
  std::ifstream qf(argv[1]),rf(argv[2]);if(!qf || !rf)throw std::runtime_error("missing report/input");
  std::vector<std::vector<std::string>> queries;std::string line;
  while(std::getline(qf,line)){auto q=split(line,'\t');if(q.size()!=4)throw std::runtime_error("bad query");number(q[3]);queries.push_back(q);}
  if(queries.empty())throw std::runtime_error("zero queries");
  if(!std::getline(rf,line) || line!="query\tsample\tpc\tcu\tfunction_die\tvariable_die\tscopes\tstatus\tdescription\tattribute\tentry\texpression\tbytes")throw std::runtime_error("bad report header");
  std::vector<size_t> seen(queries.size());std::map<size_t,uint64_t> previous_die;
  std::set<std::string> statuses,kinds;size_t previous_query=0,rows=0;
  while(std::getline(rf,line)) {
   auto r=split(line,'\t');if(r.size()!=13)throw std::runtime_error("bad report row");
   uint64_t id=number(r[0],false);if(!id || id>queries.size() || id<previous_query)throw std::runtime_error("query order/coverage");
   const auto& q=queries[id-1];if(r[1]!=q[0] || number(r[2])!=number(q[3]))throw std::runtime_error("wrong query attribution");
   previous_query=id;++rows;++seen[id-1];statuses.insert(r[7]);
   if(r[7]!="described" && r[7]!="unavailable" && r[7]!="out_of_scope" && r[7]!="not_found")throw std::runtime_error("unknown status");
   if(r[7]=="not_found") {
    for(size_t i: {3,4,5,6,8,9,10,11,12})if(r[i]!="-")throw std::runtime_error("invented not_found identity");
    if(seen[id-1]!=1)throw std::runtime_error("mixed not_found");continue;
   }
   if(!offset_field(r[3]) || !offset_field(r[4]) || !offset_field(r[5]) || r[6]=="-")throw std::runtime_error("missing candidate evidence");
   uint64_t die=number(r[5]);if(previous_die.count(id) && die<=previous_die[id])throw std::runtime_error("duplicate/unsorted variable identity");previous_die[id]=die;
   if(r[7]=="described") {
    if(r[12]=="-" || r[12].empty() || r[12].size()%2)throw std::runtime_error("empty described bytes");
    std::string kind=r[8].substr(0,r[8].find(':'));if(kind!="reg" && kind!="breg" && kind!="fbreg" && kind!="opaque")throw std::runtime_error("missing description");kinds.insert(kind);
    auto src=source(r[11]);ElfReader elf(r[1]);size_t size=0;const char* bytes=elf.GetSectionByName(src.first,&size);
    if(!bytes || src.second>size || r[12].size()/2>size-src.second)throw std::runtime_error("source outside ELF section");
    for(size_t i=0;i<r[12].size()/2;++i) {
     const char* digits="0123456789abcdef";unsigned char b=static_cast<unsigned char>(bytes[src.second+i]);
     if(r[12][2*i]!=digits[b>>4] || r[12][2*i+1]!=digits[b&15])throw std::runtime_error("expression disagrees with source bytes");
    }
    if(r[10]!="-"){auto entry=source(r[10]);if(entry.first!=src.first || entry.second>src.second)throw std::runtime_error("inconsistent list entry");}
    else if(src.first!=".debug_info")throw std::runtime_error("missing list entry");
    if(r[9]=="-")throw std::runtime_error("missing location attribute");
   } else {
    if(r[8]!="-" || r[12]!="-")throw std::runtime_error("stale description");
    if(r[7]=="out_of_scope" && (r[10]!="-" || r[11]!="-"))throw std::runtime_error("selected out-of-scope location");
   }
  }
  for(size_t n:seen)if(!n)throw std::runtime_error("unanswered query");
  for(const char* status:{"described","unavailable","out_of_scope","not_found"})if(!statuses.count(status))throw std::runtime_error("missing sample status");
  for(const char* kind:{"reg","breg","fbreg","opaque"})if(!kinds.count(kind))throw std::runtime_error("missing sample description kind");
  std::cout<<"REPORT_BYTES_OK queries="<<queries.size()<<" rows="<<rows<<"\n";
  return 0;
 } catch(const std::exception& e){std::cerr<<"report failed: "<<e.what()<<"\n";return 1;}
}
