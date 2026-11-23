// Independently authored valid DWARF4, little endian, DWARF32, 8-byte addresses.
// This is raw parser input; it does not implement inline stack symbolization.
#pragma once
#include "dwarf++.hh"
#include <map>
#include <vector>
#include <string>
#include <cstdint>
#include <stdexcept>
#include <initializer_list>
namespace normal_fixture {
struct bytes {
    std::vector<unsigned char> v;
    size_t size() const { return v.size(); }
    void n(uint64_t x, unsigned width) { for (unsigned i=0;i<width;++i) v.push_back((x>>(8*i))&255); }
    void u(uint64_t x) { do { unsigned char b=x&127; x>>=7; v.push_back(b|(x?128:0)); } while(x); }
    void str(const std::string &s) { v.insert(v.end(),s.begin(),s.end()); v.push_back(0); }
    void patch(size_t at,uint64_t x,unsigned width=4) { if(at+width>size()) throw std::logic_error("patch bounds"); for(unsigned i=0;i<width;++i)v[at+i]=(x>>(8*i))&255; }
    void append(const bytes &b) {v.insert(v.end(),b.v.begin(),b.v.end());}
};
struct fixture : dwarf::loader {
    std::map<dwarf::section_type,bytes> sections;
    std::map<std::string,uint64_t> offsets;
    fixture() {
        bytes ab;
        typedef std::pair<unsigned,unsigned> attr;
        // Attributes and forms use the numeric encodings from DWARF4.
        auto abbrev=[&](unsigned id,unsigned tag,bool children,std::initializer_list<attr> attrs){
            ab.u(id);ab.u(tag);ab.n(children,1);
            for(auto a:attrs){ab.u(a.first);ab.u(a.second);} ab.u(0);ab.u(0);
        };
        abbrev(1,0x11,true,{{3,8},{0x1b,8},{0x11,1},{0x12,6},{0x10,0x17}});
        abbrev(2,0x2e,true,{{3,8},{0x11,1},{0x12,6}});
        abbrev(3,0x0b,true,{{0x55,0x17}});
        abbrev(4,0x1d,true,{{0x31,0x10},{0x55,0x17},{0x58,0x0b},{0x59,5},{0x57,0x0b}});
        abbrev(5,0x1d,false,{{3,8},{0x31,0x10},{0x11,1},{0x12,1},{0x58,0x0b},{0x59,5},{0x57,0x0b}});
        abbrev(6,0x1d,false,{{0x31,0x10},{0x11,1},{0x12,6}});
        abbrev(7,0x39,true,{{3,8}});
        abbrev(8,0x2e,false,{{3,8},{0x6e,8},{0x3a,0x0b},{0x3b,5},{0x39,0x0b},{0x20,0x0b}});
        ab.u(0); sections[dwarf::section_type::abbrev]=ab;
        bytes line;
        auto table=[&](const std::string &one,const std::string &two){
            size_t start=line.size();line.n(0,4);line.n(4,2);size_t h=line.size();line.n(0,4);
            line.n(1,1);line.n(1,1);line.n(1,1);line.n(251,1);line.n(14,1);line.n(13,1);
            const unsigned counts[]={0,1,1,1,1,0,0,0,1,0,0,1};
            for(auto c:counts)line.n(c,1);
            line.str(""); // Directory table: directory 0 is CU comp_dir.
            for(auto f:{one,two}){line.str(f);line.u(0);line.u(0);line.u(0);}
            line.str(""); line.patch(h,line.size()-(h+4));
            // A normal set_address/copy/advance_pc/end_sequence program.
            line.n(0,1);line.u(9);line.n(2,1);line.n(0x1000,8);
            line.n(1,1);line.n(2,1);line.u(0x100);line.n(0,1);line.u(1);line.n(1,1);
            line.patch(start,line.size()-(start+4));return start;
        };
        size_t la=table("caller.cc","helper.cc"),lb=table("definition.hpp","other.hpp");
        sections[dwarf::section_type::line]=line;
        bytes ranges;
        // Both lists start with CU A's initial base 0x1000. The inline list
        // then selects a different base; gaps remain gaps.
        offsets["block_ranges"]=ranges.size();
        for(auto p:{std::make_pair(0x10ULL,0x20ULL),std::make_pair(0x30ULL,0x50ULL),std::make_pair(0ULL,0ULL)}){ranges.n(p.first,8);ranges.n(p.second,8);}
        offsets["inline_ranges"]=ranges.size();
        for(auto p:{std::make_pair(0x10ULL,0x20ULL),std::make_pair(~0ULL,0x1040ULL),std::make_pair(0ULL,0x10ULL),std::make_pair(0ULL,0ULL)}){ranges.n(p.first,8);ranges.n(p.second,8);}
        sections[dwarf::section_type::ranges]=ranges;
        bytes info;
        auto cu=[&](const std::string &name,const std::string &dir,uint64_t low,size_t stmt){
            size_t start=info.size();info.n(0,4);info.n(4,2);info.n(0,4);info.n(8,1);
            info.u(1);info.str(name);info.str(dir);info.n(low,8);info.n(0x100,4);info.n(stmt,4);return start;
        };
        std::vector<size_t> refs;
        size_t a=cu("caller.cc","/normal/A",0x1000,la);offsets["cuA"]=a;
        offsets["outer"]=info.size();info.u(2);info.str("outer");info.n(0x1000,8);info.n(0x80,4);
        offsets["block"]=info.size();info.u(3);info.n(offsets["block_ranges"],4);
        offsets["inlineA"]=info.size();info.u(4);refs.push_back(info.size());info.n(0,4);info.n(offsets["inline_ranges"],4);info.n(1,1);info.n(41,2);info.n(9,1);
        offsets["inner"]=info.size();info.u(5);info.str("inner");refs.push_back(info.size());info.n(0,4);info.n(0x1014,8);info.n(0x1018,8);info.n(2,1);info.n(42,2);info.n(3,1);
        info.u(0); // end inlineA children
        info.u(0); // end lexical block children
        offsets["inlineB"]=info.size();info.u(6);refs.push_back(info.size());info.n(0,4);info.n(0x1012,8);info.n(0x34,4);
        info.u(0);info.u(0); // end outer and CU A
        info.patch(a,info.size()-(a+4));
        size_t b=cu("definition.hpp","/normal/B",0x2000,lb);offsets["cuB"]=b;
        offsets["abstract_namespace"]=info.size();info.u(7);info.str("abstract_namespace");
        offsets["origin"]=info.size();info.u(8);info.str("callee");info.str("_Z6calleev");info.n(1,1);info.n(7,2);info.n(2,1);info.n(3,1);
        info.u(0);info.u(0);info.patch(b,info.size()-(b+4));
        for(auto r:refs)info.patch(r,offsets["origin"]);
        sections[dwarf::section_type::info]=info;
    }
    const void *load(dwarf::section_type type,size_t *size_out) override {
        auto it=sections.find(type);if(it==sections.end()){*size_out=0;return nullptr;}
        *size_out=it->second.v.size();return it->second.v.data();
    }
};
}
