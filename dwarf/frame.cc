// Copyright (c) 2013 Austin T. Clements. All rights reserved.
// Use of this source code is governed by an MIT license
// that can be found in the LICENSE file.

#include "frame.hh"
#include "internal.hh"

#include <algorithm>

using namespace std;

DWARFPP_BEGIN_NAMESPACE

namespace {

//////////////////////////////////////////////////////////////////
// Utilities
//

format_error
frame_err(section_offset off, const string &msg)
{
        return format_error(".debug_frame:0x" + to_hex(off) + ": " + msg);
}

uint64_t
checked_add(uint64_t a, uint64_t b, section_offset off, const char *what)
{
        if (b > UINT64_MAX - a)
                throw frame_err(off, string(what) + " overflows");
        return a + b;
}

uint64_t
checked_mul(uint64_t a, uint64_t b, section_offset off, const char *what)
{
        if (a != 0 && b > UINT64_MAX / a)
                throw frame_err(off, string(what) + " overflows");
        return a * b;
}

int64_t
checked_smul(int64_t a, int64_t b, section_offset off, const char *what)
{
        bool overflow;
        if (a > 0) {
                overflow = b > 0 ? a > INT64_MAX / b : b < INT64_MIN / a;
        } else if (a < 0) {
                overflow = b > 0 ? a < INT64_MIN / b
                         : b < 0 ? a < INT64_MAX / b : false;
        } else {
                overflow = false;
        }
        if (overflow)
                throw frame_err(off, string(what) + " overflows");
        return a * b;
}

int64_t
to_i64(uint64_t v, section_offset off, const char *what)
{
        if (v > (uint64_t)INT64_MAX)
                throw frame_err(off, string(what) + " too large");
        return (int64_t)v;
}

//////////////////////////////////////////////////////////////////
// CFI interpreter
//

// A complete rule state: the CFA rule plus the rules of all
// registers that have one.  Registers absent from regs are
// undefined.
struct rule_set
{
        frame_cfa_rule cfa;
        map<uint64_t, frame_register_rule> regs;
};

struct cie_data
{
        uint64_t code_align;
        int64_t data_align;
        uint64_t return_reg;
        rule_set initial;
};

// Interprets a CFI instruction stream, either a CIE's initial
// instructions (rows == nullptr) or an FDE's program.
struct interp
{
        uint64_t code_align;
        int64_t data_align;
        unsigned addr_size;
        // Rules restored by DW_CFA_restore*.
        const rule_set *initial;
        rule_set state;
        vector<rule_set> stack;
        taddr loc, loc_limit;
        vector<frame_row> *rows;

        interp(uint64_t code_align, int64_t data_align, unsigned addr_size,
               const rule_set *initial, vector<frame_row> *rows)
                : code_align(code_align), data_align(data_align),
                  addr_size(addr_size), initial(initial), loc(0),
                  loc_limit(UINT64_MAX), rows(rows) { }

        void add_row(taddr begin, taddr end)
        {
                if (begin == end || !rows)
                        return;
                if (!rows->empty() && rows->back().end == begin &&
                    rows->back().cfa == state.cfa &&
                    rows->back().registers == state.regs) {
                        // Same state as the previous row; just
                        // extend it.
                        rows->back().end = end;
                        return;
                }
                frame_row row;
                row.begin = begin;
                row.end = end;
                row.cfa = state.cfa;
                row.registers = state.regs;
                rows->push_back(std::move(row));
        }

        void advance(taddr delta, section_offset off)
        {
                if (delta == 0)
                        return;
                // loc <= loc_limit is an invariant, so this also
                // catches loc + delta wrapping around.
                if (delta > loc_limit - loc)
                        throw frame_err(off, "location advance out of range");
                add_row(loc, loc + delta);
                loc += delta;
        }

        void advance_to(taddr target, section_offset off)
        {
                if (target < loc)
                        throw frame_err(off, "set_loc moves location backward");
                advance(target - loc, off);
        }

