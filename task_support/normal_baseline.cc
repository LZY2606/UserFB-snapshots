#include <cstring>
#include <iostream>
#include <stdexcept>
#include "normal_fixture.hh"
// Exercise the original example traversal, without editing its production file.
#define main upstream_example_main
#include "../examples/find-pc.cc"
#undef main
static unsigned checks=0;
static void require(bool ok,const char *message){++checks;if(!ok)throw std::runtime_error(message);}
static void collect(const dwarf::die &d,std::map<uint64_t,dwarf::die> &all){
    all.emplace(d.get_section_offset(),d);for(auto child:d)collect(child,all);
}
int main(){
    try {
        auto f=std::make_shared<normal_fixture::fixture>();dwarf::dwarf dw(f);
        auto &cus=dw.compilation_units();require(cus.size()==2,"two actual CUs");
        std::map<uint64_t,dwarf::die> nodes;for(auto &cu:cus)collect(cu.root(),nodes);
        auto node=[&](const char *name){return nodes.at(f->offsets.at(name));};
        auto a=node("inlineA"),b=node("inlineB"),inner=node("inner"),block=node("block"),outer=node("outer");
        auto origin=a[dwarf::DW_AT::abstract_origin].as_reference();
        require(origin.get_section_offset()==f->offsets.at("origin"),"cross-CU ref_addr target");
        require(origin.get_unit()==cus[1],"origin belongs to CU B");
        require(b[dwarf::DW_AT::abstract_origin].as_reference()==origin,"shared origin distinct instances");
        require(a!=b,"concrete identities stay distinct");
        require(a.resolve(dwarf::DW_AT::name).as_string()=="callee","existing origin name resolution");
        require(inner.resolve(dwarf::DW_AT::name).as_string()=="inner","concrete name override");
        require(a.resolve(dwarf::DW_AT::decl_line).as_uconstant()==7,"definition line from origin");
        require(a[dwarf::DW_AT::call_line].as_uconstant()==41,"concrete call line");
        require(!b.has(dwarf::DW_AT::call_file),"second concrete instance has no call file");
        auto decl=origin[dwarf::DW_AT::decl_file].as_uconstant();
        auto call=a[dwarf::DW_AT::call_file].as_uconstant();
        require(decl==call && decl==1,"same numeric file index in different CUs");
        require(cus[0].get_line_table().get_file(call)->path=="/normal/A/caller.cc","call file belongs to A");
        require(cus[1].get_line_table().get_file(decl)->path=="/normal/B/definition.hpp","decl file belongs to B");
        auto ranges=dwarf::die_pc_range(a);
        require(ranges.contains(0x1010),"initial CU base");require(!ranges.contains(0x1020),"half-open first upper bound");
        require(!ranges.contains(0x1035),"range hole");require(ranges.contains(0x1040),"base selection");require(!ranges.contains(0x1050),"half-open second upper bound");
        require(dwarf::die_pc_range(block).contains(0x1035),"lexical block differs from child range");
        require(!dwarf::die_pc_range(block).contains(0x1028),"lexical block hole");
        require(dwarf::at_high_pc(inner)==0x1018,"absolute high_pc");require(dwarf::at_high_pc(b)==0x1046,"constant high_pc");
        require(dwarf::die_pc_range(b).contains(0x1015),"second sibling also covers PC");
        require(dwarf::die_pc_range(outer).contains(0x1015),"physical function covers PC");
        std::vector<dwarf::die> stack;
        require(find_pc(cus[0].root(),0x1015,&stack),"original example normal query");
        require(stack.size()==3,"original first branch depth");
        require(stack[0]==inner && stack[1]==a && stack[2]==outer,"original example chooses first branch inner-to-outer");
        std::cout<<"NORMAL_PARSE_OK cus=2 cross_cu=1 file_tables=2 range_base=1\n";
        std::cout<<"ORIGINAL_FIND_PC_OK pc=0x1015 frames=3 first_branch_only=1\n";
        std::cout<<"NORMAL_BASELINE_OK checks="<<checks<<"\n";
        return 0;
    }catch(const std::exception &e){std::cerr<<"NORMAL_BASELINE_FAIL "<<e.what()<<"\n";return 1;}
}
