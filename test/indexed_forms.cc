// Regression tests for DWARF 5 indexed forms (DW_FORM_strx/addrx and
// their sized variants) and DW_FORM_implicit_const.
//
// Everything is constructed as a synthetic ELF image: no image is
// executed and no compiler-generated DWARF is needed.  The cases
// exercise the public libelfin interface only (compilation_units(),
// root(), child iteration, attributes(), operator[] and resolve()).

#include "task_support/check.h"
#include "task_support/fixture.hh"
#include "dwarf/dwarf++.hh"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

using fixture::Bytes;

namespace {

// ---------------------------------------------------------------
// Byte encoding helpers (independent of the host endianness).
// ---------------------------------------------------------------

void put_be(Bytes &b, std::uint64_t v, unsigned n)
{
        for (unsigned i = 0; i < n; ++i)
                b.push_back((v >> (8 * (n - 1 - i))) & 255);
}

void patch_be(Bytes &b, size_t at, std::uint64_t v, unsigned n)
{
        for (unsigned i = 0; i < n; ++i)
                b.at(at + i) = (v >> (8 * (n - 1 - i))) & 255;
}

void w_put(Bytes &b, std::uint64_t v, unsigned n, bool be)
{
        if (be)
                put_be(b, v, n);
        else
                fixture::put(b, v, n);
}

void w_patch(Bytes &b, size_t at, std::uint64_t v, unsigned n, bool be)
{
        if (be)
                patch_be(b, at, v, n);
        else
                fixture::patch(b, at, v, n);
}

void uleb(Bytes &b, std::uint64_t v)
{
        do {
                std::uint8_t byte = v & 0x7f;
                v >>= 7;
                if (v)
                        byte |= 0x80;
                b.push_back(byte);
        } while (v);
}

void sleb(Bytes &b, std::int64_t v)
{
        bool more = true;
        while (more) {
                std::uint8_t byte = v & 0x7f;
                v >>= 7;
                if ((v == 0 && !(byte & 0x40)) ||
                    (v == -1 && (byte & 0x40)))
                        more = false;
                else
                        byte |= 0x80;
                b.push_back(byte);
        }
}

size_t uleb_len(std::uint64_t v)
{
        size_t n = 1;
        while (v >= 0x80) {
                v >>= 7;
                ++n;
        }
        return n;
}

// ---------------------------------------------------------------
// DWARF / DWARF 5 encodings used by these fixtures.  The tag and
// attribute/form values are typed so they can be compared with the
// library enums; the encoder casts them to their on-disk numbers.
// ---------------------------------------------------------------

const std::uint32_t TAG_compile_unit = (std::uint32_t)dwarf::DW_TAG::compile_unit;
const std::uint32_t TAG_subprogram   = (std::uint32_t)dwarf::DW_TAG::subprogram;
const std::uint32_t TAG_variable     = (std::uint32_t)dwarf::DW_TAG::variable;

const dwarf::DW_AT AT_name                 = dwarf::DW_AT::name;
const dwarf::DW_AT AT_byte_size            = dwarf::DW_AT::byte_size;
const dwarf::DW_AT AT_low_pc               = dwarf::DW_AT::low_pc;
const dwarf::DW_AT AT_high_pc              = dwarf::DW_AT::high_pc;
const dwarf::DW_AT AT_abstract_origin      = dwarf::DW_AT::abstract_origin;
const dwarf::DW_AT AT_data_member_location = dwarf::DW_AT::data_member_location;
const dwarf::DW_AT AT_decl_column          = dwarf::DW_AT::decl_column;
const dwarf::DW_AT AT_decl_line            = dwarf::DW_AT::decl_line;
const dwarf::DW_AT AT_str_offsets_base     = dwarf::DW_AT::str_offsets_base;
const dwarf::DW_AT AT_addr_base            = dwarf::DW_AT::addr_base;

const dwarf::DW_FORM FORM_addr           = dwarf::DW_FORM::addr;
const dwarf::DW_FORM FORM_data2          = dwarf::DW_FORM::data2;
const dwarf::DW_FORM FORM_data4          = dwarf::DW_FORM::data4;
const dwarf::DW_FORM FORM_string         = dwarf::DW_FORM::string;
const dwarf::DW_FORM FORM_data1          = dwarf::DW_FORM::data1;
const dwarf::DW_FORM FORM_strp           = dwarf::DW_FORM::strp;
const dwarf::DW_FORM FORM_ref_addr       = dwarf::DW_FORM::ref_addr;
const dwarf::DW_FORM FORM_indirect       = dwarf::DW_FORM::indirect;
const dwarf::DW_FORM FORM_sec_offset     = dwarf::DW_FORM::sec_offset;
const dwarf::DW_FORM FORM_strx           = dwarf::DW_FORM::strx;
const dwarf::DW_FORM FORM_addrx          = dwarf::DW_FORM::addrx;
const dwarf::DW_FORM FORM_implicit_const = dwarf::DW_FORM::implicit_const;
const dwarf::DW_FORM FORM_strx1          = dwarf::DW_FORM::strx1;
const dwarf::DW_FORM FORM_strx2          = dwarf::DW_FORM::strx2;
const dwarf::DW_FORM FORM_strx3          = dwarf::DW_FORM::strx3;
const dwarf::DW_FORM FORM_strx4          = dwarf::DW_FORM::strx4;
const dwarf::DW_FORM FORM_addrx1         = dwarf::DW_FORM::addrx1;
const dwarf::DW_FORM FORM_addrx2         = dwarf::DW_FORM::addrx2;
const dwarf::DW_FORM FORM_addrx3         = dwarf::DW_FORM::addrx3;
const dwarf::DW_FORM FORM_addrx4         = dwarf::DW_FORM::addrx4;

struct AttrSpec
{
        dwarf::DW_AT at;
        dwarf::DW_FORM form;
        std::int64_t implicit_value;
};

AttrSpec A(dwarf::DW_AT at, dwarf::DW_FORM form, std::int64_t iv = 0)
{
        AttrSpec s = { at, form, iv };
        return s;
}

void abbrev_entry(Bytes &b, std::uint32_t code, std::uint32_t tag,
                  bool children, const std::vector<AttrSpec> &attrs)
{
        uleb(b, code);
        uleb(b, tag);
        b.push_back(children ? 1 : 0);
        for (size_t i = 0; i < attrs.size(); ++i) {
                uleb(b, (std::uint32_t)attrs[i].at);
                uleb(b, (std::uint32_t)attrs[i].form);
                if (attrs[i].form == dwarf::DW_FORM::implicit_const)
                        sleb(b, attrs[i].implicit_value);
        }
        b.push_back(0);
        b.push_back(0);
}

void abbrev_end(Bytes &b)
{
        b.push_back(0);
}

// Size in bytes of an index of the given indexed form.
unsigned index_width(dwarf::DW_FORM form)
{
        switch (form) {
        case dwarf::DW_FORM::strx1:
        case dwarf::DW_FORM::addrx1:
                return 1;
        case dwarf::DW_FORM::strx2:
        case dwarf::DW_FORM::addrx2:
                return 2;
        case dwarf::DW_FORM::strx3:
        case dwarf::DW_FORM::addrx3:
                return 3;
        case dwarf::DW_FORM::strx4:
        case dwarf::DW_FORM::addrx4:
                return 4;
        default:
                return 0; // ULEB128
        }
}

void put_index(Bytes &b, dwarf::DW_FORM form, std::uint64_t index, bool be)
{
        unsigned width = index_width(form);
        if (width)
                w_put(b, index, width, be);
        else
                uleb(b, index);
}


// Add a string to .debug_str and return its offset.
size_t add_string(Bytes &strs, const char *s)
{
        size_t off = strs.size();
        fixture::text(strs, s);
        return off;
}

// Append a .debug_str_offsets contribution.  Returns the offset of
// its first table entry (what DW_AT_str_offsets_base points at).
size_t add_stroff_contrib(Bytes &sec, const std::vector<std::uint64_t> &offs,
                          bool dwarf64, bool be)
{
        size_t start = sec.size();
        if (dwarf64) {
                w_put(sec, 0xffffffff, 4, be);
                w_put(sec, 0, 8, be);
        } else {
                w_put(sec, 0, 4, be);
        }
        w_put(sec, 5, 2, be);   // version
        w_put(sec, 0, 2, be);   // padding
        size_t entries = sec.size();
        for (size_t i = 0; i < offs.size(); ++i)
                w_put(sec, offs[i], dwarf64 ? 8 : 4, be);
        size_t len = sec.size() - start - (dwarf64 ? 12 : 4);
        if (dwarf64)
                w_patch(sec, start + 4, len, 8, be);
        else
                w_patch(sec, start, len, 4, be);
        return entries;
}

// Append a .debug_addr contribution.  Returns the offset of its
// first table entry.
size_t add_addr_contrib(Bytes &sec, const std::vector<std::uint64_t> &addrs,
                        unsigned addr_size, bool dwarf64, bool be)
{
        size_t start = sec.size();
        if (dwarf64) {
                w_put(sec, 0xffffffff, 4, be);
                w_put(sec, 0, 8, be);
        } else {
                w_put(sec, 0, 4, be);
        }
        w_put(sec, 5, 2, be);                 // version
        sec.push_back((std::uint8_t)addr_size);
        sec.push_back(0);                     // segment selector size
        size_t entries = sec.size();
        for (size_t i = 0; i < addrs.size(); ++i)
                w_put(sec, addrs[i], addr_size, be);
        size_t len = sec.size() - start - (dwarf64 ? 12 : 4);
        if (dwarf64)
                w_patch(sec, start + 4, len, 8, be);
        else
                w_patch(sec, start, len, 4, be);
        return entries;
}

// Begin a DWARF 5 compilation unit and return its start offset.
size_t cu_begin_v5(Bytes &info, bool be, bool dwarf64,
                   unsigned addr_size, size_t abbrev_off)
{
        size_t start = info.size();
        if (dwarf64) {
                w_put(info, 0xffffffff, 4, be);
                w_put(info, 0, 8, be);
        } else {
                w_put(info, 0, 4, be);
        }
        w_put(info, 5, 2, be);                       // version
        info.push_back(1);                           // DW_UT_compile
        info.push_back((std::uint8_t)addr_size);
        w_put(info, abbrev_off, dwarf64 ? 8 : 4, be); // abbrev offset
        return start;
}

void cu_finish(Bytes &info, size_t start, bool be, bool dwarf64)
{
        size_t len = info.size() - start;
        if (dwarf64) {
                len -= 12;
                w_patch(info, start + 4, len, 8, be);
        } else {
                len -= 4;
                w_patch(info, start, len, 4, be);
        }
}

// A self-contained DWARF file built from a synthetic ELF image.
struct Handle
{
        std::shared_ptr<fixture::Loader> loader;
        ::elf::elf elf;
        dwarf::dwarf dw;
};

Handle open(const std::map<std::string, Bytes> &sections)
{
        Handle h;
        h.loader = std::make_shared<fixture::Loader>(
                fixture::elf_image(sections));
        h.elf = ::elf::elf(h.loader);
        h.dw = dwarf::dwarf(dwarf::elf::create_loader(h.elf));
        return h;
}

// Return the single child 'index' of a DIE by walking siblings.
dwarf::die nth_child(const dwarf::die &d, size_t index)
{
        dwarf::die::iterator it = d.begin();
        for (size_t i = 0; i < index; ++i)
                ++it;
        return *it;
}

} // namespace

