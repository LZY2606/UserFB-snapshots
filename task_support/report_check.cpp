#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
using Bytes=std::vector<unsigned char>;
static void need(bool b,const char*m){if(!b)throw std::runtime_error(m);}
static Bytes load(const std::string&p){std::ifstream f(p,std::ios::binary);need(bool(f),"open input");return Bytes(std::istreambuf_iterator<char>(f),{});}
static std::vector<std::string> split(const std::string&s){std::vector<std::string>v;std::stringstream f(s);std::string x;while(std::getline(f,x,'\t'))v.push_back(x);return v;}
static unsigned digit(char c){if(c>='0'&&c<='9')return unsigned(c-'0');if(c>='a'&&c<='f')return unsigned(c-'a'+10);throw std::runtime_error("non-lowercase hex");}
static Bytes unhex(const std::string&s){if(s=="-")return {};need(!s.empty()&&s.size()%2==0,"hex length");Bytes b;for(std::size_t i=0;i<s.size();i+=2)b.push_back(static_cast<unsigned char>(16*digit(s[i])+digit(s[i+1])));return b;}
static std::size_t number(const std::string&s){std::size_t used=0;auto n=std::stoull(s,&used,0);need(used==s.size(),"number suffix");return n;}
static void evidence(const Bytes&file,const std::string&off,const Bytes&b){auto o=number(off);need(o<=file.size()&&b.size()<=file.size()-o,"evidence range");need(std::equal(b.begin(),b.end(),file.begin()+o),"evidence bytes differ");}
struct Row{std::string relation;unsigned candidates=0;std::set<std::string>sources;std::set<std::string>keys;};
int main(int argc,char**argv){try{
 need(argc>=3,"report and inputs required");std::map<std::string,Bytes>files;std::map<std::string,unsigned>seen;
 for(int i=2;i<argc;++i){std::string p=argv[i];auto n=p.substr(p.find_last_of("/\\")+1);need(!n.empty()&&!files.count(n),"duplicate input basename");files[n]=load(p);seen[n]=0;}
 std::ifstream report(argv[1]);need(bool(report),"open report");std::string line;std::map<std::pair<std::string,std::string>,Row>rows;std::set<std::pair<std::string,std::string>>members;unsigned candidates=0;
 while(std::getline(report,line)){
  auto f=split(line);need(!f.empty(),"empty report row");
  if(f[0]=="R"){
   need(f.size()==7&&files.count(f[1]),"R shape/input");number(f[2]);number(f[4]);auto b=unhex(f[5]);need(b.size()==28,"directory record width");evidence(files.at(f[1]),f[3],b);
   need(!rows.count({f[1],f[2]}),"duplicate record");need(std::set<std::string>{"none","file_only","rva_only","same","equal_distinct","conflict"}.count(f[6]),"relation");rows[{f[1],f[2]}].relation=f[6];++seen[f[1]];
  }else if(f[0]=="C"){
   need(f.size()==10&&files.count(f[1]),"C shape/input");auto it=rows.find({f[1],f[2]});need(it!=rows.end(),"candidate without record");auto &r=it->second;need(f[3]=="file"||f[3]=="rva","source");need(r.sources.insert(f[3]).second,"duplicate candidate source");
   need(f[5]=="overlay"||f[5]=="headers"||f[5]=="gap"||f[5].rfind("section:",0)==0,"owner");auto b=unhex(f[6]);need(!b.empty(),"empty candidate");evidence(files.at(f[1]),f[4],b);need(f[7]=="RSDS"||f[7]=="NB10"||f[7]=="opaque","format");unhex(f[9]);if(f[8]!="-")r.keys.insert(f[8]);++r.candidates;++candidates;
  }else if(f[0]=="G"){
   need(f.size()==4,"G shape");auto it=rows.find({f[2],f[3]});need(it!=rows.end(),"member without record");need(it->second.relation!="conflict"&&it->second.keys.count(f[1]),"unconfirmed member");need(members.insert({f[2],f[3]}).second,"duplicate group member");
  }else throw std::runtime_error("unknown row");
 }
 need(!rows.empty()&&candidates>0,"empty report");for(const auto&x:seen)need(x.second>0,"missing input report");
 for(const auto&x:rows){auto&r=x.second;unsigned n=r.relation=="none"?0:(r.relation=="file_only"||r.relation=="rva_only"?1:2);need(r.candidates==n,"missing candidate");}
 std::cout<<"REPORT_BYTE_EVIDENCE_OK records="<<rows.size()<<" candidates="<<candidates<<"\n";
 }catch(const std::exception&e){std::cerr<<"report: "<<e.what()<<"\n";return 1;}}