        taddr read_address(cursor &cur)
        {
                if (addr_size == 4)
                        return cur.fixed<uint32_t>();
                return cur.fixed<uint64_t>();
        }

        void restore(uint64_t reg)
        {
                auto it = initial->regs.find(reg);
                if (it == initial->regs.end())
                        state.regs.erase(reg);
                else
                        state.regs[reg] = it->second;
        }

        void set_offset(uint64_t reg, uint64_t factor, section_offset off)
        {
                int64_t offset = checked_smul(to_i64(factor, off, "offset factor"),
                                              data_align, off, "register offset");
                state.regs[reg] = frame_register_rule::at_offset(offset);
        }

        void set_offset_sf(uint64_t reg, int64_t factor, section_offset off)
        {
                int64_t offset = checked_smul(factor, data_align, off,
                                              "register offset");
                state.regs[reg] = frame_register_rule::at_offset(offset);
        }

        void set_val_offset(uint64_t reg, uint64_t factor, section_offset off)
        {
                int64_t offset = checked_smul(to_i64(factor, off, "offset factor"),
                                              data_align, off, "register offset");
                state.regs[reg] = frame_register_rule::at_val_offset(offset);
        }

        void set_val_offset_sf(uint64_t reg, int64_t factor, section_offset off)
        {
                int64_t offset = checked_smul(factor, data_align, off,
                                              "register offset");
                state.regs[reg] = frame_register_rule::at_val_offset(offset);
        }

