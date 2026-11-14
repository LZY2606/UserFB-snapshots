// Copyright (c) 2013 Austin T. Clements. All rights reserved.
// Use of this source code is governed by an MIT license
// that can be found in the LICENSE file.

#include "internal.hh"

#include <cstring>

using namespace std;

DWARFPP_BEGIN_NAMESPACE

/**
 * Read a table index encoded with one of the DWARF5 indexed forms
 * (DW_FORM_strx* or DW_FORM_addrx*) from cur.
 */
static uint64_t
read_index(cursor &cur, DW_FORM form)
{
        switch (form) {
        case DW_FORM::strx:
        case DW_FORM::addrx:
                return cur.uleb128();
        case DW_FORM::strx1:
        case DW_FORM::addrx1:
                return cur.fixed<uint8_t>();
        case DW_FORM::strx2:
        case DW_FORM::addrx2:
                return cur.fixed<uint16_t>();
        case DW_FORM::strx3:
        case DW_FORM::addrx3: {
                // There's no 3-byte integer type, so assemble the
                // index explicitly, honoring the section byte order.
                uint64_t val = 0;
                if (cur.sec->ord == byte_order::lsb) {
                        for (unsigned i = 0; i < 3; i++)
                                val |= (uint64_t)cur.fixed<uint8_t>() << (8 * i);
                } else {
                        for (unsigned i = 0; i < 3; i++)
                                val = (val << 8) | cur.fixed<uint8_t>();
                }
                return val;
        }
        case DW_FORM::strx4:
        case DW_FORM::addrx4:
                return cur.fixed<uint32_t>();
        default:
                throw logic_error("not an indexed form " + to_string(form));
        }
}

/**
 * Return the offset of the index'th entry of a table section whose
 * per-unit contribution begins at base, or throw format_error if
 * the entry is not wholly contained in the section.
 */
static section_offset
table_entry_offset(const std::shared_ptr<section> &sec, section_offset base,
                   uint64_t index, unsigned entry_size, const char *what)
{
        section_offset off = base + index * (section_offset)entry_size;
        if (off < base || off > sec->size() || entry_size > sec->size() - off)
                throw format_error(std::string(what) + " index 0x" +
                                   to_hex(index) + " out of range of table");
        return off;
}

/**
 * Read an address of the given size from sec at off.
 */
static taddr
read_addr_entry(const std::shared_ptr<section> &sec, section_offset off,
                unsigned addr_size)
{
        cursor cur(sec, off);
        switch (addr_size) {
        case 1:
                return cur.fixed<uint8_t>();
        case 2:
                return cur.fixed<uint16_t>();
        case 4:
                return cur.fixed<uint32_t>();
        case 8:
                return cur.fixed<uint64_t>();
        default:
                throw format_error("address size " + std::to_string(addr_size) +
                                   " not supported");
        }
}

value::value(const unit *cu,
             DW_AT name, DW_FORM form, type typ, section_offset offset,
             std::int64_t implicit_const)
        : cu(cu), form(form), typ(typ), offset(offset),
          implicit_const(implicit_const) {
        if (form == DW_FORM::indirect)
                resolve_indirect(name);
}

section_offset
value::get_section_offset() const
{
        return cu->get_section_offset() + offset;
}

taddr
value::as_address() const
{
        cursor cur(cu->data(), offset);
        switch (form) {
        case DW_FORM::addr:
                return cur.address();

        case DW_FORM::addrx:
        case DW_FORM::addrx1:
        case DW_FORM::addrx2:
        case DW_FORM::addrx3:
        case DW_FORM::addrx4: {
                // DWARF5 section 7.27.  The index selects an entry in
                // this unit's contribution to .debug_addr; entries
                // are addresses of the unit's address size.
                uint64_t index = read_index(cur, form);
                auto sec = cu->get_dwarf().get_section(section_type::addr);
                unsigned addr_size = cu->data()->addr_size;
                section_offset off = table_entry_offset(
                        sec, cu->get_addr_base(), index, addr_size,
                        ".debug_addr");
                return read_addr_entry(sec, off, addr_size);
        }

        default:
                throw value_type_mismatch("cannot read " + to_string(typ) + " as address");
        }
}

const void *
value::as_block(size_t *size_out) const
{
        // XXX Blocks can contain all sorts of things, including
        // references, which couldn't be resolved by callers in the
        // current minimal API.
        cursor cur(cu->data(), offset);
        switch (form) {
        case DW_FORM::block1:
                *size_out = cur.fixed<uint8_t>();
                break;
        case DW_FORM::block2:
                *size_out = cur.fixed<uint16_t>();
                break;
        case DW_FORM::block4:
                *size_out = cur.fixed<uint32_t>();
                break;
        case DW_FORM::block:
        case DW_FORM::exprloc:
                *size_out = cur.uleb128();
                break;
        default:
                throw value_type_mismatch("cannot read " + to_string(typ) + " as block");
        }
        cur.ensure(*size_out);
        return cur.pos;
}