// ===============================================================
// Group: indexed
// ===============================================================

// strx/strx1..4, addrx/addrx1..4, DW_FORM_indirect wrapping, with
// the base attributes placed after the indexed attributes in the
// root DIE (so callers never have to read them first).
static int case_indexed_forms(void)
{
        Bytes strs(1, 0); // initial NUL
        size_t s_root = add_string(strs, "indexed-root");
        size_t s_u = add_string(strs, "fn-uleb");
        size_t s_2 = add_string(strs, "fn-2");
        size_t s_3 = add_string(strs, "fn-3");
        size_t s_4 = add_string(strs, "fn-4");
        size_t s_ind = add_string(strs, "fn-indirect");

        Bytes stroff;
        size_t stroff_base = add_stroff_contrib(
                stroff, { s_root, s_u, s_2, s_3, s_4, s_ind }, false, false);

        Bytes addr;
        size_t addr_base = add_addr_contrib(
                addr, { 0x401000, 0x401100, 0x401200,
                        0x401300, 0x401400, 0x401500 }, 8, false, false);

        Bytes abbrev;
        // 1: compile unit.  name comes before str_offsets_base and
        // low_pc comes before addr_base on purpose.
        abbrev_entry(abbrev, 1, TAG_compile_unit, true,
                     { A(AT_name, FORM_strx1),
                       A(AT_str_offsets_base, FORM_sec_offset),
                       A(AT_low_pc, FORM_addrx2),
                       A(AT_addr_base, FORM_sec_offset) });
        // 2: ULEB forms
        abbrev_entry(abbrev, 2, TAG_subprogram, false,
                     { A(AT_name, FORM_strx), A(AT_low_pc, FORM_addrx) });
        // 3: two-byte string, three-byte address index
        abbrev_entry(abbrev, 3, TAG_subprogram, false,
                     { A(AT_name, FORM_strx2), A(AT_low_pc, FORM_addrx3) });
        // 4: three-byte string, one-byte address index
        abbrev_entry(abbrev, 4, TAG_subprogram, false,
                     { A(AT_name, FORM_strx3), A(AT_low_pc, FORM_addrx1) });
        // 5: four-byte forms
        abbrev_entry(abbrev, 5, TAG_subprogram, false,
                     { A(AT_name, FORM_strx4), A(AT_low_pc, FORM_addrx4) });
        // 6: indirect wrapping an indexed string and address
        abbrev_entry(abbrev, 6, TAG_subprogram, false,
                     { A(AT_name, FORM_indirect),
                       A(AT_low_pc, FORM_indirect) });
        abbrev_end(abbrev);

        Bytes info;
        size_t cu = cu_begin_v5(info, false, false, 8, 0);
        // root DIE
        uleb(info, 1);
        put_index(info, FORM_strx1, 0, false);
        w_put(info, stroff_base, 4, false);
        put_index(info, FORM_addrx2, 0, false);
        w_put(info, addr_base, 4, false);
        // child 1: ULEB
        uleb(info, 2); uleb(info, 1); uleb(info, 1);
        // child 2: strx2 / addrx3
        uleb(info, 3); put_index(info, FORM_strx2, 2, false);
        put_index(info, FORM_addrx3, 2, false);
        // child 3: strx3 / addrx1
        uleb(info, 4); put_index(info, FORM_strx3, 3, false);
        put_index(info, FORM_addrx1, 3, false);
        // child 4: strx4 / addrx4
        uleb(info, 5); put_index(info, FORM_strx4, 4, false);
        put_index(info, FORM_addrx4, 4, false);
        // child 5: indirect -> strx and indirect -> addrx
        uleb(info, 6);
        uleb(info, (std::uint32_t)FORM_strx); uleb(info, 5);
        uleb(info, (std::uint32_t)FORM_addrx); uleb(info, 5);
        uleb(info, 0); // child list terminator
        cu_finish(info, cu, false, false);

        Handle h = open({ { ".debug_info", info },
                          { ".debug_abbrev", abbrev },
                          { ".debug_str", strs },
                          { ".debug_str_offsets", stroff },
                          { ".debug_addr", addr } });
        const dwarf::compilation_unit &u = h.dw.compilation_units().at(0);
        dwarf::die root = u.root();

        TASK_CHECK(root.tag == dwarf::DW_TAG::compile_unit);
        TASK_CHECK(root[AT_name].as_string() == "indexed-root");
        TASK_CHECK(root[AT_low_pc].as_address() == 0x401000);
        TASK_CHECK(root[AT_str_offsets_base].as_sec_offset() == stroff_base);
        TASK_CHECK(root[AT_addr_base].as_sec_offset() == addr_base);

        dwarf::die d0 = nth_child(root, 0);
        TASK_CHECK(d0[AT_name].as_string() == "fn-uleb");
        TASK_CHECK(d0[AT_low_pc].as_address() == 0x401100);

        dwarf::die d1 = nth_child(root, 1);
        TASK_CHECK(d1[AT_name].as_string() == "fn-2");
        TASK_CHECK(d1[AT_low_pc].as_address() == 0x401200);

        // Three-byte index forms.
        dwarf::die d2 = nth_child(root, 2);
        TASK_CHECK(d2[AT_name].as_string() == "fn-3");
        TASK_CHECK(d2[AT_low_pc].as_address() == 0x401300);

        dwarf::die d3 = nth_child(root, 3);
        TASK_CHECK(d3[AT_name].as_string() == "fn-4");
        TASK_CHECK(d3[AT_low_pc].as_address() == 0x401400);

        // DW_FORM_indirect wrapping indexed forms.
        dwarf::die d4 = nth_child(root, 4);
        TASK_CHECK(d4[AT_name].get_form() == dwarf::DW_FORM::strx);
        TASK_CHECK(d4[AT_name].as_string() == "fn-indirect");
        TASK_CHECK(d4[AT_low_pc].get_form() == dwarf::DW_FORM::addrx);
        TASK_CHECK(d4[AT_low_pc].as_address() == 0x401500);

        // attributes() must agree with operator[].
        std::vector<std::pair<dwarf::DW_AT, dwarf::value> > av =
                d4.attributes();
        TASK_CHECK(av.size() == 2);
        TASK_CHECK(av[0].second.as_string() == d4[AT_name].as_string());
        TASK_CHECK(av[1].second.as_address() == d4[AT_low_pc].as_address());
        return 0;
}

