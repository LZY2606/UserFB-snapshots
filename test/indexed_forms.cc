// Regression tests for DWARF5 indexed attribute forms
// (DW_FORM_strx*/DW_FORM_addrx*), DW_FORM_implicit_const, and their
// interaction with the pre-existing DWARF4 forms.  All tests go
// through the regular public interface (compilation_units(), root(),
// child DIE iteration, attributes(), operator[] and resolve()) on
// synthetic in-memory ELF images.

#include "task_support/check.h"

#include "task_support/fixture.hh"
#include "dwarf/dwarf++.hh"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

using fixture::Bytes;
using fixture::put;
using fixture::patch;
using fixture::text;

using AT = dwarf::DW_AT;

namespace {

void
uleb(Bytes &b, std::uint64_t v)
{
        do {
                unsigned char c = v & 0x7f;
                v >>= 7;
                if (v)
                        c |= 0x80;
                b.push_back(c);
        } while (v);
}

void
sleb(Bytes &b, std::int64_t v)
{
        while (1) {
                unsigned char c = v & 0x7f;
                v >>= 7;
                if ((v == 0 && !(c & 0x40)) || (v == -1 && (c & 0x40))) {
                        b.push_back(c);
                        return;
                }
                b.push_back(c | 0x80);
        }
}

void
putbe(Bytes &b, std::uint64_t v, unsigned n)
{
        for (unsigned i = 0; i < n; ++i)
                b.push_back((v >> (8 * (n - 1 - i))) & 255);
}

// Begin a DWARF5 compilation unit (DWARF32, little endian); the
// caller appends DIEs and then calls finish_cu to patch the length.
Bytes
begin_cu5(unsigned addr_size, std::uint32_t abbrev_off)
{
        Bytes b;
        put(b, 0, 4);           // unit_length, patched by finish_cu
        put(b, 5, 2);           // DWARF version
        put(b, 1, 1);           // DW_UT_compile
        put(b, addr_size, 1);
        put(b, abbrev_off, 4);
        return b;
}

void
finish_cu(Bytes &b)
{
        patch(b, 0, b.size() - 4, 4);
}

// Open a synthetic ELF image as a dwarf::dwarf.  Everything stays
// live as long as the session object.
struct session {
        Bytes raw;
        std::shared_ptr<fixture::Loader> loader;
        elf::elf file;
        dwarf::dwarf dw;