        // DWARF4 section 6.4.2
        void run(cursor &cur)
        {
                while (!cur.end()) {
                        section_offset off = cur.get_section_offset();
                        uint8_t op = cur.fixed<uint8_t>();

                        // Primary opcodes: opcode in the high two
                        // bits, operand in the low six.
                        switch (op & 0xC0) {
                        case 0x40:      // DW_CFA_advance_loc
                                advance(checked_mul(op & 0x3F, code_align, off,
                                                    "location delta"), off);
                                continue;
                        case 0x80: {    // DW_CFA_offset
                                uint64_t factor = cur.uleb128();
                                set_offset(op & 0x3F, factor, off);
                                continue;
                        }
                        case 0xC0:      // DW_CFA_restore
                                restore(op & 0x3F);
                                continue;
                        default:
                                break;
                        }

                        switch (op) {
                        case 0x00:      // DW_CFA_nop
                                break;
                        case 0x01: {    // DW_CFA_set_loc
                                taddr target = read_address(cur);
                                advance_to(target, off);
                                break;
                        }
                        case 0x02:      // DW_CFA_advance_loc1
                                advance(checked_mul(cur.fixed<uint8_t>(),
                                                    code_align, off,
                                                    "location delta"), off);
                                break;
                        case 0x03:      // DW_CFA_advance_loc2
                                advance(checked_mul(cur.fixed<uint16_t>(),
                                                    code_align, off,
                                                    "location delta"), off);
                                break;
                        case 0x04:      // DW_CFA_advance_loc4
                                advance(checked_mul(cur.fixed<uint32_t>(),
                                                    code_align, off,
                                                    "location delta"), off);
                                break;
                        case 0x05: {    // DW_CFA_offset_extended
                                uint64_t reg = cur.uleb128();
                                uint64_t factor = cur.uleb128();
                                set_offset(reg, factor, off);
                                break;
                        }
                        case 0x06:      // DW_CFA_restore_extended
                                restore(cur.uleb128());
                                break;
                        case 0x07:      // DW_CFA_undefined
                                state.regs.erase(cur.uleb128());
                                break;
                        case 0x08:      // DW_CFA_same_value
                                state.regs[cur.uleb128()] =
                                        frame_register_rule::same_value();
                                break;
                        case 0x09: {    // DW_CFA_register
                                uint64_t reg = cur.uleb128();
                                uint64_t reg2 = cur.uleb128();
                                state.regs[reg] =
                                        frame_register_rule::in_register(reg2);
                                break;
                        }
                        case 0x0a:      // DW_CFA_remember_state
                                stack.push_back(state);
                                break;
                        case 0x0b:      // DW_CFA_restore_state
                                if (stack.empty())
                                        throw frame_err(off, "state stack underflow");
                                state = stack.back();
                                stack.pop_back();
                                break;
                        case 0x0c: {    // DW_CFA_def_cfa
                                uint64_t reg = cur.uleb128();
                                uint64_t offset = cur.uleb128();
                                state.cfa = frame_cfa_rule(
                                        reg, to_i64(offset, off, "CFA offset"));
                                break;
                        }
                        case 0x0d:      // DW_CFA_def_cfa_register
                                if (!state.cfa.defined)
                                        throw frame_err(off, "def_cfa_register "
                                                        "with undefined CFA");
                                state.cfa.reg = cur.uleb128();
                                break;
                        case 0x0e:      // DW_CFA_def_cfa_offset
                                if (!state.cfa.defined)
                                        throw frame_err(off, "def_cfa_offset "
                                                        "with undefined CFA");
                                state.cfa.offset =
                                        to_i64(cur.uleb128(), off, "CFA offset");
                                break;
                        case 0x0f:      // DW_CFA_def_cfa_expression
                        case 0x10:      // DW_CFA_expression
                        case 0x16:      // DW_CFA_val_expression
                                throw frame_err(off, "expression CFI rules "
                                                "not supported");
                        case 0x11: {    // DW_CFA_offset_extended_sf
                                uint64_t reg = cur.uleb128();
                                int64_t factor = cur.sleb128();
                                set_offset_sf(reg, factor, off);
                                break;
                        }
                        case 0x12: {    // DW_CFA_def_cfa_sf
                                uint64_t reg = cur.uleb128();
                                int64_t factor = cur.sleb128();
                                state.cfa = frame_cfa_rule(
                                        reg, checked_smul(factor, data_align,
                                                          off, "CFA offset"));
                                break;
                        }
                        case 0x13:      // DW_CFA_def_cfa_offset_sf
                                if (!state.cfa.defined)
                                        throw frame_err(off, "def_cfa_offset_sf "
                                                        "with undefined CFA");
                                state.cfa.offset =
                                        checked_smul(cur.sleb128(), data_align,
                                                     off, "CFA offset");
                                break;
                        case 0x14: {    // DW_CFA_val_offset
                                uint64_t reg = cur.uleb128();
                                uint64_t factor = cur.uleb128();
                                set_val_offset(reg, factor, off);
                                break;
                        }
                        case 0x15: {    // DW_CFA_val_offset_sf
                                uint64_t reg = cur.uleb128();
                                int64_t factor = cur.sleb128();
                                set_val_offset_sf(reg, factor, off);
                                break;
                        }
                        default:
                                throw frame_err(off, "unknown CFI opcode 0x" +
                                                to_hex(op));
                        }
                }
        }
};

//////////////////////////////////////////////////////////////////
// Section parser
//

struct parser
{
        const shared_ptr<section> &sec;
        unsigned addr_size;
        map<section_offset, cie_data> cies;
        vector<frame_table::fde> fdes;

        // Read a record header at cur and return a cursor over the
        // record body.  Advances cur past the whole record.
        cursor record_body(cursor &cur, section_offset rec_off)
        {
                uint32_t length = cur.fixed<uint32_t>();
                if (length == 0xffffffff)
                        throw frame_err(rec_off, "DWARF64 frame records "
                                        "not supported");
                if (length >= 0xfffffff0)
                        throw frame_err(rec_off, "reserved initial "
                                        "length value");
                if (length < 4)
                        throw frame_err(rec_off, "frame record too short "
                                        "for CIE id");
                if ((uint64_t)(sec->end - cur.pos) < length)
                        throw frame_err(rec_off, "truncated frame record");
                shared_ptr<section> subsec =
                        sec->slice(cur.get_section_offset(), length);
                cur += length;
                return cursor(subsec);
        }