// Big-endian DWARF64 compilation unit with a four-byte address model
// and a DWARF64 .debug_str_offsets contribution: exercises the
// preserved DWARF32/64, endianness and address-width meanings at once.
static int case_indexed_widths(void)
{
        const bool be = true;
        const bool d64 = true;
        const unsigned addr_size = 4;

        Bytes strs(1, 0);
        size_t s_root = add_string(strs, "be64-root");
        size_t s_fn = add_string(strs, "be64-fn");

        Bytes stroff;
        size_t stroff_base =
                add_stroff_contrib(stroff, { s_root, s_fn }, true, be);

        Bytes addr;
        // .debug_addr contribution is DWARF32 but has 4-byte addresses.
        size_t addr_base = add_addr_contrib(addr, { 0x1234, 0x5678 },
                                            addr_size, false, be);

        Bytes abbrev;
        abbrev_entry(abbrev, 1, TAG_compile_unit, true,
                     { A(AT_low_pc, FORM_addrx4),
                       A(AT_name, FORM_strx4),
                       A(AT_str_offsets_base, FORM_sec_offset),
                       A(AT_addr_base, FORM_sec_offset) });
        abbrev_entry(abbrev, 2, TAG_subprogram, false,
                     { A(AT_name, FORM_strx),
                       A(AT_low_pc, FORM_addrx) });
        abbrev_end(abbrev);

        Bytes info;
        size_t cu = cu_begin_v5(info, be, d64, addr_size, 0);
        uleb(info, 1);
        put_index(info, FORM_addrx4, 0, be);
        put_index(info, FORM_strx4, 0, be);
        w_put(info, stroff_base, 8, be);
        w_put(info, addr_base, 8, be);
        uleb(info, 2);
        uleb(info, 1);
        uleb(info, 1);
        uleb(info, 0); // child list terminator
        cu_finish(info, cu, be, d64);

        Handle h = open({ { ".debug_info", info },
                          { ".debug_abbrev", abbrev },
                          { ".debug_str", strs },
                          { ".debug_str_offsets", stroff },
                          { ".debug_addr", addr } });
        const dwarf::compilation_unit &u = h.dw.compilation_units().at(0);
        dwarf::die root = u.root();
        TASK_CHECK(root[AT_name].as_string() == "be64-root");
        TASK_CHECK(root[AT_low_pc].as_address() == 0x1234);
        TASK_CHECK(root[AT_str_offsets_base].as_sec_offset() == stroff_base);

        dwarf::die fn = nth_child(root, 0);
        TASK_CHECK(fn[AT_name].as_string() == "be64-fn");
        TASK_CHECK(fn[AT_low_pc].as_address() == 0x5678);
        return 0;
}