uint64_t
value::as_uconstant() const
{
        cursor cur(cu->data(), offset);
        switch (form) {
        case DW_FORM::data1:
                return cur.fixed<uint8_t>();
        case DW_FORM::data2:
                return cur.fixed<uint16_t>();
        case DW_FORM::data4:
                return cur.fixed<uint32_t>();
        case DW_FORM::data8:
                return cur.fixed<uint64_t>();
        case DW_FORM::udata:
                return cur.uleb128();
        default:
                throw value_type_mismatch("cannot read " + to_string(typ) + " as uconstant");
        }
}

int64_t
value::as_sconstant() const
{
        cursor cur(cu->data(), offset);
        switch (form) {
        case DW_FORM::data1:
                return cur.fixed<int8_t>();
        case DW_FORM::data2:
                return cur.fixed<int16_t>();
        case DW_FORM::data4:
                return cur.fixed<int32_t>();
        case DW_FORM::data8:
                return cur.fixed<int64_t>();
        case DW_FORM::sdata:
                return cur.sleb128();
        case DW_FORM::implicit_const:
                // The value is recorded in the abbrev declaration,
                // not in the DIE.
                return implicit_const;
        default:
                throw value_type_mismatch("cannot read " + to_string(typ) + " as sconstant");
        }
}

expr
value::as_exprloc() const
{
        cursor cur(cu->data(), offset);
        size_t size;
        // Prior to DWARF 4, exprlocs were encoded as blocks.
        switch (form) {
        case DW_FORM::exprloc:
        case DW_FORM::block:
                size = cur.uleb128();
                break;
        case DW_FORM::block1:
                size = cur.fixed<uint8_t>();
                break;
        case DW_FORM::block2:
                size = cur.fixed<uint16_t>();
                break;
        case DW_FORM::block4:
                size = cur.fixed<uint32_t>();
                break;
        default:
                throw value_type_mismatch("cannot read " + to_string(typ) + " as exprloc");
        }
        return expr(cu, cur.get_section_offset(), size);
}

bool
value::as_flag() const
{
        switch (form) {
        case DW_FORM::flag: {
                cursor cur(cu->data(), offset);
                return cur.fixed<ubyte>() != 0;
        }
        case DW_FORM::flag_present:
                return true;
        default:
                throw value_type_mismatch("cannot read " + to_string(typ) + " as flag");
        }
}

rangelist
value::as_rangelist() const
{
        section_offset off = as_sec_offset();

        // The compilation unit may not have a base address.  In this
        // case, the first entry in the range list must be a base
        // address entry, but we'll just assume 0 for the initial base
        // address.
        die cudie = cu->root();
        taddr cu_low_pc = cudie.has(DW_AT::low_pc) ? at_low_pc(cudie) : 0;
        auto sec = cu->get_dwarf().get_section(section_type::ranges);
        auto cusec = cu->data();
        return rangelist(sec, off, cusec->addr_size, cu_low_pc);
}

die
value::as_reference() const
{
        section_offset off;
        // XXX Would be nice if we could avoid this.  The cursor is
        // all overhead here.
        cursor cur(cu->data(), offset);
        switch (form) {
        case DW_FORM::ref1:
                off = cur.fixed<ubyte>();
                break;
        case DW_FORM::ref2:
                off = cur.fixed<uhalf>();
                break;
        case DW_FORM::ref4:
                off = cur.fixed<uword>();
                break;
        case DW_FORM::ref8:
                off = cur.fixed<uint64_t>();
                break;
        case DW_FORM::ref_udata:
                off = cur.uleb128();
                break;

        case DW_FORM::ref_addr: {
                off = cur.offset();
                // These seem to be extremely rare in practice (I
                // haven't been able to get gcc to produce a
                // ref_addr), so it's not worth caching this lookup.
                const compilation_unit *base_cu = nullptr;
                for (auto &file_cu : cu->get_dwarf().compilation_units()) {
                        if (file_cu.get_section_offset() > off)
                                break;
                        base_cu = &file_cu;
                }
                die d(base_cu);
                d.read(off - base_cu->get_section_offset());
                return d;
        }

        case DW_FORM::ref_sig8: {
                uint64_t sig = cur.fixed<uint64_t>();
                try {
                        return cu->get_dwarf().get_type_unit(sig).type();
                } catch (std::out_of_range &e) {
                        throw format_error("unknown type signature 0x" + to_hex(sig));
                }
        }

        default:
                throw value_type_mismatch("cannot read " + to_string(typ) + " as reference");
        }

        die d(cu);
        d.read(off);
        return d;
}