        void parse_cie_body(cursor &sub, section_offset rec_off, cie_data *cie)
        {
                uint8_t version = sub.fixed<uint8_t>();
                if (version != 3)
                        throw frame_err(rec_off, "unsupported CIE version " +
                                        std::to_string(version));
                string aug;
                sub.string(aug);
                if (!aug.empty())
                        throw frame_err(rec_off, "CIE augmentations "
                                        "not supported");
                cie->code_align = sub.uleb128();
                cie->data_align = sub.sleb128();
                cie->return_reg = sub.uleb128();
                if (cie->code_align == 0)
                        throw frame_err(rec_off, "zero code alignment factor");
                if (cie->data_align == 0)
                        throw frame_err(rec_off, "zero data alignment factor");

                // Initial instructions start from a completely
                // undefined state.
                rule_set empty;
                interp it(cie->code_align, cie->data_align, addr_size,
                          &empty, nullptr);
                it.run(sub);
                cie->initial = it.state;
        }

        const cie_data &get_cie(uint32_t off, section_offset fde_off)
        {
                auto it = cies.find(off);
                if (it != cies.end())
                        return it->second;
                if ((uint64_t)off >= sec->size())
                        throw frame_err(fde_off, "invalid CIE reference 0x" +
                                        to_hex(off));
                cie_data cie;
                cursor c(sec, off);
                try {
                        cursor body = record_body(c, off);
                        uint32_t id = body.fixed<uint32_t>();
                        if (id != 0xffffffff)
                                throw frame_err(fde_off, "CIE reference 0x" +
                                                to_hex(off) +
                                                " does not name a CIE");
                        parse_cie_body(body, off, &cie);
                } catch (const underflow_error &) {
                        throw frame_err(off, "truncated frame record");
                }
                return cies.insert(make_pair((section_offset)off, cie))
                        .first->second;
        }

        void parse_fde_body(cursor &sub, section_offset rec_off,
                            uint32_t cie_off)
        {
                const cie_data &cie = get_cie(cie_off, rec_off);
                interp it(cie.code_align, cie.data_align, addr_size,
                          &cie.initial, nullptr);
                taddr initial_location = it.read_address(sub);
                taddr address_range = it.read_address(sub);
                taddr fde_end = checked_add(initial_location, address_range,
                                            rec_off, "FDE range");
                vector<frame_row> rows;
                it.rows = &rows;
                it.state = cie.initial;
                it.loc = initial_location;
                it.loc_limit = fde_end;
                it.run(sub);
                // The last row extends to the end of the FDE's range.
                it.add_row(it.loc, fde_end);

                frame_table::fde f;
                f.initial_location = initial_location;
                f.address_range = address_range;
                f.code_alignment = cie.code_align;
                f.data_alignment = cie.data_align;
                f.return_address_register = cie.return_reg;
                f.initial_state.cfa = cie.initial.cfa;
                f.initial_state.registers = cie.initial.regs;
                f.rows = std::move(rows);
                fdes.push_back(std::move(f));
        }

        void run()
        {
                cursor cur(sec);
                while (!cur.end()) {
                        section_offset rec_off = cur.get_section_offset();
                        try {
                                cursor body = record_body(cur, rec_off);
                                uint32_t id = body.fixed<uint32_t>();
                                if (id == 0xffffffff) {
                                        cie_data cie;
                                        parse_cie_body(body, rec_off, &cie);
                                        cies[rec_off] = cie;
                                } else {
                                        parse_fde_body(body, rec_off, id);
                                }
                        } catch (const underflow_error &) {
                                throw frame_err(rec_off,
                                                "truncated frame record");
                        }
                }
        }
};

//////////////////////////////////////////////////////////////////
// Output generation
//

struct writer
{
        vector<char> buf;

        size_t size() const
        {
                return buf.size();
        }