// ===============================================================
// Group: constants
// ===============================================================

// DW_FORM_implicit_const: signed LEB value stored in the abbrev, no
// bytes in the DIE.  Several implicit constants interleaved with
// indexed and ordinary fields; verify both interfaces and the
// position of the following DIE.
static int case_implicit_const(void)
{
        Bytes strs(1, 0);
        size_t s_root = add_string(strs, "const-root");
        size_t s_a = add_string(strs, "const-a");
        size_t s_b = add_string(strs, "const-b");

        Bytes stroff;
        size_t stroff_base = add_stroff_contrib(stroff, { s_root, s_a, s_b },
                                                false, false);
        Bytes addr;
        size_t addr_base = add_addr_contrib(addr, { 0x5000, 0x5100, 0x5200 },
                                            8, false, false);

        Bytes abbrev;
        abbrev_entry(abbrev, 1, TAG_compile_unit, true,
                     { A(AT_name, FORM_strx),
                       A(AT_str_offsets_base, FORM_sec_offset),
                       A(AT_addr_base, FORM_sec_offset) });
        // 2: implicit constants interleaved with normal and indexed
        // fields, including a negative constant immediately next to
        // an ordinary data1 field.
        abbrev_entry(abbrev, 2, TAG_subprogram, false,
                     { A(AT_name, FORM_strx),
                       A(AT_byte_size, FORM_data2),
                       A(AT_high_pc, FORM_implicit_const, -8),
                       A(AT_low_pc, FORM_addrx),
                       A(AT_decl_line, FORM_implicit_const, 42),
                       A(AT_data_member_location, FORM_implicit_const, -16),
                       A(AT_decl_column, FORM_data1) });
        // 3: another DIE after it
        abbrev_entry(abbrev, 3, TAG_subprogram, false,
                     { A(AT_name, FORM_strx), A(AT_low_pc, FORM_addrx) });
        abbrev_end(abbrev);

        Bytes info;
        size_t cu = cu_begin_v5(info, false, false, 8, 0);
        uleb(info, 1);
        uleb(info, 0);
        w_put(info, stroff_base, 4, false);
        w_put(info, addr_base, 4, false);

        uleb(info, 2);
        uleb(info, 1);            // name index
        w_put(info, 0x100, 2, false); // byte_size
        // high_pc implicit: no bytes
        uleb(info, 2);            // low_pc addr index
        // decl_line, data_member_location implicit: no bytes
        info.push_back(7);        // decl_column data1

        // Expected encoded size of DIE A; the next DIE must start
        // exactly here.
        size_t a_size = uleb_len(2) + uleb_len(1) + 2 + uleb_len(2) + 1;
        size_t b_offset = info.size();

        uleb(info, 3);
        uleb(info, 2);
        uleb(info, 2);
        uleb(info, 0); // child list terminator
        cu_finish(info, cu, false, false);

        Handle h = open({ { ".debug_info", info },
                          { ".debug_abbrev", abbrev },
                          { ".debug_str", strs },
                          { ".debug_str_offsets", stroff },
                          { ".debug_addr", addr } });
        const dwarf::compilation_unit &u = h.dw.compilation_units().at(0);
        dwarf::die root = u.root();
        TASK_CHECK(root[AT_name].as_string() == "const-root");

        dwarf::die a = nth_child(root, 0);
        TASK_CHECK(a[AT_name].as_string() == "const-a");
        TASK_CHECK(a[AT_byte_size].as_uconstant() == 0x100);
        TASK_CHECK(a[AT_high_pc].as_sconstant() == -8);
        TASK_CHECK(a[AT_high_pc].as_uconstant() ==
                   (std::uint64_t)(std::int64_t)-8);
        TASK_CHECK(a[AT_low_pc].as_address() == 0x5200);
        TASK_CHECK(a[AT_decl_line].as_sconstant() == 42);
        TASK_CHECK(a[AT_data_member_location].as_sconstant() == -16);
        TASK_CHECK(a[AT_decl_column].as_uconstant() == 7);

        // The DIE following the interleaved implicit constants must
        // be located exactly where the encoder put it.
        dwarf::die b = nth_child(root, 1);
        TASK_CHECK(b.get_unit_offset() == b_offset);
        TASK_CHECK((a.get_unit_offset() + a_size) == b.get_unit_offset());
        TASK_CHECK(b[AT_name].as_string() == "const-b");
        TASK_CHECK(b[AT_low_pc].as_address() == 0x5200);

        // attributes() and operator[] must agree.
        std::vector<std::pair<dwarf::DW_AT, dwarf::value> > av =
                a.attributes();
        TASK_CHECK(av.size() == 7);
        TASK_CHECK(av[1].first == dwarf::DW_AT::byte_size);
        TASK_CHECK(av[1].second.as_uconstant() == 0x100);
        TASK_CHECK(av[2].first == dwarf::DW_AT::high_pc);
        TASK_CHECK(av[2].second.as_sconstant() == -8);
        TASK_CHECK(av[2].second.get_unit_offset() ==
                   a[AT_high_pc].get_unit_offset());
        TASK_CHECK(av[5].second.as_sconstant() == -16);
        TASK_CHECK(av[6].second.as_uconstant() == 7);

        // No child should remain.
        dwarf::die::iterator end = root.end();
        dwarf::die::iterator it = root.begin();
        ++it;
        ++it;
        TASK_CHECK(!(it != root.end()));
        return 0;
}

