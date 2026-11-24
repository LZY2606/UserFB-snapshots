#include "dwarf/dwarf++.hh"
#include <cassert>
#include <iostream>
#include <memory>
#include <vector>
struct sections : dwarf::loader {
    std::vector<unsigned char> info{10,0,0,0,4,0,0,0,0,0,8,1,1,0x53};
    std::vector<unsigned char> abbrev{1,0x11,0,2,0x18,0,0,0};
    const void *load(dwarf::section_type t, size_t *n) override {
        auto *v = t == dwarf::section_type::info ? &info :
            t == dwarf::section_type::abbrev ? &abbrev : nullptr;
        if (!v) return nullptr;
        *n=v->size(); return v->data();
    }
};
int main() {
    auto s=std::make_shared<sections>(); dwarf::dwarf d(s);
    const auto &u=d.compilation_units().at(0);
    auto r=u.root()[dwarf::DW_AT::location].as_exprloc().evaluate(&dwarf::no_expr_context);
    assert(r.location_type==dwarf::expr_result::type::reg && r.value==3);
    std::cout << "DWARF_REGISTER_BASELINE_OK\n";
}