        void u8(uint8_t v)
        {
                buf.push_back((char)v);
        }

        void u16(uint16_t v)
        {
                u8((uint8_t)v);
                u8((uint8_t)(v >> 8));
        }

        void u32(uint32_t v)
        {
                u16((uint16_t)v);
                u16((uint16_t)(v >> 16));
        }

        void u64(uint64_t v)
        {
                u32((uint32_t)v);
                u32((uint32_t)(v >> 32));
        }

        void uleb(uint64_t v)
        {
                do {
                        uint8_t b = v & 0x7f;
                        v >>= 7;
                        u8(b | (v ? 0x80 : 0));
                } while (v);
        }

        void sleb(int64_t v)
        {
                for (;;) {
                        uint8_t b = v & 0x7f;
                        v >>= 7;
                        if ((v == 0 && !(b & 0x40)) ||
                            (v == -1 && (b & 0x40))) {
                                u8(b);
                                return;
                        }
                        u8(b | 0x80);
                }
        }

        void addr(uint64_t v, unsigned addr_size)
        {
                if (addr_size == 4) {
                        if (v > 0xffffffffu)
                                throw format_error("address does not fit "
                                                   "in 4 bytes");
                        u32((uint32_t)v);
                } else {
                        u64(v);
                }
        }

        void patch32(size_t pos, uint32_t v)
        {
                for (unsigned i = 0; i < 4; i++)
                        buf[pos + i] = (char)(v >> (8 * i));
        }
};

// Emit instructions moving the location counter from *cur_loc to
// target.  The output CIE uses a code alignment factor of 1, so any
// delta is exact; targets are never rounded.
void
emit_advance(writer &w, unsigned addr_size, taddr *cur_loc, taddr target)
{
        taddr delta = target - *cur_loc;
        if (delta == 0)
                return;
        if (delta <= 0x3f) {
                w.u8(0x40 | (uint8_t)delta);            // advance_loc
        } else if (delta <= 0xff) {
                w.u8(0x02);                             // advance_loc1
                w.u8((uint8_t)delta);
        } else if (delta <= 0xffff) {
                w.u8(0x03);                             // advance_loc2
                w.u16((uint16_t)delta);
        } else if (delta <= 0xffffffffu) {
                w.u8(0x04);                             // advance_loc4
                w.u32((uint32_t)delta);
        } else {
                w.u8(0x01);                             // set_loc
                w.addr(target, addr_size);
        }
        *cur_loc = target;
}

int64_t
exact_factor(int64_t offset, int64_t data_align, const char *what)
{
        if (offset % data_align != 0)
                throw format_error(string(what) + " not expressible with "
                                   "the CIE data alignment factor");
        return offset / data_align;
}

void
emit_register_rule(writer &w, uint64_t reg, const frame_register_rule &rule,
                   int64_t data_align)
{
        switch (rule.type) {
        case frame_rule_type::same_value:
                w.u8(0x08);                     // DW_CFA_same_value
                w.uleb(reg);
                break;
        case frame_rule_type::offset: {
                int64_t factor = exact_factor(rule.offset, data_align,
                                              "register offset");
                if (reg < 64 && factor >= 0) {
                        w.u8(0x80 | (uint8_t)reg);      // DW_CFA_offset
                        w.uleb((uint64_t)factor);
                } else if (factor >= 0) {
                        w.u8(0x05);             // DW_CFA_offset_extended
                        w.uleb(reg);
                        w.uleb((uint64_t)factor);
                } else {
                        w.u8(0x11);             // DW_CFA_offset_extended_sf
                        w.uleb(reg);
                        w.sleb(factor);
                }
                break;
        }
        case frame_rule_type::val_offset: {
                int64_t factor = exact_factor(rule.offset, data_align,
                                              "register offset");
                if (factor >= 0) {
                        w.u8(0x14);             // DW_CFA_val_offset
                        w.uleb(reg);
                        w.uleb((uint64_t)factor);
                } else {
                        w.u8(0x15);             // DW_CFA_val_offset_sf
                        w.uleb(reg);
                        w.sleb(factor);
                }
                break;
        }
        case frame_rule_type::reg:
                w.u8(0x09);                     // DW_CFA_register
                w.uleb(reg);
                w.uleb(rule.reg);
                break;
        case frame_rule_type::undefined:
                // Undefined rules are represented by absence and
                // never stored.
                throw format_error("internal: cannot emit undefined rule");
        }
}

// Emit the instructions needed to take the emitted state to the
// target state, and update *emitted.
void
emit_state(writer &w, const rule_set &target, rule_set *emitted,
           int64_t data_align)
{
        if (target.cfa != emitted->cfa) {
                if (!target.cfa.defined)
                        throw format_error("cannot encode a row whose CFA "
                                           "becomes undefined");
                if (target.cfa.offset >= 0) {
                        w.u8(0x0c);             // DW_CFA_def_cfa
                        w.uleb(target.cfa.reg);
                        w.uleb((uint64_t)target.cfa.offset);
                } else {
                        w.u8(0x12);             // DW_CFA_def_cfa_sf
                        w.uleb(target.cfa.reg);
                        w.sleb(exact_factor(target.cfa.offset, data_align,
                                            "CFA offset"));
                }
        }
        // Registers that had a rule but no longer do
        for (auto &kv : emitted->regs) {
                if (target.regs.find(kv.first) == target.regs.end()) {
                        w.u8(0x07);             // DW_CFA_undefined
                        w.uleb(kv.first);
                }
        }
        // New and changed rules
        for (auto &kv : target.regs) {
                auto it = emitted->regs.find(kv.first);
                if (it != emitted->regs.end() && it->second == kv.second)
                        continue;
                emit_register_rule(w, kv.first, kv.second, data_align);
        }
        *emitted = target;
}

void
emit_segment(writer &w, unsigned addr_size, const frame_table::fde &f,
             const frame_segment &s)
{
        taddr old_end = s.old_start + s.length;

        // One CIE per emitted FDE.  A code alignment factor of 1
        // lets row boundaries land on any address; the source data
        // alignment factor is kept so factored offsets stay exact.
        // The initial instructions are empty: every row's state is
        // established explicitly by the FDE, so no state (and no
        // remember_state stack) is shared across a cut.
        size_t cie_off = w.size();
        w.u32(0);                       // length, patched below
        w.u32(0xffffffff);              // CIE id
        w.u8(3);                        // version
        w.u8(0);                        // empty augmentation
        w.uleb(1);                      // code alignment factor
        w.sleb(f.data_alignment);
        w.uleb(f.return_address_register);
        w.patch32(cie_off, (uint32_t)(w.size() - cie_off - 4));

        size_t fde_off = w.size();
        w.u32(0);                       // length, patched below
        w.u32((uint32_t)cie_off);       // CIE pointer
        w.addr(s.new_start, addr_size);
        w.addr(s.length, addr_size);

        taddr cur_loc = s.new_start;
        rule_set emitted;
        for (auto &row : f.rows) {
                if (row.end <= s.old_start || row.begin >= old_end)
                        continue;
                taddr nb = std::max(row.begin, s.old_start)
                        - s.old_start + s.new_start;
                emit_advance(w, addr_size, &cur_loc, nb);
                rule_set target;
                target.cfa = row.cfa;
                target.regs = row.registers;
                emit_state(w, target, &emitted, f.data_alignment);
        }
        w.patch32(fde_off, (uint32_t)(w.size() - fde_off - 4));
}

} // namespace