// An index that is valid in a *different* contribution must not be
// served from that neighbour: it is out of range for this unit's
// contribution.
static int case_index_out_of_contribution(void)
{
        Bytes strs(1, 0);
        size_t s0 = add_string(strs, "first");
        size_t s1 = add_string(strs, "second");
        size_t s2 = add_string(strs, "neighbour");

        Bytes stroff;
        // Contribution A has two entries; contribution B follows and
        // does have a third entry.
        size_t baseA = add_stroff_contrib(stroff, { s0, s1 }, false, false);
        add_stroff_contrib(stroff, { s2, s0, s1, s2 }, false, false);

        Bytes abbrev;
        abbrev_entry(abbrev, 1, TAG_compile_unit, false,
                     { A(AT_name, FORM_strx),
                       A(AT_str_offsets_base, FORM_sec_offset) });
        abbrev_end(abbrev);

        Bytes info;
        size_t cu = cu_begin_v5(info, false, false, 8, 0);
        uleb(info, 1);
        uleb(info, 2); // index 2 is absent from contribution A
        w_put(info, baseA, 4, false);
        cu_finish(info, cu, false, false);

        Handle h = open({ { ".debug_info", info },
                          { ".debug_abbrev", abbrev },
                          { ".debug_str", strs },
                          { ".debug_str_offsets", stroff } });
        const dwarf::compilation_unit &u = h.dw.compilation_units().at(0);
        dwarf::die root = u.root();
        bool threw = false;
        try {
                (void)root[AT_name].as_string();
        } catch (const dwarf::format_error &) {
                threw = true;
        }
        TASK_CHECK(threw);
        return 0;
}

// Introducing an address index without a .debug_addr section must
// throw rather than fabricate an address.
static int case_missing_addr(void)
{
        Bytes abbrev;
        abbrev_entry(abbrev, 1, TAG_compile_unit, false,
                     { A(AT_low_pc, FORM_addrx) });
        abbrev_end(abbrev);

        Bytes info;
        size_t cu = cu_begin_v5(info, false, false, 8, 0);
        uleb(info, 1);
        uleb(info, 0);
        cu_finish(info, cu, false, false);

        Handle h = open({ { ".debug_info", info },
                          { ".debug_abbrev", abbrev } });
        const dwarf::compilation_unit &u = h.dw.compilation_units().at(0);
        dwarf::die root = u.root();
        bool threw = false;
        try {
                (void)root[AT_low_pc].as_address();
        } catch (const dwarf::format_error &) {
                threw = true;
        }
        TASK_CHECK(threw);
        return 0;
}

// An address index that is valid in a neighbouring .debug_addr
// contribution must not be served from it.
static int case_addr_out_of_contribution(void)
{
        Bytes addr;
        size_t baseA = add_addr_contrib(addr, { 0x1000, 0x1100 }, 8,
                                        false, false);
        add_addr_contrib(addr, { 0x9000, 0x9100, 0x9200 }, 8, false, false);

        Bytes abbrev;
        abbrev_entry(abbrev, 1, TAG_compile_unit, false,
                     { A(AT_low_pc, FORM_addrx),
                       A(AT_addr_base, FORM_sec_offset) });
        abbrev_end(abbrev);

        Bytes info;
        size_t cu = cu_begin_v5(info, false, false, 8, 0);
        uleb(info, 1);
        uleb(info, 2); // index 2 is absent from contribution A
        w_put(info, baseA, 4, false);
        cu_finish(info, cu, false, false);

        Handle h = open({ { ".debug_info", info },
                          { ".debug_abbrev", abbrev },
                          { ".debug_addr", addr } });
        const dwarf::compilation_unit &u = h.dw.compilation_units().at(0);
        dwarf::die root = u.root();
        bool threw = false;
        try {
                (void)root[AT_low_pc].as_address();
        } catch (const dwarf::format_error &) {
                threw = true;
        }
        TASK_CHECK(threw);
        return 0;
}