void
value::as_string(string &buf) const
{
        size_t size;
        const char *p = as_cstr(&size);
        buf.resize(size);
        memmove(&buf.front(), p, size);
}

string
value::as_string() const
{
        size_t size;
        const char *s = as_cstr(&size);
        return string(s, size);
}

const char *
value::as_cstr(size_t *size_out) const
{
        cursor cur(cu->data(), offset);
        switch (form) {
        case DW_FORM::string:
                return cur.cstr(size_out);
        case DW_FORM::strp: {
                section_offset off = cur.offset();
                cursor scur(cu->get_dwarf().get_section(section_type::str), off);
                return scur.cstr(size_out);
        }
        case DW_FORM::strx:
        case DW_FORM::strx1:
        case DW_FORM::strx2:
        case DW_FORM::strx3:
        case DW_FORM::strx4: {
                // DWARF5 section 7.26.  The index selects an entry in
                // this unit's contribution to .debug_str_offsets;
                // entries are offsets into .debug_str, sized
                // according to the unit's DWARF format.
                uint64_t index = read_index(cur, form);
                auto sec = cu->get_dwarf().get_section(section_type::str_offsets);
                bool dwarf64 = cu->data()->fmt == format::dwarf64;
                unsigned entry_size = dwarf64 ? 8 : 4;
                section_offset off = table_entry_offset(
                        sec, cu->get_str_offsets_base(), index, entry_size,
                        ".debug_str_offsets");
                cursor ecur(sec, off);
                section_offset str_off = dwarf64 ? ecur.fixed<uint64_t>()
                                                 : ecur.fixed<uint32_t>();
                auto strsec = cu->get_dwarf().get_section(section_type::str);
                if (str_off >= strsec->size())
                        throw format_error("string offset 0x" + to_hex(str_off) +
                                           " out of range of .debug_str");
                cursor scur(strsec, str_off);
                return scur.cstr(size_out);
        }
        default:
                throw value_type_mismatch("cannot read " + to_string(typ) + " as string");
        }
}

section_offset
value::as_sec_offset() const
{
        // Prior to DWARF 4, sec_offsets were encoded as data4 or
        // data8.
        cursor cur(cu->data(), offset);
        switch (form) {
        case DW_FORM::data4:
                return cur.fixed<uint32_t>();
        case DW_FORM::data8:
                return cur.fixed<uint64_t>();
        case DW_FORM::sec_offset:
                return cur.offset();
        default:
                throw value_type_mismatch("cannot read " + to_string(typ) + " as sec_offset");
        }
}

void
value::resolve_indirect(DW_AT name)
{
        if (form != DW_FORM::indirect)
                return;

        cursor c(cu->data(), offset);
        do {
                form = (DW_FORM)c.uleb128();
        } while (form == DW_FORM::indirect);
        typ = attribute_spec(name, form).type;
        offset = c.get_section_offset();
}

string
to_string(const value &v)
{
        switch (v.get_type()) {
        case value::type::invalid:
                return "<invalid value type>";
        case value::type::address:
                return "0x" + to_hex(v.as_address());
        case value::type::block: {
                size_t size;
                const char *b = (const char*)v.as_block(&size);
                string res = ::to_string(size) + " byte block:";
                for (size_t pos = 0; pos < size; ++pos) {
                        res += ' ';
                        res += to_hex(b[pos]);
                }
                return res;
        }
        case value::type::constant:
                return "0x" + to_hex(v.as_uconstant());
        case value::type::uconstant:
                return ::to_string(v.as_uconstant());
        case value::type::sconstant:
                return ::to_string(v.as_sconstant());
        case value::type::exprloc:
                // XXX
                return "<exprloc>";
        case value::type::flag:
                return v.as_flag() ? "true" : "false";
        case value::type::line:
                return "<line 0x" + to_hex(v.as_sec_offset()) + ">";
        case value::type::loclist:
                return "<loclist 0x" + to_hex(v.as_sec_offset()) + ">";
        case value::type::mac:
                return "<mac 0x" + to_hex(v.as_sec_offset()) + ">";
        case value::type::rangelist:
                return "<rangelist 0x" + to_hex(v.as_sec_offset()) + ">";
        case value::type::reference: {
                die d = v.as_reference();
                auto tu = dynamic_cast<const type_unit*>(&d.get_unit());
                if (tu)
                        return "<.debug_types+0x" + to_hex(d.get_section_offset()) + ">";
                return "<0x" + to_hex(d.get_section_offset()) + ">";
        }
        case value::type::string:
                return v.as_string();
        }
        return "<unexpected value type " + to_string(v.get_type()) + ">";
}

DWARFPP_END_NAMESPACE
