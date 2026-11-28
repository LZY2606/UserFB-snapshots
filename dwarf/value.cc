// Copyright (c) 2013 Austin T. Clements. All rights reserved.
// Use of this source code is governed by an MIT license
// that can be found in the LICENSE file.

#include "internal.hh"

#include <cstring>

using namespace std;

DWARFPP_BEGIN_NAMESPACE

namespace {

// DWARF 5 section 7.26: .debug_str_offsets and .debug_addr are
// organized as contributions.  Each contribution starts with an
// initial length, a 2-byte version, and 2 more bytes of header
// (padding for .debug_str_offsets; address and segment selector sizes
// for .debug_addr).
struct contribution
{
        format fmt;
        section_offset begin;
        section_offset end;
        section_offset entries_begin;
        unsigned address_size;
};

/**
 * Parse the contribution that begins at offset begin in sec.
 */
contribution
read_contribution(const shared_ptr<section> &sec, section_offset begin)
{
        cursor cur(sec, begin);
        shared_ptr<section> sub = cur.subsection();
        section_offset end = cur.get_section_offset();

        cursor h(sub);
        h.skip_initial_length();
        uhalf version = h.fixed<uhalf>();
        if (version != 5)
                throw format_error("unknown indexed table contribution version " +
                                   std::to_string(version));

        unsigned address_size = 0;
        if (sec->type == section_type::addr) {
                address_size = h.fixed<ubyte>();
                (void)h.fixed<ubyte>(); // segment selector size
        } else {
                (void)h.fixed<uhalf>(); // padding
        }

        contribution c;
        c.fmt = sub->fmt;
        c.begin = begin;
        c.end = end;
        c.entries_begin = begin + h.get_section_offset();
        c.address_size = address_size;
        return c;
}

/**
 * Locate the contribution of sec that contains base.  Throws
 * format_error if base is not inside any contribution.
 */
contribution
find_contribution(const shared_ptr<section> &sec, section_offset base)
{
        cursor cur(sec);
        while (!cur.end()) {
                section_offset begin = cur.get_section_offset();
                contribution c = read_contribution(sec, begin);
                if (base >= c.begin && base < c.end) {
                        if (base < c.entries_begin)
                                throw format_error("indexed table base points into contribution header");
                        return c;
                }
                // Skip to just past this contribution.  read_contribution
                // left its own cursor at c.end; construct a cursor
                // there to continue scanning.
                cursor next(sec, c.end);
                cur = next;
        }
        throw format_error("indexed table base outside section contributions");
}

/**
 * If a unit omits its DW_AT_*_base attribute, the base defaults to
 * the first entry of the first contribution.
 */
section_offset
default_table_base(const shared_ptr<section> &sec)
{
        if (sec->size() == 0)
                throw format_error("indexed table section is empty");
        return read_contribution(sec, 0).entries_begin;
}

uint64_t
read_uint_at(const shared_ptr<section> &sec, section_offset off,
             unsigned bytes)
{
        cursor cur(sec, off);
        switch (bytes) {
        case 4:
                return cur.fixed<uint32_t>();
        case 8:
                return cur.fixed<uint64_t>();
        default:
                throw logic_error("bad indexed table entry size");
        }
}

/**
 * Read the index of an indexed form (strx/addrx and their sized
 * variants).  The index itself always lives in the DIE.
 */
uint64_t
read_index(const unit *cu, DW_FORM form, section_offset offset)
{
        cursor cur(cu->data(), offset);
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
        case DW_FORM::addrx3:
                return cur.u24();
        case DW_FORM::strx4:
        case DW_FORM::addrx4:
                return cur.fixed<uint32_t>();
        default:
                throw value_type_mismatch("not an indexed form");
        }
}

/**
 * Compute the byte offset of entry index in the contribution
 * containing base and bounds-check it against that contribution (not
 * merely against the section).  entry_size is the stride in bytes.
 */
section_offset
entry_offset(const contribution &c, section_offset base,
             uint64_t index, unsigned entry_size)
{
        if (base < c.entries_begin || base > c.end)
                throw format_error("indexed table base outside contribution entries");
        section_offset available = c.end - base;
        if (index > available / entry_size)
                throw format_error("indexed table entry out of range");
        section_offset entry = base + index * entry_size;
        if (entry + entry_size > c.end)
                throw format_error("indexed table entry out of range");
        return entry;
}

const char *
resolve_strx(const unit *cu, uint64_t index, size_t *size_out)
{
        section_offset base;
        shared_ptr<section> sec =
                cu->get_dwarf().get_section(section_type::str_offsets);
        if (cu->root_attribute(DW_AT::str_offsets_base, &base)) {
                // use the explicit base
        } else {
                base = default_table_base(sec);
        }

        contribution c = find_contribution(sec, base);
        unsigned entry_size = c.fmt == format::dwarf64 ? 8 : 4;
        section_offset entry = entry_offset(c, base, index, entry_size);
        section_offset str_off = read_uint_at(sec, entry, entry_size);

        shared_ptr<section> strs =
                cu->get_dwarf().get_section(section_type::str);
        if (str_off >= strs->size())
                throw format_error("string offset outside .debug_str");
        cursor scur(strs, str_off);
        return scur.cstr(size_out);
}

taddr
resolve_addrx(const unit *cu, uint64_t index)
{
        section_offset base;
        shared_ptr<section> sec =
                cu->get_dwarf().get_section(section_type::addr);
        if (cu->root_attribute(DW_AT::addr_base, &base)) {
                // use the explicit base
        } else {
                base = default_table_base(sec);
        }

        contribution c = find_contribution(sec, base);
        // The entry width is the address size of the table (which
        // must agree with the unit's address size).  The address is
        // read with that width in the section's byte order.
        unsigned addr_size = c.address_size ? c.address_size
                                            : cu->data()->addr_size;
        if (addr_size != 1 && addr_size != 2 && addr_size != 4 && addr_size != 8)
                throw format_error("unsupported address size in .debug_addr");
        section_offset entry = entry_offset(c, base, index, addr_size);

        // Slice with the unit's address size so cursor::address uses
        // the correct width.
        shared_ptr<section> one = sec->slice(entry, addr_size,
                                             format::unknown, addr_size);
        cursor cur(one);
        return cur.address();
}

} // namespace

value::value(const unit *cu,
             DW_AT name, DW_FORM form, type typ, section_offset offset,
             std::int64_t implicit_value)
        : cu(cu), form(form), typ(typ), offset(offset),
          implicit_value(implicit_value) {
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
        switch (form) {
        case DW_FORM::addr: {
                cursor cur(cu->data(), offset);
                return cur.address();
        }
        case DW_FORM::addrx:
        case DW_FORM::addrx1:
        case DW_FORM::addrx2:
        case DW_FORM::addrx3:
        case DW_FORM::addrx4:
                return resolve_addrx(cu, read_index(cu, form, offset));
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
        case DW_FORM::implicit_const:
                // The value is encoded in the abbrev as an SLEB128.
                // Constant forms may be read either signed or unsigned.
                return (uint64_t)implicit_value;
        case DW_FORM::sec_offset:
                return cur.offset();
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
                return implicit_value;
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
        case DW_FORM::strx4:
                return resolve_strx(cu, read_index(cu, form, offset), size_out);
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
        DW_FORM resolved;
        do {
                resolved = (DW_FORM)c.uleb128();
        } while (resolved == DW_FORM::indirect);
        // DW_FORM_implicit_const is only permitted in an abbrev table
        // (DWARF 5 section 7.5.3), so it has no value in the DIE here.
        form = resolved;
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