// Introducing a string index without a .debug_str_offsets section
// must throw rather than fabricate a value.
static int case_missing_str_offsets(void)
{
        Bytes strs(1, 0);
        add_string(strs, "unused");

        Bytes abbrev;
        abbrev_entry(abbrev, 1, TAG_compile_unit, false,
                     { A(AT_name, FORM_strx) });
        abbrev_end(abbrev);

        Bytes info;
        size_t cu = cu_begin_v5(info, false, false, 8, 0);
        uleb(info, 1);
        uleb(info, 0);
        cu_finish(info, cu, false, false);

        Handle h = open({ { ".debug_info", info },
                          { ".debug_abbrev", abbrev },
                          { ".debug_str", strs } });
        const dwarf::compilation_unit &u = h.dw.compilation_units().at(0);
        dwarf::die root = u.root();
        bool threw = false;
        try {
                (void)root[AT_name].as_string();
        } catch (const dwarf::format_error &) {
                threw = true;
        }
        TASK_CHECK(threw);
        return 0;
}

// A truncated implicit-constant value in the abbrev table must throw.
static int case_truncated_implicit_const(void)
{
        Bytes abbrev;
        uleb(abbrev, 1);
        uleb(abbrev, TAG_compile_unit);
        abbrev.push_back(0);
        uleb(abbrev, (std::uint32_t)AT_high_pc);
        uleb(abbrev, (std::uint32_t)FORM_implicit_const);
        // Section ends here: the SLEB128 value is missing.

        Bytes info;
        size_t cu = cu_begin_v5(info, false, false, 8, 0);
        uleb(info, 1);
        cu_finish(info, cu, false, false);

        Handle h = open({ { ".debug_info", info },
                          { ".debug_abbrev", abbrev } });
        const dwarf::compilation_unit &u = h.dw.compilation_units().at(0);
        bool threw = false;
        try {
                (void)u.root();
        } catch (const std::exception &) {
                threw = true;
        }
        TASK_CHECK(threw);
        return 0;
}

// A truncated index in the DIE must throw.
static int case_truncated_index(void)
{
        Bytes addr;
        add_addr_contrib(addr, { 0x7000 }, 8, false, false);

        Bytes abbrev;
        abbrev_entry(abbrev, 1, TAG_compile_unit, false,
                     { A(AT_low_pc, FORM_addrx4) });
        abbrev_end(abbrev);

        Bytes info;
        size_t cu = cu_begin_v5(info, false, false, 8, 0);
        uleb(info, 1);
        info.push_back(0x11); // only two of the four index bytes
        info.push_back(0x22);
        cu_finish(info, cu, false, false);

        Handle h = open({ { ".debug_info", info },
                          { ".debug_abbrev", abbrev },
                          { ".debug_addr", addr } });
        const dwarf::compilation_unit &u = h.dw.compilation_units().at(0);
        dwarf::die root = u.root();
        bool threw = false;
        try {
                (void)root[AT_low_pc].as_address();
        } catch (const std::exception &) {
                threw = true;
        }
        TASK_CHECK(threw);
        return 0;
}

// ===============================================================
// Group: context
// ===============================================================