        session(const std::map<std::string, Bytes> &sections)
                : raw(fixture::elf_image(sections)),
                  loader(std::make_shared<fixture::Loader>(raw)),
                  file(loader),
                  dw(dwarf::elf::create_loader(file)) { }
};

template<typename F>
bool
throws_std(F &&f)
{
        try {
                f();
        } catch (const std::exception &) {
                return true;
        }
        return false;
}

//////////////////////////////////////////////////////////////////
// Shared two-CU image.  Both CUs use the same table indices, but
// have disjoint .debug_str_offsets/.debug_addr contributions.
//
// .debug_str layout (offset of each string):
//   0  "unit-alpha"   11 "producer-x"  22 "func-alpha"  33 "var-alpha"
//   43 "unit-beta"    53 "producer-y"  64 "func-beta"   74 "var-beta"
//
// .debug_str_offsets: [0..7] CU1 header, [8..23] CU1 entries
// {0,11,22,33}, [24..31] CU2 header, [32..47] CU2 entries
// {43,53,64,74}.
//
// .debug_addr: [0..31] CU1 {0x401000..0x404000}, [32..63] CU2
// {0x501000..0x504000}.

Bytes
main_abbrev()
{
        Bytes b;
        // Code 1: DW_TAG_compile_unit, has children.  The indexed
        // attributes deliberately precede the base attributes.
        uleb(b, 1); uleb(b, 0x11); put(b, 1, 1);
        uleb(b, 0x03); uleb(b, 0x25);   // DW_AT_name, strx1
        uleb(b, 0x25); uleb(b, 0x1a);   // DW_AT_producer, strx
        uleb(b, 0x11); uleb(b, 0x1b);   // DW_AT_low_pc, addrx
        uleb(b, 0x72); uleb(b, 0x17);   // DW_AT_str_offsets_base, sec_offset
        uleb(b, 0x73); uleb(b, 0x17);   // DW_AT_addr_base, sec_offset
        uleb(b, 0); uleb(b, 0);
        // Code 2: DW_TAG_subprogram, no children
        uleb(b, 2); uleb(b, 0x2e); put(b, 0, 1);
        uleb(b, 0x03); uleb(b, 0x27);   // name, strx3
        uleb(b, 0x11); uleb(b, 0x2e);   // low_pc, addrx4
        uleb(b, 0); uleb(b, 0);
        // Code 3: DW_TAG_subprogram, no children
        uleb(b, 3); uleb(b, 0x2e); put(b, 0, 1);
        uleb(b, 0x03); uleb(b, 0x26);   // name, strx2
        uleb(b, 0x11); uleb(b, 0x2b);   // low_pc, addrx1
        uleb(b, 0); uleb(b, 0);
        // Code 4: DW_TAG_subprogram, no children
        uleb(b, 4); uleb(b, 0x2e); put(b, 0, 1);
        uleb(b, 0x03); uleb(b, 0x28);   // name, strx4
        uleb(b, 0x11); uleb(b, 0x2c);   // low_pc, addrx2
        uleb(b, 0); uleb(b, 0);
        uleb(b, 0);
        return b;
}

Bytes
main_cu(std::uint32_t str_base, std::uint32_t addr_base)
{
        Bytes b = begin_cu5(8, 0);
        // Root DIE
        uleb(b, 1);
        put(b, 0, 1);                   // strx1 index 0
        uleb(b, 1);                     // strx index 1
        uleb(b, 0);                     // addrx index 0
        put(b, str_base, 4);            // str_offsets_base
        put(b, addr_base, 4);           // addr_base
        // Child 1
        uleb(b, 2);
        put(b, 2, 3);                   // strx3 index 2
        put(b, 1, 4);                   // addrx4 index 1
        // Child 2
        uleb(b, 3);
        put(b, 3, 2);                   // strx2 index 3
        put(b, 2, 1);                   // addrx1 index 2
        // Child 3
        uleb(b, 4);
        put(b, 1, 4);                   // strx4 index 1
        put(b, 3, 2);                   // addrx2 index 3
        uleb(b, 0);                     // end of children
        finish_cu(b);
        return b;
}

Bytes
main_str()
{
        Bytes b;
        text(b, "unit-alpha");          // 0
        text(b, "producer-x");          // 11
        text(b, "func-alpha");          // 22
        text(b, "var-alpha");           // 33
        text(b, "unit-beta");           // 43
        text(b, "producer-y");          // 53
        text(b, "func-beta");           // 64
        text(b, "var-beta");            // 74
        return b;
}

Bytes
main_str_offsets()
{
        Bytes b;
        put(b, 0, 8);                                   // CU1 header
        put(b, 0, 4); put(b, 11, 4); put(b, 22, 4); put(b, 33, 4);
        put(b, 0, 8);                                   // CU2 header
        put(b, 43, 4); put(b, 53, 4); put(b, 64, 4); put(b, 74, 4);
        return b;
}

Bytes
main_addr()
{
        Bytes b;
        put(b, 0x401000, 8); put(b, 0x402000, 8);
        put(b, 0x403000, 8); put(b, 0x404000, 8);
        put(b, 0x501000, 8); put(b, 0x502000, 8);
        put(b, 0x503000, 8); put(b, 0x504000, 8);
        return b;
}

std::map<std::string, Bytes>
main_sections()
{
        Bytes info = main_cu(8, 0);
        Bytes cu2 = main_cu(32, 32);
        info.insert(info.end(), cu2.begin(), cu2.end());
        return {{".debug_info", info},
                {".debug_abbrev", main_abbrev()},
                {".debug_str", main_str()},
                {".debug_str_offsets", main_str_offsets()},
                {".debug_addr", main_addr()}};
}

//////////////////////////////////////////////////////////////////
// indexed group

int
case_strx_addrx_forms()
{
        session s(main_sections());
        auto &cus = s.dw.compilation_units();
        TASK_CHECK(cus.size() == 2);

        dwarf::die root = cus.at(0).root();
        TASK_CHECK(root.tag == dwarf::DW_TAG::compile_unit);
        TASK_CHECK(root[AT::name].as_string() == "unit-alpha");
        TASK_CHECK(root[AT::name].get_form() == dwarf::DW_FORM::strx1);
        TASK_CHECK(root[AT::producer].as_string() == "producer-x");
        TASK_CHECK(root[AT::producer].get_form() == dwarf::DW_FORM::strx);
        TASK_CHECK(root[AT::low_pc].as_address() == 0x401000);
        TASK_CHECK(root[AT::low_pc].get_form() == dwarf::DW_FORM::addrx);
        size_t n = 0;
        const char *p = root[AT::name].as_cstr(&n);
        TASK_CHECK(n == 10 && std::string(p, n) == "unit-alpha");
        TASK_CHECK(root[AT::str_offsets_base].as_sec_offset() == 8);
        TASK_CHECK(root[AT::addr_base].as_sec_offset() == 0);

        // attributes() and operator[] must agree on every attribute
        auto attrs = root.attributes();
        TASK_CHECK(attrs.size() == 5);
        for (auto &a : attrs) {
                dwarf::value v = root[a.first];
                TASK_CHECK(a.second.get_form() == v.get_form());
                TASK_CHECK(a.second.get_unit_offset() == v.get_unit_offset());
        }

        // Children cover the 1/2/3/4-byte forms of both families
        auto it = root.begin();
        TASK_CHECK(it != root.end());
        dwarf::die c1 = *it;
        TASK_CHECK(c1[AT::name].as_string() == "func-alpha");   // strx3
        TASK_CHECK(c1[AT::name].get_form() == dwarf::DW_FORM::strx3);
        TASK_CHECK(c1[AT::low_pc].as_address() == 0x402000);    // addrx4
        ++it;
        dwarf::die c2 = *it;
        TASK_CHECK(c2[AT::name].as_string() == "var-alpha");    // strx2
        TASK_CHECK(c2[AT::low_pc].as_address() == 0x403000);    // addrx1
        ++it;
        dwarf::die c3 = *it;
        TASK_CHECK(c3[AT::name].as_string() == "producer-x");   // strx4
        TASK_CHECK(c3[AT::low_pc].as_address() == 0x404000);    // addrx2
        ++it;
        TASK_CHECK(!(it != root.end()));
        return 0;
}

int
case_late_base_attributes()
{
        // The root DIE declares name (strx1) and low_pc (addrx)
        // before str_offsets_base/addr_base; reading them must not
        // require the caller to have touched the base attributes.
        session s(main_sections());
        dwarf::die root = s.dw.compilation_units().at(1).root();

        auto attrs = root.attributes();
        TASK_CHECK(attrs.size() == 5);
        TASK_CHECK(attrs[0].first == AT::name);
        TASK_CHECK(attrs[2].first == AT::low_pc);
        TASK_CHECK(attrs[3].first == AT::str_offsets_base);
        TASK_CHECK(attrs[4].first == AT::addr_base);

        // First operations on this image: indexed attribute reads.
        TASK_CHECK(root[AT::name].as_string() == "unit-beta");
        TASK_CHECK(root[AT::low_pc].as_address() == 0x501000);
        TASK_CHECK(root[AT::producer].as_string() == "producer-y");
        // The bases were resolved from the trailing attributes.
        TASK_CHECK(root[AT::str_offsets_base].as_sec_offset() == 32);
        TASK_CHECK(root[AT::addr_base].as_sec_offset() == 32);
        return 0;
}

int
case_indirect_indexed()
{
        Bytes ab;
        // Code 1: compile_unit with indirect-wrapped forms
        uleb(ab, 1); uleb(ab, 0x11); put(ab, 1, 1);
        uleb(ab, 0x03); uleb(ab, 0x16);         // name, indirect
        uleb(ab, 0x11); uleb(ab, 0x16);         // low_pc, indirect
        uleb(ab, 0x72); uleb(ab, 0x17);         // str_offsets_base, sec_offset
        uleb(ab, 0x73); uleb(ab, 0x17);         // addr_base, sec_offset
        uleb(ab, 0); uleb(ab, 0);
        // Code 2: subprogram
        uleb(ab, 2); uleb(ab, 0x2e); put(ab, 0, 1);
        uleb(ab, 0x03); uleb(ab, 0x16);         // name, indirect
        uleb(ab, 0); uleb(ab, 0);
        uleb(ab, 0);

        Bytes info = begin_cu5(8, 0);
        uleb(info, 1);
        uleb(info, 0x26); put(info, 1, 2);      // indirect strx2, index 1
        uleb(info, 0x2b); put(info, 0, 1);      // indirect addrx1, index 0
        put(info, 0, 4);                        // str_offsets_base
        put(info, 0, 4);                        // addr_base
        uleb(info, 2);
        uleb(info, 0x1a); uleb(info, 2);        // indirect strx, index 2
        uleb(info, 0);
        finish_cu(info);

        Bytes str;
        text(str, "indirect-unit");             // 0
        text(str, "via-indirect");              // 14
        text(str, "child-fn");                  // 27
        Bytes stroff;
        put(stroff, 0, 4); put(stroff, 14, 4); put(stroff, 27, 4);
        Bytes addr;
        put(addr, 0x600000, 8);

        session s({{".debug_info", info},
                   {".debug_abbrev", ab},
                   {".debug_str", str},
                   {".debug_str_offsets", stroff},
                   {".debug_addr", addr}});
        dwarf::die root = s.dw.compilation_units().at(0).root();
        // get_form resolves the indirect encoding to the actual form
        TASK_CHECK(root[AT::name].get_form() == dwarf::DW_FORM::strx2);
        TASK_CHECK(root[AT::name].as_string() == "via-indirect");
        TASK_CHECK(root[AT::low_pc].get_form() == dwarf::DW_FORM::addrx1);
        TASK_CHECK(root[AT::low_pc].as_address() == 0x600000);
        auto it = root.begin();
        TASK_CHECK(it != root.end());
        TASK_CHECK((*it)[AT::name].get_form() == dwarf::DW_FORM::strx);
        TASK_CHECK((*it)[AT::name].as_string() == "child-fn");
        ++it;
        TASK_CHECK(!(it != root.end()));
        return 0;
}

int
case_dwarf64_addr4()
{
        // DWARF64 unit with 4-byte addresses: .debug_str_offsets
        // entries are 8 bytes, .debug_addr entries are 4 bytes.
        Bytes ab;
        uleb(ab, 1); uleb(ab, 0x11); put(ab, 1, 1);
        uleb(ab, 0x03); uleb(ab, 0x25);         // name, strx1
        uleb(ab, 0x11); uleb(ab, 0x2b);         // low_pc, addrx1
        uleb(ab, 0x72); uleb(ab, 0x17);         // str_offsets_base, sec_offset
        uleb(ab, 0x73); uleb(ab, 0x17);         // addr_base, sec_offset
        uleb(ab, 0); uleb(ab, 0);
        uleb(ab, 2); uleb(ab, 0x2e); put(ab, 0, 1);
        uleb(ab, 0x03); uleb(ab, 0x28);         // name, strx4
        uleb(ab, 0x11); uleb(ab, 0x2e);         // low_pc, addrx4
        uleb(ab, 0); uleb(ab, 0);
        uleb(ab, 0);

        Bytes info;
        put(info, 0xffffffff, 4);               // DWARF64 marker
        put(info, 0, 8);                        // unit_length, patched below
        put(info, 5, 2);
        put(info, 1, 1);
        put(info, 4, 1);                        // 4-byte addresses
        put(info, 0, 8);                        // abbrev offset
        uleb(info, 1);
        put(info, 0, 1);                        // strx1 index 0
        put(info, 0, 1);                        // addrx1 index 0
        put(info, 8, 8);                        // str_offsets_base (8 bytes)
        put(info, 0, 8);                        // addr_base
        uleb(info, 2);
        put(info, 1, 4);                        // strx4 index 1
        put(info, 1, 4);                        // addrx4 index 1
        uleb(info, 0);
        patch(info, 4, info.size() - 12, 8);

        Bytes str;
        text(str, "wide-unit");                 // 0
        text(str, "wide-fn");                   // 10
        Bytes stroff;
        put(stroff, 0, 8);                      // header
        put(stroff, 0, 8); put(stroff, 10, 8);  // 8-byte entries
        Bytes addr;
        put(addr, 0x80004000, 4); put(addr, 0x80005000, 4);

        session s({{".debug_info", info},
                   {".debug_abbrev", ab},
                   {".debug_str", str},
                   {".debug_str_offsets", stroff},
                   {".debug_addr", addr}});
        auto &cus = s.dw.compilation_units();
        TASK_CHECK(cus.size() == 1);
        dwarf::die root = cus.at(0).root();
        TASK_CHECK(root[AT::name].as_string() == "wide-unit");
        TASK_CHECK(root[AT::low_pc].as_address() == 0x80004000);
        TASK_CHECK(root[AT::str_offsets_base].as_sec_offset() == 8);
        auto it = root.begin();
        TASK_CHECK(it != root.end());
        TASK_CHECK((*it)[AT::name].as_string() == "wide-fn");
        TASK_CHECK((*it)[AT::low_pc].as_address() == 0x80005000);
        ++it;
        TASK_CHECK(!(it != root.end()));
        return 0;
}

int
case_big_endian()
{
        // Big-endian DWARF5 unit: indices, table entries, offsets
        // and addresses are all most-significant-byte first.
        Bytes ab;
        uleb(ab, 1); uleb(ab, 0x11); put(ab, 1, 1);
        uleb(ab, 0x03); uleb(ab, 0x26);         // name, strx2
        uleb(ab, 0x11); uleb(ab, 0x2d);         // low_pc, addrx3
        uleb(ab, 0x72); uleb(ab, 0x17);         // str_offsets_base, sec_offset
        uleb(ab, 0x73); uleb(ab, 0x17);         // addr_base, sec_offset
        uleb(ab, 0); uleb(ab, 0);
        uleb(ab, 2); uleb(ab, 0x2e); put(ab, 0, 1);
        uleb(ab, 0x03); uleb(ab, 0x27);         // name, strx3
        uleb(ab, 0x11); uleb(ab, 0x2c);         // low_pc, addrx2
        uleb(ab, 0); uleb(ab, 0);
        uleb(ab, 0);

        Bytes info;
        putbe(info, 0, 4);                      // unit_length, patched below
        putbe(info, 5, 2);
        put(info, 1, 1);
        put(info, 8, 1);
        putbe(info, 0, 4);
        uleb(info, 1);
        putbe(info, 0, 2);                      // strx2 index 0
        putbe(info, 1, 3);                      // addrx3 index 1
        putbe(info, 8, 4);                      // str_offsets_base
        putbe(info, 0, 4);                      // addr_base
        uleb(info, 2);
        putbe(info, 1, 3);                      // strx3 index 1
        putbe(info, 0, 2);                      // addrx2 index 0
        uleb(info, 0);
        patch(info, 0, info.size() - 4, 4);
        // patch() is little-endian; rewrite the length big-endian
        info[0] = 0; info[1] = 0; info[2] = 0;
        info[3] = (unsigned char)(info.size() - 4);

        Bytes str;
        text(str, "big-unit");                  // 0
        text(str, "big-fn");                    // 9
        Bytes stroff;
        putbe(stroff, 0, 8);                    // header
        putbe(stroff, 0, 4); putbe(stroff, 9, 4);
        Bytes addr;
        putbe(addr, 0xA0B0C0D0, 8); putbe(addr, 0x11223344, 8);

        session s({{".debug_info", info},
                   {".debug_abbrev", ab},
                   {".debug_str", str},
                   {".debug_str_offsets", stroff},
                   {".debug_addr", addr}});
        dwarf::die root = s.dw.compilation_units().at(0).root();
        TASK_CHECK(root[AT::name].as_string() == "big-unit");
        TASK_CHECK(root[AT::low_pc].as_address() == 0x11223344);
        auto it = root.begin();
        TASK_CHECK(it != root.end());
        TASK_CHECK((*it)[AT::name].as_string() == "big-fn");
        TASK_CHECK((*it)[AT::low_pc].as_address() == 0xA0B0C0D0);
        ++it;
        TASK_CHECK(!(it != root.end()));
        return 0;
}

int
case_default_bases()
{
        // Without str_offsets_base/addr_base attributes, the tables
        // are read from offset 0.
        Bytes ab;
        uleb(ab, 1); uleb(ab, 0x11); put(ab, 0, 1);
        uleb(ab, 0x03); uleb(ab, 0x25);         // name, strx1
        uleb(ab, 0x11); uleb(ab, 0x2b);         // low_pc, addrx1
        uleb(ab, 0); uleb(ab, 0);
        uleb(ab, 0);

        Bytes info = begin_cu5(8, 0);
        uleb(info, 1);
        put(info, 0, 1);
        put(info, 0, 1);
        finish_cu(info);

        Bytes str;
        text(str, "solo");
        Bytes stroff;
        put(stroff, 0, 4);
        Bytes addr;
        put(addr, 0x777000, 8);

        session s({{".debug_info", info},
                   {".debug_abbrev", ab},
                   {".debug_str", str},
                   {".debug_str_offsets", stroff},
                   {".debug_addr", addr}});
        dwarf::die root = s.dw.compilation_units().at(0).root();
        TASK_CHECK(!root.has(AT::str_offsets_base));
        TASK_CHECK(!root.has(AT::addr_base));
        TASK_CHECK(root[AT::name].as_string() == "solo");
        TASK_CHECK(root[AT::low_pc].as_address() == 0x777000);
        return 0;
}

int
case_table_errors()
{
        // Missing .debug_str_offsets
        {
                Bytes ab;
                uleb(ab, 1); uleb(ab, 0x11); put(ab, 0, 1);
                uleb(ab, 0x03); uleb(ab, 0x25); // name, strx1
                uleb(ab, 0x11); uleb(ab, 0x2b); // low_pc, addrx1
                uleb(ab, 0); uleb(ab, 0);
                uleb(ab, 0);
                Bytes info = begin_cu5(8, 0);
                uleb(info, 1); put(info, 0, 1); put(info, 0, 1);
                finish_cu(info);
                Bytes str;
                text(str, "x");
                session s({{".debug_info", info},
                           {".debug_abbrev", ab},
                           {".debug_str", str}});
                dwarf::die root = s.dw.compilation_units().at(0).root();
                TASK_CHECK(throws_std([&] { root[AT::name].as_string(); }));
                TASK_CHECK(throws_std([&] { root[AT::low_pc].as_address(); }));
        }
        // Index past the end of the unit's table contribution
        {
                Bytes ab;
                uleb(ab, 1); uleb(ab, 0x11); put(ab, 0, 1);
                uleb(ab, 0x03); uleb(ab, 0x25); // name, strx1
                uleb(ab, 0); uleb(ab, 0);
                uleb(ab, 0);
                Bytes info = begin_cu5(8, 0);
                uleb(info, 1); put(info, 5, 1); // index 5, table has 1 entry
                finish_cu(info);
                Bytes str;
                text(str, "x");
                Bytes stroff;
                put(stroff, 0, 4);
                session s({{".debug_info", info},
                           {".debug_abbrev", ab},
                           {".debug_str", str},
                           {".debug_str_offsets", stroff}});
                dwarf::die root = s.dw.compilation_units().at(0).root();
                TASK_CHECK(throws_std([&] { root[AT::name].as_string(); }));
        }
        // String offset recorded in the table escapes .debug_str
        {
                Bytes ab;
                uleb(ab, 1); uleb(ab, 0x11); put(ab, 0, 1);
                uleb(ab, 0x03); uleb(ab, 0x25); // name, strx1
                uleb(ab, 0); uleb(ab, 0);
                uleb(ab, 0);
                Bytes info = begin_cu5(8, 0);
                uleb(info, 1); put(info, 0, 1);
                finish_cu(info);
                Bytes str;
                text(str, "x");
                Bytes stroff;
                put(stroff, 0x1000, 4);
                session s({{".debug_info", info},
                           {".debug_abbrev", ab},
                           {".debug_str", str},
                           {".debug_str_offsets", stroff}});
                dwarf::die root = s.dw.compilation_units().at(0).root();
                TASK_CHECK(throws_std([&] { root[AT::name].as_string(); }));
        }
        // Truncated index: strx2 with only one byte left in the unit
        {
                Bytes ab;
                uleb(ab, 1); uleb(ab, 0x11); put(ab, 0, 1);
                uleb(ab, 0x03); uleb(ab, 0x26); // name, strx2
                uleb(ab, 0); uleb(ab, 0);
                uleb(ab, 0);
                Bytes info = begin_cu5(8, 0);
                uleb(info, 1); put(info, 0, 1); // only 1 of 2 index bytes
                finish_cu(info);
                Bytes str;
                text(str, "x");
                Bytes stroff;
                put(stroff, 0, 4);
                session s({{".debug_info", info},
                           {".debug_abbrev", ab},
                           {".debug_str", str},
                           {".debug_str_offsets", stroff}});
                dwarf::die root = s.dw.compilation_units().at(0).root();
                TASK_CHECK(throws_std([&] { root[AT::name].as_string(); }));
        }
        return 0;
}

//////////////////////////////////////////////////////////////////
// constants group

int
case_implicit_const()
{
        Bytes ab;
        // Code 1: compile_unit
        uleb(ab, 1); uleb(ab, 0x11); put(ab, 1, 1);
        uleb(ab, 0x03); uleb(ab, 0x08);         // name, string
        uleb(ab, 0); uleb(ab, 0);
        // Code 2: DW_TAG_constant; a negative implicit constant
        // immediately followed by an ordinary field
        uleb(ab, 2); uleb(ab, 0x27); put(ab, 0, 1);
        uleb(ab, 0x03); uleb(ab, 0x08);         // name, string
        uleb(ab, 0x1c); uleb(ab, 0x21);         // const_value, implicit_const
        sleb(ab, -42);
        uleb(ab, 0x3b); uleb(ab, 0x0b);         // decl_line, data1
        uleb(ab, 0); uleb(ab, 0);
        // Code 3: DW_TAG_variable mixing indexed, implicit and
        // ordinary fields in one abbrev entry
        uleb(ab, 3); uleb(ab, 0x34); put(ab, 0, 1);
        uleb(ab, 0x03); uleb(ab, 0x25);         // name, strx1
        uleb(ab, 0x1c); uleb(ab, 0x21);         // const_value, implicit_const
        sleb(ab, -7);
        uleb(ab, 0x3b); uleb(ab, 0x0b);         // decl_line, data1
        uleb(ab, 0x39); uleb(ab, 0x21);         // decl_column, implicit_const
        sleb(ab, 100);
        uleb(ab, 0x3a); uleb(ab, 0x0b);         // decl_file, data1
        uleb(ab, 0); uleb(ab, 0);
        uleb(ab, 0);

        Bytes info = begin_cu5(8, 0);
        uleb(info, 1);
        text(info, "const-unit");
        size_t child1_off = info.size();
        uleb(info, 2);
        text(info, "the-answer");
        put(info, 7, 1);                        // decl_line
        size_t child2_off = info.size();
        uleb(info, 3);
        put(info, 0, 1);                        // strx1 index 0
        put(info, 9, 1);                        // decl_line
        put(info, 2, 1);                        // decl_file
        uleb(info, 0);
        finish_cu(info);

        Bytes str;
        text(str, "mixed-var");
        Bytes stroff;
        put(stroff, 0, 4);

        session s({{".debug_info", info},
                   {".debug_abbrev", ab},
                   {".debug_str", str},
                   {".debug_str_offsets", stroff}});
        dwarf::die root = s.dw.compilation_units().at(0).root();
        TASK_CHECK(root[AT::name].as_string() == "const-unit");

        auto it = root.begin();
        TASK_CHECK(it != root.end());
        dwarf::die c1 = *it;
        TASK_CHECK(c1.tag == dwarf::DW_TAG::constant);
        TASK_CHECK(c1.get_unit_offset() == child1_off);
        TASK_CHECK(c1[AT::name].as_string() == "the-answer");
        TASK_CHECK(c1[AT::const_value].get_form() == dwarf::DW_FORM::implicit_const);
        TASK_CHECK(c1[AT::const_value].get_type() == dwarf::value::type::sconstant);
        TASK_CHECK(c1[AT::const_value].as_sconstant() == -42);
        // The ordinary field right after the implicit constant
        TASK_CHECK(c1[AT::decl_line].as_uconstant() == 7);

        ++it;
        TASK_CHECK(it != root.end());
        dwarf::die c2 = *it;
        // The implicit constants occupied no DIE bytes, so the next
        // DIE begins exactly where it was placed
        TASK_CHECK(c2.get_unit_offset() == child2_off);
        TASK_CHECK(c2[AT::name].as_string() == "mixed-var");
        TASK_CHECK(c2[AT::const_value].as_sconstant() == -7);
        TASK_CHECK(c2[AT::decl_line].as_uconstant() == 9);
        TASK_CHECK(c2[AT::decl_column].as_sconstant() == 100);
        TASK_CHECK(c2[AT::decl_file].as_uconstant() == 2);

        // attributes() must see the same five attributes with the
        // same values as operator[]
        auto attrs = c2.attributes();
        TASK_CHECK(attrs.size() == 5);
        TASK_CHECK(attrs[0].first == AT::name &&
                   attrs[0].second.as_string() == "mixed-var");
        TASK_CHECK(attrs[1].first == AT::const_value &&
                   attrs[1].second.as_sconstant() == -7);
        TASK_CHECK(attrs[2].first == AT::decl_line &&
                   attrs[2].second.as_uconstant() == 9);
        TASK_CHECK(attrs[3].first == AT::decl_column &&
                   attrs[3].second.as_sconstant() == 100);
        TASK_CHECK(attrs[4].first == AT::decl_file &&
                   attrs[4].second.as_uconstant() == 2);

        ++it;
        TASK_CHECK(!(it != root.end()));
        return 0;
}

int
case_implicit_const_truncated()
{
        // The abbrev declares an implicit_const whose SLEB128 value
        // is cut off by the end of the section.
        Bytes ab;
        uleb(ab, 1); uleb(ab, 0x11); put(ab, 0, 1);
        uleb(ab, 0x03); uleb(ab, 0x21);         // name, implicit_const
        ab.push_back(0x80);                     // unterminated SLEB128

        Bytes info = begin_cu5(8, 0);
        uleb(info, 1);
        finish_cu(info);

        session s({{".debug_info", info},
                   {".debug_abbrev", ab}});
        auto &cus = s.dw.compilation_units();
        TASK_CHECK(cus.size() == 1);
        TASK_CHECK(throws_std([&] { cus.at(0).root(); }));
        return 0;
}

//////////////////////////////////////////////////////////////////
// context group

int
case_two_cu_interleaved()
{
        session s(main_sections());
        auto &cus = s.dw.compilation_units();
        TASK_CHECK(cus.size() == 2);
        auto &cu1 = cus.at(0);
        auto &cu2 = cus.at(1);

        // Interleave reads between the two units; each must keep
        // using its own table contributions.
        TASK_CHECK(cu1.root()[AT::name].as_string() == "unit-alpha");
        TASK_CHECK(cu2.root()[AT::name].as_string() == "unit-beta");
        TASK_CHECK(cu1.root()[AT::producer].as_string() == "producer-x");
        TASK_CHECK(cu2.root()[AT::producer].as_string() == "producer-y");
        TASK_CHECK(cu1.root()[AT::low_pc].as_address() == 0x401000);
        TASK_CHECK(cu2.root()[AT::low_pc].as_address() == 0x501000);

        dwarf::die a1 = *cu1.root().begin();
        dwarf::die b1 = *cu2.root().begin();
        TASK_CHECK(a1[AT::name].as_string() == "func-alpha");
        TASK_CHECK(b1[AT::name].as_string() == "func-beta");
        TASK_CHECK(a1[AT::low_pc].as_address() == 0x402000);
        TASK_CHECK(b1[AT::low_pc].as_address() == 0x502000);

        // Same table index (1) in both units, different strings
        auto it1 = cu1.root().begin();
        ++it1; ++it1;
        dwarf::die a3 = *it1;
        auto it2 = cu2.root().begin();
        ++it2; ++it2;
        dwarf::die b3 = *it2;
        TASK_CHECK(a3[AT::name].as_string() == "producer-x");
        TASK_CHECK(b3[AT::name].as_string() == "producer-y");
        TASK_CHECK(a3[AT::low_pc].as_address() == 0x404000);
        TASK_CHECK(b3[AT::low_pc].as_address() == 0x504000);

        // Re-reading after the interleaving still gives each unit's
        // own values
        TASK_CHECK(cu1.root()[AT::name].as_string() == "unit-alpha");
        TASK_CHECK(cu2.root()[AT::name].as_string() == "unit-beta");
        return 0;
}

int
case_resolve_cross_unit()
{
        Bytes ab;
        // Code 1: compile_unit
        uleb(ab, 1); uleb(ab, 0x11); put(ab, 1, 1);
        uleb(ab, 0x03); uleb(ab, 0x25);         // name, strx1
        uleb(ab, 0x72); uleb(ab, 0x17);         // str_offsets_base, sec_offset
        uleb(ab, 0x73); uleb(ab, 0x17);         // addr_base, sec_offset
        uleb(ab, 0); uleb(ab, 0);
        // Code 2: subprogram with a name and address
        uleb(ab, 2); uleb(ab, 0x2e); put(ab, 0, 1);
        uleb(ab, 0x03); uleb(ab, 0x25);         // name, strx1
        uleb(ab, 0x11); uleb(ab, 0x2b);         // low_pc, addrx1
        uleb(ab, 0); uleb(ab, 0);
        // Code 3: subprogram with only a specification reference
        uleb(ab, 3); uleb(ab, 0x2e); put(ab, 0, 1);
        uleb(ab, 0x47); uleb(ab, 0x10);         // specification, ref_addr
        uleb(ab, 0); uleb(ab, 0);
        uleb(ab, 0);

        // CU1 provides the referenced DIE
        Bytes cu1 = begin_cu5(8, 0);
        uleb(cu1, 1);
        put(cu1, 0, 1);                         // name index 0
        put(cu1, 8, 4);                         // str_offsets_base
        put(cu1, 0, 4);                         // addr_base
        size_t func_off = cu1.size();           // absolute: CU1 starts at 0
        uleb(cu1, 2);
        put(cu1, 1, 1);                         // name index 1
        put(cu1, 0, 1);                         // low_pc index 0
        uleb(cu1, 0);
        finish_cu(cu1);

        // CU2 refers across units
        Bytes cu2 = begin_cu5(8, 0);
        uleb(cu2, 1);
        put(cu2, 0, 1);                         // name index 0
        put(cu2, 24, 4);                        // str_offsets_base
        put(cu2, 8, 4);                         // addr_base
        uleb(cu2, 3);
        put(cu2, func_off, 4);                  // ref_addr to CU1's func
        uleb(cu2, 0);
        finish_cu(cu2);

        Bytes info = cu1;
        info.insert(info.end(), cu2.begin(), cu2.end());

        Bytes str;
        text(str, "unit-alpha");                // 0
        text(str, "func-alpha");                // 11
        text(str, "unit-beta");                 // 22
        Bytes stroff;
        put(stroff, 0, 8);                      // CU1 header
        put(stroff, 0, 4); put(stroff, 11, 4);  // CU1 entries @8
        put(stroff, 0, 8);                      // CU2 header
        put(stroff, 22, 4);                     // CU2 entries @24
        Bytes addr;
        put(addr, 0x401000, 8);                 // CU1 @0

        session s({{".debug_info", info},
                   {".debug_abbrev", ab},
                   {".debug_str", str},
                   {".debug_str_offsets", stroff},
                   {".debug_addr", addr}});
        auto &cus = s.dw.compilation_units();
        TASK_CHECK(cus.size() == 2);
        TASK_CHECK(cus.at(0).root()[AT::name].as_string() == "unit-alpha");
        TASK_CHECK(cus.at(1).root()[AT::name].as_string() == "unit-beta");

        auto it = cus.at(1).root().begin();
        TASK_CHECK(it != cus.at(1).root().end());
        dwarf::die spec = *it;
        TASK_CHECK(!spec.has(AT::name));
        // resolve follows the specification into CU1; the value must
        // be interpreted with CU1's tables, not CU2's
        dwarf::value n = spec.resolve(AT::name);
        TASK_CHECK(n.valid());
        TASK_CHECK(n.as_string() == "func-alpha");
        dwarf::value pc = spec.resolve(AT::low_pc);
        TASK_CHECK(pc.valid());
        TASK_CHECK(pc.as_address() == 0x401000);
        TASK_CHECK(!spec.resolve(AT::producer).valid());
        return 0;
}

//////////////////////////////////////////////////////////////////
// legacy group

int
case_dwarf4_baseline()
{
        // The pre-existing baseline image must keep working
        // unchanged through the same interfaces.
        session s({{".debug_info", fixture::baseline_info()},
                   {".debug_abbrev", fixture::baseline_abbrev()}});
        auto &cu = s.dw.compilation_units().at(0);
        dwarf::die root = cu.root();
        TASK_CHECK(root[AT::name].as_string() == "baseline-unit");
        auto it = root.begin();
        TASK_CHECK(it != root.end());
        dwarf::die child = *it;
        TASK_CHECK(child[AT::name].as_string() == "entry");
        TASK_CHECK(child[AT::low_pc].as_address() == 0x401020);
        TASK_CHECK(child[AT::high_pc].as_uconstant() == 0x18);
        ++it;
        TASK_CHECK(!(it != root.end()));
        return 0;
}

int
case_dwarf4_strp_addr()
{
        // DWARF4 unit using direct strings, strp, addr and data
        // forms; none of the DWARF5 machinery may interfere.
        Bytes ab;
        uleb(ab, 1); uleb(ab, 0x11); put(ab, 1, 1);
        uleb(ab, 0x03); uleb(ab, 0x0e);         // name, strp
        uleb(ab, 0x1b); uleb(ab, 0x08);         // comp_dir, string
        uleb(ab, 0); uleb(ab, 0);
        uleb(ab, 2); uleb(ab, 0x2e); put(ab, 0, 1);
        uleb(ab, 0x03); uleb(ab, 0x08);         // name, string
        uleb(ab, 0x11); uleb(ab, 0x01);         // low_pc, addr
        uleb(ab, 0x12); uleb(ab, 0x06);         // high_pc, data4
        uleb(ab, 0); uleb(ab, 0);
        uleb(ab, 0);

        Bytes info;
        put(info, 0, 4);                        // unit_length, patched below
        put(info, 4, 2);                        // DWARF version 4
        put(info, 0, 4);                        // abbrev offset
        put(info, 8, 1);                        // address size
        uleb(info, 1);
        put(info, 0, 4);                        // strp offset 0
        text(info, "/tmp");
        uleb(info, 2);
        text(info, "legacy-fn");
        put(info, 0x404000, 8);
        put(info, 0x20, 4);
        uleb(info, 0);
        patch(info, 0, info.size() - 4, 4);

        Bytes str;
        text(str, "legacy-unit");

        session s({{".debug_info", info},
                   {".debug_abbrev", ab},
                   {".debug_str", str}});
        dwarf::die root = s.dw.compilation_units().at(0).root();
        TASK_CHECK(root[AT::name].as_string() == "legacy-unit");
        TASK_CHECK(root[AT::name].get_form() == dwarf::DW_FORM::strp);
        TASK_CHECK(root[AT::comp_dir].as_string() == "/tmp");
        auto it = root.begin();
        TASK_CHECK(it != root.end());
        dwarf::die child = *it;
        TASK_CHECK(child[AT::name].as_string() == "legacy-fn");
        TASK_CHECK(child[AT::low_pc].as_address() == 0x404000);
        TASK_CHECK(child[AT::high_pc].as_uconstant() == 0x20);
        ++it;
        TASK_CHECK(!(it != root.end()));
        return 0;
}

} // anonymous namespace

const struct task_case task_cases[] = {
        {"indexed", "strx_addrx_forms", case_strx_addrx_forms},
        {"indexed", "late_base_attributes", case_late_base_attributes},
        {"indexed", "indirect_indexed", case_indirect_indexed},
        {"indexed", "dwarf64_addr4", case_dwarf64_addr4},
        {"indexed", "big_endian", case_big_endian},
        {"indexed", "default_bases", case_default_bases},
        {"indexed", "table_errors", case_table_errors},
        {"constants", "implicit_const", case_implicit_const},
        {"constants", "implicit_const_truncated", case_implicit_const_truncated},
        {"context", "two_cu_interleaved", case_two_cu_interleaved},
        {"context", "resolve_cross_unit", case_resolve_cross_unit},
        {"legacy", "dwarf4_baseline", case_dwarf4_baseline},
        {"legacy", "dwarf4_strp_addr", case_dwarf4_strp_addr},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