//////////////////////////////////////////////////////////////////
// class frame_table
//

frame_table::frame_table(const dwarf &file, unsigned address_size)
        : frame_table(file.get_section(section_type::frame), address_size)
{
}

frame_table::frame_table(const shared_ptr<section> &sec, unsigned address_size)
        : addr_size(address_size)
{
        if (address_size != 4 && address_size != 8)
                throw format_error("unsupported frame address size " +
                                   std::to_string(address_size));
        if (sec->ord != byte_order::lsb)
                throw format_error("big-endian .debug_frame not supported");
        parser p{sec, address_size};
        p.run();
        fdes_ = std::move(p.fdes);
}

const frame_row *
frame_table::fde::find_row(taddr addr) const
{
        for (auto &row : rows)
                if (row.covers(addr))
                        return &row;
        return nullptr;
}

const frame_table::fde *
frame_table::find_fde(taddr addr) const
{
        for (auto &f : fdes_)
                if (f.covers(addr))
                        return &f;
        return nullptr;
}

const frame_row *
frame_table::find_row(taddr addr) const
{
        const fde *f = find_fde(addr);
        return f ? f->find_row(addr) : nullptr;
}

relocated_frame
frame_table::relocate(const vector<frame_segment> &segments) const
{
        if (addr_size != 4 && addr_size != 8)
                throw format_error("cannot relocate an invalid frame table");

        // Validate the segments themselves.
        for (size_t i = 0; i < segments.size(); i++) {
                const frame_segment &s = segments[i];
                string what = "segment " + std::to_string(i) + ": ";
                if (s.length == 0)
                        throw format_error(what + "empty segment");
                if (s.length > UINT64_MAX - s.old_start)
                        throw format_error(what + "source range wraps around");
                if (s.length > UINT64_MAX - s.new_start)
                        throw format_error(what + "target range wraps around");
                if (addr_size == 4 &&
                    s.new_start + s.length - 1 > 0xffffffffu)
                        throw format_error(what + "target range does not fit "
                                           "the address size");
        }

        // Source ranges must not overlap each other; likewise targets.
        vector<const frame_segment *> sorted;
        for (auto &s : segments)
                sorted.push_back(&s);
        sort(sorted.begin(), sorted.end(),
             [](const frame_segment *a, const frame_segment *b) {
                     return a->old_start < b->old_start;
             });
        for (size_t i = 1; i < sorted.size(); i++)
                if (sorted[i]->old_start <
                    sorted[i-1]->old_start + sorted[i-1]->length)
                        throw format_error("overlapping source ranges");
        sort(sorted.begin(), sorted.end(),
             [](const frame_segment *a, const frame_segment *b) {
                     return a->new_start < b->new_start;
             });
        for (size_t i = 1; i < sorted.size(); i++)
                if (sorted[i]->new_start <
                    sorted[i-1]->new_start + sorted[i-1]->length)
                        throw format_error("overlapping target ranges");

        // Each segment must lie within exactly one original FDE.
        vector<const fde *> fde_of(segments.size());
        for (size_t i = 0; i < segments.size(); i++) {
                taddr old_end = segments[i].old_start + segments[i].length;
                const fde *found = nullptr;
                for (auto &f : fdes_) {
                        if (f.initial_location <= segments[i].old_start &&
                            old_end <= f.end()) {
                                if (found)
                                        throw format_error("segment " +
                                                std::to_string(i) + " lies in "
                                                "more than one FDE");
                                found = &f;
                        }
                }
                if (!found)
                        throw format_error("segment " + std::to_string(i) +
                                           " is not contained in any FDE");
                fde_of[i] = found;
        }

        writer w;
        for (size_t i = 0; i < segments.size(); i++)
                emit_segment(w, addr_size, *fde_of[i], segments[i]);

        // Read the output back so the result owns its rules and the
        // bytes are known to round-trip.
        static const char empty = 0;
        auto out_sec = make_shared<section>(section_type::frame,
                w.buf.empty() ? (const void *)&empty : (const void *)w.buf.data(),
                w.buf.size(), byte_order::lsb);
        frame_table out_table(out_sec, addr_size);
        return relocated_frame(std::move(w.buf), std::move(out_table));
}

DWARFPP_END_NAMESPACE