// Two compilation units share the same indices but use different
// tables.  Interleaved lookups must stay with the owning unit.  Cross
// unit resolve() must use the unit that actually supplies the
// attribute.
static int case_two_units(void)
{
        Bytes strs(1, 0);
        size_t s1_root = add_string(strs, "cu1-root");
        size_t s1_fn = add_string(strs, "cu1-fn");
        size_t s2_root = add_string(strs, "cu2-root");
        size_t s2_fn = add_string(strs, "cu2-fn");
        size_t s2_abs = add_string(strs, "cu2-absolute");

        Bytes stroff;
        // Both contributions use the same index layout; only the
        // strings differ.
        size_t base1 = add_stroff_contrib(stroff, { s1_root, s1_fn },
                                          false, false);
        size_t base2 = add_stroff_contrib(stroff, { s2_root, s2_fn, s2_abs },
                                          false, false);

        Bytes addr;
        size_t addr1 = add_addr_contrib(addr, { 0x1000, 0x1100 }, 8,
                                        false, false);
        size_t addr2 = add_addr_contrib(addr, { 0x2000, 0x2100, 0x2200 }, 8,
                                        false, false);

        Bytes abbrev;
        // 1: root with base attributes after the indexed ones.
        abbrev_entry(abbrev, 1, TAG_compile_unit, true,
                     { A(AT_name, FORM_strx1),
                       A(AT_low_pc, FORM_addrx1),
                       A(AT_str_offsets_base, FORM_sec_offset),
                       A(AT_addr_base, FORM_sec_offset) });
        abbrev_entry(abbrev, 2, TAG_subprogram, false,
                     { A(AT_name, FORM_strx),
                       A(AT_low_pc, FORM_addrx) });
        // 3: concrete DIE referring to another unit
        abbrev_entry(abbrev, 3, TAG_subprogram, false,
                     { A(AT_abstract_origin, FORM_ref_addr) });
        // 4: the absolute (provider) DIE in unit 2
        abbrev_entry(abbrev, 4, TAG_subprogram, false,
                     { A(AT_name, FORM_strx),
                       A(AT_low_pc, FORM_addrx) });
        abbrev_end(abbrev);

        // Unit 2 first so we know the section offset of its absolute
        // DIE for the cross-unit reference.
        Bytes info2;
        size_t cu2 = info2.size();
        cu_begin_v5(info2, false, false, 8, 0);
        uleb(info2, 1);            // root
        uleb(info2, 0);            // name
        uleb(info2, 0);            // low_pc
        w_put(info2, base2, 4, false);
        w_put(info2, addr2, 4, false);
        size_t abs_rel = info2.size();
        uleb(info2, 4);            // absolute DIE
        uleb(info2, 2);            // name index -> "cu2-absolute"
        uleb(info2, 2);            // low_pc index -> 0x2200
        uleb(info2, 0);            // child list terminator
        cu_finish(info2, cu2, false, false);

        Bytes info1;
        size_t cu1 = info1.size();
        cu_begin_v5(info1, false, false, 8, 0);
        uleb(info1, 1);            // root
        uleb(info1, 0);            // name -> cu1-root
        uleb(info1, 0);            // low_pc -> 0x1000
        w_put(info1, base1, 4, false);
        w_put(info1, addr1, 4, false);
        // child: normal function
        uleb(info1, 2);
        uleb(info1, 1);            // name -> cu1-fn
        uleb(info1, 1);            // low_pc -> 0x1100
        // child: concrete DIE with a cross-unit abstract origin
        uleb(info1, 3);
        size_t ref_at = info1.size();
        w_put(info1, 0, 4, false); // patched below
        uleb(info1, 0);            // child list terminator
        cu_finish(info1, cu1, false, false);

        // The absolute section offset of unit 2's absolute DIE.
        std::uint64_t abs_off = info1.size() + abs_rel;
        w_patch(info1, ref_at, abs_off, 4, false);

        Bytes info = info1;
        info.insert(info.end(), info2.begin(), info2.end());

        Handle h = open({ { ".debug_info", info },
                          { ".debug_abbrev", abbrev },
                          { ".debug_str", strs },
                          { ".debug_str_offsets", stroff },
                          { ".debug_addr", addr } });
        const std::vector<dwarf::compilation_unit> &cus =
                h.dw.compilation_units();
        TASK_CHECK(cus.size() == 2);

        const dwarf::compilation_unit &u1 = cus.at(0);
        const dwarf::compilation_unit &u2 = cus.at(1);

        // Interleave the reads: u1, u2, u1, u2.
        dwarf::die r1 = u1.root();
        dwarf::die r2 = u2.root();
        TASK_CHECK(r1[AT_name].as_string() == "cu1-root");
        TASK_CHECK(r2[AT_name].as_string() == "cu2-root");
        TASK_CHECK(r1[AT_low_pc].as_address() == 0x1000);
        TASK_CHECK(r2[AT_low_pc].as_address() == 0x2000);

        dwarf::die f1 = nth_child(r1, 0);
        dwarf::die f2 = nth_child(r2, 0);
        TASK_CHECK(f1[AT_name].as_string() == "cu1-fn");
        TASK_CHECK(f2[AT_name].as_string() == "cu2-absolute");
        TASK_CHECK(f1[AT_low_pc].as_address() == 0x1100);
        TASK_CHECK(f2[AT_low_pc].as_address() == 0x2200);

        // Back to unit 1 after unit 2: same index, must still be
        // unit 1's table.
        TASK_CHECK(r1[AT_name].as_string() == "cu1-root");
        TASK_CHECK(f1[AT_name].as_string() == "cu1-fn");

        // resolve() across units must use the providing unit's
        // tables (unit 2 here), not the referencing unit's.
        dwarf::die concrete = nth_child(r1, 1);
        TASK_CHECK(concrete.has(AT_abstract_origin));
        TASK_CHECK(!concrete.has(AT_name));
        dwarf::value rn = concrete.resolve(AT_name);
        TASK_CHECK(rn.get_type() == dwarf::value::type::string);
        TASK_CHECK(rn.as_string() == "cu2-absolute");
        // The value must come from unit 2, the actual provider.
        dwarf::die provider = concrete[AT_abstract_origin].as_reference();
        TASK_CHECK(provider.get_unit() == u2);
        TASK_CHECK(rn.get_unit_offset() == provider[AT_name].get_unit_offset());
        dwarf::value ra = concrete.resolve(AT_low_pc);
        TASK_CHECK(ra.as_address() == 0x2200);
        TASK_CHECK(ra.get_unit_offset() == provider[AT_low_pc].get_unit_offset());
        return 0;
}

// A base attribute that appears after the indexed attributes it
// locates, at the root DIE, must still be found without the caller
// reading it first.
static int case_late_bases(void)
{
        Bytes strs(1, 0);
        size_t s_root = add_string(strs, "late-root");
        size_t s_child = add_string(strs, "late-child");

        Bytes stroff;
        size_t stroff_base = add_stroff_contrib(stroff, { s_root, s_child },
                                                false, false);
        Bytes addr;
        size_t addr_base = add_addr_contrib(addr, { 0x9000, 0x9100 }, 8,
                                            false, false);

        Bytes abbrev;
        // name (strx) and low_pc (addrx) precede both base
        // attributes.
        abbrev_entry(abbrev, 1, TAG_compile_unit, true,
                     { A(AT_name, FORM_strx),
                       A(AT_low_pc, FORM_addrx),
                       A(AT_str_offsets_base, FORM_sec_offset),
                       A(AT_addr_base, FORM_sec_offset) });
        abbrev_entry(abbrev, 2, TAG_variable, false,
                     { A(AT_name, FORM_strx1),
                       A(AT_low_pc, FORM_addrx2) });
        abbrev_end(abbrev);

        Bytes info;
        size_t cu = cu_begin_v5(info, false, false, 8, 0);
        uleb(info, 1);
        uleb(info, 0);
        uleb(info, 0);
        w_put(info, stroff_base, 4, false);
        w_put(info, addr_base, 4, false);
        uleb(info, 2);
        put_index(info, FORM_strx1, 1, false);
        put_index(info, FORM_addrx2, 1, false);
        uleb(info, 0); // child list terminator
        cu_finish(info, cu, false, false);

        Handle h = open({ { ".debug_info", info },
                          { ".debug_abbrev", abbrev },
                          { ".debug_str", strs },
                          { ".debug_str_offsets", stroff },
                          { ".debug_addr", addr } });
        const dwarf::compilation_unit &u = h.dw.compilation_units().at(0);
        // Reading the root name first, with no prior access to the
        // base attributes, must work.
        TASK_CHECK(u.root()[AT_name].as_string() == "late-root");
        TASK_CHECK(u.root()[AT_low_pc].as_address() == 0x9000);
        dwarf::die child = nth_child(u.root(), 0);
        TASK_CHECK(child[AT_name].as_string() == "late-child");
        TASK_CHECK(child[AT_low_pc].as_address() == 0x9100);
        return 0;
}

// ===============================================================
// Group: legacy
// ===============================================================

// Direct strings, DW_FORM_strp and DW_FORM_addr keep working exactly
// as before (DWARF 4).
static int case_legacy_forms(void)
{
        Bytes strs(1, 0);
        size_t s_name = add_string(strs, "legacy-name");
        size_t s_dir = add_string(strs, "legacy-dir");

        Bytes abbrev;
        // DWARF 4 layout: version, abbrev offset, address size.
        abbrev_entry(abbrev, 1, TAG_compile_unit, true,
                     { A(AT_name, FORM_string),
                       A(AT_byte_size, FORM_data2) });
        abbrev_entry(abbrev, 2, TAG_subprogram, false,
                     { A(AT_name, FORM_strp),
                       A(AT_low_pc, FORM_addr),
                       A(AT_high_pc, FORM_data4) });
        abbrev_entry(abbrev, 3, TAG_variable, false,
                     { A(AT_name, FORM_strp) });
        abbrev_end(abbrev);

        Bytes info;
        size_t start = info.size();
        fixture::put(info, 0, 4);      // length
        fixture::put(info, 4, 2);      // version 4
        fixture::put(info, 0, 4);      // abbrev offset
        info.push_back(8);             // address size
        uleb(info, 1);
        fixture::text(info, "legacy-root");
        fixture::put(info, 0x20, 2);
        uleb(info, 2);
        fixture::put(info, s_name, 4); // name via strp
        fixture::put(info, 0x401234, 8);
        fixture::put(info, 0x40, 4);
        uleb(info, 3);
        fixture::put(info, s_dir, 4);  // comp_dir-like strp
        uleb(info, 0);                 // child list terminator
        fixture::patch(info, start, info.size() - start - 4, 4);

        Handle h = open({ { ".debug_info", info },
                          { ".debug_abbrev", abbrev },
                          { ".debug_str", strs } });
        const dwarf::compilation_unit &u = h.dw.compilation_units().at(0);
        dwarf::die root = u.root();
        TASK_CHECK(root[AT_name].as_string() == "legacy-root");
        TASK_CHECK(root[AT_byte_size].as_uconstant() == 0x20);

        dwarf::die fn = nth_child(root, 0);
        TASK_CHECK(fn[AT_name].as_string() == "legacy-name");
        TASK_CHECK(fn[AT_low_pc].as_address() == 0x401234);
        TASK_CHECK(dwarf::at_high_pc(fn) == 0x401234 + 0x40);

        dwarf::die var = nth_child(root, 1);
        TASK_CHECK(var[AT_name].as_string() == "legacy-dir");

        // attributes()/operator[] agreement on legacy forms too.
        std::vector<std::pair<dwarf::DW_AT, dwarf::value> > av =
                fn.attributes();
        TASK_CHECK(av.size() == 3);
        TASK_CHECK(av[0].second.as_string() == fn[AT_name].as_string());
        TASK_CHECK(av[1].second.as_address() == fn[AT_low_pc].as_address());
        return 0;
}

// The baseline DWARF 4 fixture must keep reading the same values
// through the same interface.
static int case_legacy_baseline(void)
{
        Handle h = open({ { ".debug_info", fixture::baseline_info() },
                          { ".debug_abbrev", fixture::baseline_abbrev() } });
        const dwarf::compilation_unit &u = h.dw.compilation_units().at(0);
        dwarf::die root = u.root();
        TASK_CHECK(root[AT_name].as_string() == "baseline-unit");
        dwarf::die child = *root.begin();
        TASK_CHECK(child[AT_name].as_string() == "entry");
        TASK_CHECK(child[AT_low_pc].as_address() == 0x401020);
        TASK_CHECK(child[AT_high_pc].as_uconstant() == 0x18);
        dwarf::die::iterator it = root.begin();
        ++it;
        TASK_CHECK(!(it != root.end()));
        return 0;
}

// ===============================================================
// Registration
// ===============================================================

extern const struct task_case task_cases[];
const struct task_case task_cases[] = {
        { "indexed",   "forms",                    case_indexed_forms },
        { "indexed",   "widths",                   case_indexed_widths },
        { "constants", "implicit-const",           case_implicit_const },
        { "constants", "index-out-of-contribution", case_index_out_of_contribution },
        { "constants", "addr-out-of-contribution", case_addr_out_of_contribution },
        { "constants", "missing-str-offsets",      case_missing_str_offsets },
        { "constants", "missing-addr",             case_missing_addr },
        { "constants", "truncated-implicit-const", case_truncated_implicit_const },
        { "constants", "truncated-index",          case_truncated_index },
        { "context",   "two-units",                case_two_units },
        { "context",   "late-bases",               case_late_bases },
        { "legacy",    "forms",                    case_legacy_forms },
        { "legacy",    "baseline",                 case_legacy_baseline },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
