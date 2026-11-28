// Copyright (c) 2013 Austin T. Clements. All rights reserved.
// Use of this source code is governed by an MIT license
// that can be found in the LICENSE file.

#include "internal.hh"
#include "frame.hh"

#include <algorithm>
#include <cstring>

using namespace std;

DWARFPP_BEGIN_NAMESPACE

// Call frame instruction encodings (DWARF4 section 7.23).  The
// primary opcodes occupy the upper two bits of the opcode byte.
namespace
{
        const ubyte CFA_advance_loc        = 0x40; // high 2 bits, delta in low 6
        const ubyte CFA_offset             = 0x80; // high 2 bits, register in low 6
        const ubyte CFA_restore            = 0xc0; // high 2 bits, register in low 6

        const ubyte CFA_nop                = 0x00;
        const ubyte CFA_set_loc            = 0x01;
        const ubyte CFA_advance_loc1       = 0x02;
        const ubyte CFA_advance_loc2       = 0x03;
        const ubyte CFA_advance_loc4       = 0x04;
        const ubyte CFA_offset_extended    = 0x05;
        const ubyte CFA_restore_extended   = 0x06;
        const ubyte CFA_undefined          = 0x07;
        const ubyte CFA_same_value         = 0x08;
        const ubyte CFA_register           = 0x09;
        const ubyte CFA_remember_state     = 0x0a;
        const ubyte CFA_restore_state      = 0x0b;
        const ubyte CFA_def_cfa            = 0x0c;
        const ubyte CFA_def_cfa_register   = 0x0d;
        const ubyte CFA_def_cfa_offset     = 0x0e;
        const ubyte CFA_def_cfa_expression = 0x0f;
        const ubyte CFA_expression         = 0x10;
        const ubyte CFA_offset_extended_sf = 0x11;
        const ubyte CFA_def_cfa_sf         = 0x12;
        const ubyte CFA_def_cfa_offset_sf  = 0x13;
        const ubyte CFA_val_offset         = 0x14;
        const ubyte CFA_val_offset_sf      = 0x15;
        const ubyte CFA_val_expression     = 0x16;
        const ubyte CFA_lo_user            = 0x1c;
        const ubyte CFA_hi_user            = 0x3f;
}

//////////////////////////////////////////////////////////////////
// Errors
//

static void
frame_error(section_offset off, const string &msg)
{
        throw format_error(".debug_frame+0x" + to_hex(off) + ": " + msg);
}

//////////////////////////////////////////////////////////////////
// Decoded entries
//

namespace
{
        struct cie
        {
                section_offset offset;
                uint64_t code_align;
                int64_t data_align;
                uint64_t return_address_register;
                // State established by the initial instructions
                frame_cfa_rule cfa;
                std::map<uint64_t, frame_reg_rule> regs;
        };

        struct fde
        {
                section_offset offset;
                // Offset of this FDE's instructions in .debug_frame
                section_offset prog_offset, prog_end;
                section_offset cie_offset;
                size_t cie_index;
                taddr begin, end;
        };
}

struct frame_table::impl
{
        unsigned addr_size;
        // A private copy of the section bytes, so the table does not
        // depend on the loader after construction.
        std::vector<char> bytes;
        std::vector<cie> cies;
        std::unordered_map<section_offset, size_t> cie_by_offset;
        std::vector<fde> fdes;
        std::vector<frame_row> all_rows;
};

//////////////////////////////////////////////////////////////////
// Instruction interpretation
//

namespace
{
        // A snapshot of the rules in effect at a point in a CFI
        // program.
        struct rule_state
        {
                frame_cfa_rule cfa;
                std::map<uint64_t, frame_reg_rule> regs;

                bool operator==(const rule_state &o) const
                {
                        return cfa == o.cfa && regs == o.regs;
                }
        };

        struct interp
        {
                const cie &c;
                // Section offset of the entry being interpreted, for
                // error messages.
                section_offset entry_off;
                // Current state
                rule_state state;
                std::vector<rule_state> stack;
                // Current location and the start of the pending row
                taddr loc, row_begin, fde_end;
                // If non-null, rows are appended here (FDE mode);
                // otherwise location-changing instructions are
                // rejected (CIE mode).
                std::vector<frame_row> *rows;

                interp(const cie &c, section_offset entry_off,
                       std::vector<frame_row> *rows, taddr begin, taddr end)
                        : c(c), entry_off(entry_off), loc(begin),
                          row_begin(begin), fde_end(end), rows(rows) { }

                void advance_to(taddr new_loc, section_offset op_off)
                {
                        if (!rows)
                                frame_error(op_off, "location advance in CIE "
                                            "initial instructions");
                        if (new_loc < loc)
                                frame_error(op_off, "location moved backward");
                        if (new_loc > fde_end)
                                frame_error(op_off, "location past end of FDE");
                        if (new_loc > row_begin) {
                                frame_row row;
                                row.begin = row_begin;
                                row.end = new_loc;
                                row.cfa = state.cfa;
                                row.regs = state.regs;
                                rows->push_back(std::move(row));
                        }
                        loc = new_loc;
                        row_begin = new_loc;
                }

                void advance(uint64_t delta, section_offset op_off)
                {
                        if (delta > ~0ULL - loc)
                                frame_error(op_off, "location overflow");
                        advance_to(loc + delta, op_off);
                }

                uint64_t scale(uint64_t operand, section_offset op_off) const
                {
                        // Multiply operand by code alignment with
                        // overflow checking.
                        if (c.code_align != 0 &&
                            operand > ~0ULL / c.code_align)
                                frame_error(op_off, "location overflow");
                        return operand * c.code_align;
                }

                int64_t data_scale_u(uint64_t operand, section_offset op_off) const
                {
                        // Multiply an unsigned operand by the data
                        // alignment factor with overflow checking.
                        uint64_t m = c.data_align < 0 ?
                                (uint64_t)-c.data_align : (uint64_t)c.data_align;
                        if (m != 0 && operand > (uint64_t)INT64_MAX / m)
                                frame_error(op_off, "rule offset overflow");
                        int64_t r = (int64_t)(operand * m);
                        return c.data_align < 0 ? -r : r;
                }

                int64_t data_scale_s(int64_t operand, section_offset op_off) const
                {
                        __int128 r = (__int128)operand * c.data_align;
                        if (r > INT64_MAX || r < INT64_MIN)
                                frame_error(op_off, "rule offset overflow");
                        return (int64_t)r;
                }

                void set_rule(uint64_t reg, const frame_reg_rule &rule)
                {
                        if (rule.kind == frame_reg_kind::undefined)
                                state.regs.erase(reg);
                        else
                                state.regs[reg] = rule;
                }

                void restore(uint64_t reg)
                {
                        auto it = c.regs.find(reg);
                        if (it == c.regs.end())
                                state.regs.erase(reg);
                        else
                                state.regs[reg] = it->second;
                }

                void def_cfa_register(uint64_t reg, section_offset op_off)
                {
                        if (!state.cfa.defined)
                                frame_error(op_off, "DW_CFA_def_cfa_register "
                                            "with undefined CFA");
                        state.cfa.reg = reg;
                }

                void def_cfa_offset(int64_t offset, section_offset op_off)
                {
                        if (!state.cfa.defined)
                                frame_error(op_off, "DW_CFA_def_cfa_offset "
                                            "with undefined CFA");
                        state.cfa.offset = offset;
                }

                void run(cursor &cur, section_offset end);
        };

        void
        interp::run(cursor &cur, section_offset end)
        {
                while (cur.get_section_offset() < end) {
                        section_offset op_off = cur.get_section_offset();
                        ubyte op = cur.fixed<ubyte>();

                        ubyte primary = op & 0xc0;
                        if (primary == CFA_offset) {
                                // DW_CFA_offset (register in low 6 bits)
                                uint64_t reg = op & 0x3f;
                                int64_t off = data_scale_u(cur.uleb128(), op_off);
                                set_rule(reg, frame_reg_rule::make_offset(off));
                        } else if (primary == CFA_advance_loc) {
                                advance(scale(op & 0x3f, op_off), op_off);
                        } else if (primary == CFA_restore) {
                                restore(op & 0x3f);
                        } else {
                                switch (op) {
                                case CFA_nop:
                                        break;
                                case CFA_set_loc:
                                        advance_to(cur.address(), op_off);
                                        break;
                                case CFA_advance_loc1:
                                        advance(scale(cur.fixed<ubyte>(), op_off), op_off);
                                        break;
                                case CFA_advance_loc2:
                                        advance(scale(cur.fixed<uhalf>(), op_off), op_off);
                                        break;
                                case CFA_advance_loc4:
                                        advance(scale(cur.fixed<uword>(), op_off), op_off);
                                        break;
                                case CFA_offset_extended: {
                                        uint64_t reg = cur.uleb128();
                                        int64_t off = data_scale_u(cur.uleb128(), op_off);
                                        set_rule(reg, frame_reg_rule::make_offset(off));
                                        break;
                                }
                                case CFA_restore_extended:
                                        restore(cur.uleb128());
                                        break;
                                case CFA_undefined:
                                        set_rule(cur.uleb128(), frame_reg_rule());
                                        break;
                                case CFA_same_value:
                                        set_rule(cur.uleb128(),
                                                 frame_reg_rule::make_same_value());
                                        break;
                                case CFA_register: {
                                        uint64_t reg = cur.uleb128();
                                        uint64_t reg2 = cur.uleb128();
                                        set_rule(reg, frame_reg_rule::make_reg(reg2));
                                        break;
                                }
                                case CFA_remember_state:
                                        stack.push_back(state);
                                        break;
                                case CFA_restore_state:
                                        if (stack.empty())
                                                frame_error(op_off, "state stack "
                                                            "underflow");
                                        state = stack.back();
                                        stack.pop_back();
                                        break;
                                case CFA_def_cfa: {
                                        uint64_t reg = cur.uleb128();
                                        uint64_t off = cur.uleb128();
                                        if (off > (uint64_t)INT64_MAX)
                                                frame_error(op_off, "rule offset "
                                                            "overflow");
                                        state.cfa = frame_cfa_rule(reg, (int64_t)off);
                                        break;
                                }
                                case CFA_def_cfa_register:
                                        def_cfa_register(cur.uleb128(), op_off);
                                        break;
                                case CFA_def_cfa_offset: {
                                        uint64_t off = cur.uleb128();
                                        if (off > (uint64_t)INT64_MAX)
                                                frame_error(op_off, "rule offset "
                                                            "overflow");
                                        def_cfa_offset((int64_t)off, op_off);
                                        break;
                                }
                                case CFA_def_cfa_sf: {
                                        uint64_t reg = cur.uleb128();
                                        int64_t off = data_scale_s(cur.sleb128(), op_off);
                                        state.cfa = frame_cfa_rule(reg, off);
                                        break;
                                }
                                case CFA_def_cfa_offset_sf:
                                        def_cfa_offset(data_scale_s(cur.sleb128(), op_off),
                                                       op_off);
                                        break;
                                case CFA_offset_extended_sf: {
                                        uint64_t reg = cur.uleb128();
                                        int64_t off = data_scale_s(cur.sleb128(), op_off);
                                        set_rule(reg, frame_reg_rule::make_offset(off));
                                        break;
                                }
                                case CFA_val_offset: {
                                        uint64_t reg = cur.uleb128();
                                        int64_t off = data_scale_u(cur.uleb128(), op_off);
                                        set_rule(reg, frame_reg_rule::make_val_offset(off));
                                        break;
                                }
                                case CFA_val_offset_sf: {
                                        uint64_t reg = cur.uleb128();
                                        int64_t off = data_scale_s(cur.sleb128(), op_off);
                                        set_rule(reg, frame_reg_rule::make_val_offset(off));
                                        break;
                                }
                                case CFA_def_cfa_expression:
                                case CFA_expression:
                                case CFA_val_expression:
                                        frame_error(op_off, "expression frame rules "
                                                    "not supported");
                                        break;
                                default:
                                        if (op >= CFA_lo_user && op <= CFA_hi_user)
                                                frame_error(op_off, "vendor frame "
                                                            "instruction not supported");
                                        frame_error(op_off, "unknown frame instruction "
                                                    "0x" + to_hex(op));
                                }
                        }
                }
                if (cur.get_section_offset() != end)
                        frame_error(entry_off, "frame instruction runs past "
                                    "end of entry");
        }

        // Append the rows of an FDE program to out, merging adjacent
        // rows with identical state.
        void
        run_fde(const cie &c, const fde &f, const std::shared_ptr<section> &sec,
                std::vector<frame_row> &out)
        {
                std::vector<frame_row> rows;
                interp it(c, f.offset, &rows, f.begin, f.end);
                it.state.cfa = c.cfa;
                it.state.regs = c.regs;
                cursor cur(sec, f.prog_offset);
                it.run(cur, f.prog_end);
                // The final state covers the rest of the FDE.
                if (it.row_begin < f.end) {
                        frame_row row;
                        row.begin = it.row_begin;
                        row.end = f.end;
                        row.cfa = it.state.cfa;
                        row.regs = it.state.regs;
                        rows.push_back(std::move(row));
                }
                // Merge adjacent rows with identical state (within
                // this FDE only), then append.
                std::vector<frame_row> merged;
                for (auto &row : rows) {
                        if (!merged.empty() && merged.back().end == row.begin &&
                            merged.back().same_state(row)) {
                                merged.back().end = row.end;
                        } else {
                                merged.push_back(std::move(row));
                        }
                }
                out.insert(out.end(), merged.begin(), merged.end());
        }
}

//////////////////////////////////////////////////////////////////
// class frame_table
//

frame_table::frame_table(const dwarf &file, unsigned addr_size)
        : m(make_shared<impl>())
{
        if (addr_size != 4 && addr_size != 8)
                throw invalid_argument("frame_table: address size must be 4 or 8");
        m->addr_size = addr_size;

        shared_ptr<section> src = file.get_section(section_type::frame);
        if (src->ord != byte_order::lsb)
                frame_error(0, "big-endian .debug_frame not supported");

        // Keep our own copy of the bytes so the table (and anything
        // derived from it) does not depend on the loader.
        m->bytes.assign(src->begin, src->begin + src->size());
        auto sec = make_shared<section>(section_type::frame, m->bytes.data(),
                                        m->bytes.size(), byte_order::lsb,
                                        format::dwarf32, addr_size);

        // First pass: decode entry headers, CIEs, and FDE programs.
        cursor cur(sec);
        while (!cur.end()) {
                section_offset entry_off = cur.get_section_offset();
                uint32_t length;
                try {
                        length = cur.fixed<uint32_t>();
                } catch (underflow_error &) {
                        frame_error(entry_off, "truncated frame entry");
                }
                if (length == 0xffffffff)
                        frame_error(entry_off, "DWARF64 frame entries not "
                                    "supported");
                if (length == 0)
                        break;          // Padding terminator
                section_offset entry_end = entry_off + 4 + (section_offset)length;
                if (entry_end > sec->size())
                        frame_error(entry_off, "truncated frame entry");

                try {
                        uint32_t id = cur.fixed<uint32_t>();
                        if (id == 0xffffffff) {
                                // CIE
                                cie c;
                                c.offset = entry_off;
                                ubyte version = cur.fixed<ubyte>();
                                if (version != 3)
                                        frame_error(entry_off, "unsupported CIE "
                                                    "version " +
                                                    std::to_string(version));
                                string aug;
                                cur.string(aug);
                                if (!aug.empty())
                                        frame_error(entry_off, "CIE augmentations "
                                                    "not supported");
                                c.code_align = cur.uleb128();
                                c.data_align = cur.sleb128();
                                if (c.code_align == 0)
                                        frame_error(entry_off, "zero code "
                                                    "alignment factor");
                                if (c.data_align == 0)
                                        frame_error(entry_off, "zero data "
                                                    "alignment factor");
                                c.return_address_register = cur.uleb128();
                                // Initial instructions
                                interp it(c, entry_off, nullptr, 0, 0);
                                it.run(cur, entry_end);
                                c.cfa = it.state.cfa;
                                c.regs = it.state.regs;
                                m->cie_by_offset[entry_off] = m->cies.size();
                                m->cies.push_back(std::move(c));
                        } else {
                                // FDE; id is the section offset of its CIE
                                fde f;
                                f.offset = entry_off;
                                f.cie_offset = id;
                                f.begin = cur.address();
                                taddr range = cur.address();
                                if (range > ~0ULL - f.begin)
                                        frame_error(entry_off, "FDE address "
                                                    "range wraps");
                                f.end = f.begin + range;
                                f.prog_offset = cur.get_section_offset();
                                f.prog_end = entry_end;
                                m->fdes.push_back(f);
                        }
                } catch (underflow_error &) {
                        frame_error(entry_off, "truncated frame entry");
                }
                cur = cursor(sec, entry_end);
        }

        // Second pass: resolve CIE references and interpret FDE
        // programs into rows.
        for (auto &f : m->fdes) {
                auto cie_it = m->cie_by_offset.find(f.cie_offset);
                if (cie_it == m->cie_by_offset.end())
                        frame_error(f.offset, "FDE references unknown CIE at "
                                    "offset 0x" + to_hex(f.cie_offset));
                f.cie_index = cie_it->second;
                try {
                        run_fde(m->cies[f.cie_index], f, sec, m->all_rows);
                } catch (underflow_error &) {
                        frame_error(f.offset, "truncated frame entry");
                }
        }
}

const std::vector<frame_row> &
frame_table::rows() const
{
        static const std::vector<frame_row> empty;
        if (!m)
                return empty;
        return m->all_rows;
}

const frame_row *
frame_table::find_row(taddr addr) const
{
        if (!m)
                return nullptr;
        for (auto &row : m->all_rows)
                if (row.begin <= addr && addr < row.end)
                        return &row;
        return nullptr;
}

//////////////////////////////////////////////////////////////////
// Relocation
//

namespace
{
        // Little-endian output writer.
        struct writer
        {
                std::vector<char> &out;

                writer(std::vector<char> &out) : out(out) { }

                void u8(uint8_t v)
                {
                        out.push_back((char)v);
                }

                void u16(uint16_t v)
                {
                        out.push_back((char)(v & 0xff));
                        out.push_back((char)((v >> 8) & 0xff));
                }

                void u32(uint32_t v)
                {
                        for (unsigned i = 0; i < 4; i++)
                                out.push_back((char)((v >> (i * 8)) & 0xff));
                }

                void u64(uint64_t v)
                {
                        for (unsigned i = 0; i < 8; i++)
                                out.push_back((char)((v >> (i * 8)) & 0xff));
                }

                void address(taddr v, unsigned addr_size)
                {
                        if (addr_size == 4)
                                u32((uint32_t)v);
                        else
                                u64(v);
                }

                void uleb128(uint64_t v)
                {
                        do {
                                uint8_t b = v & 0x7f;
                                v >>= 7;
                                if (v)
                                        b |= 0x80;
                                u8(b);
                        } while (v);
                }

                void sleb128(int64_t v)
                {
                        bool more;
                        do {
                                uint8_t b = v & 0x7f;
                                v >>= 7;
                                more = !((v == 0 && !(b & 0x40)) ||
                                         (v == -1 && (b & 0x40)));
                                if (more)
                                        b |= 0x80;
                                u8(b);
                        } while (more);
                }
        };

        void
        emit_advance(writer &w, taddr from, taddr to, const cie &c,
                     unsigned addr_size)
        {
                uint64_t delta = to - from;
                if (delta == 0)
                        return;
                if (delta % c.code_align == 0) {
                        uint64_t n = delta / c.code_align;
                        if (n < 0x40) {
                                w.u8(CFA_advance_loc | (ubyte)n);
                                return;
                        }
                        if (n <= 0xff) {
                                w.u8(CFA_advance_loc1);
                                w.u8((uint8_t)n);
                                return;
                        }
                        if (n <= 0xffff) {
                                w.u8(CFA_advance_loc2);
                                w.u16((uint16_t)n);
                                return;
                        }
                        if (n <= 0xffffffffULL) {
                                w.u8(CFA_advance_loc4);
                                w.u32((uint32_t)n);
                                return;
                        }
                }
                // The delta is not a multiple of the code alignment
                // factor (or is too large); use an absolute location.
                // Never round the location to make it encodable.
                w.u8(CFA_set_loc);
                w.address(to, addr_size);
        }

        void
        emit_cfa(writer &w, const frame_cfa_rule &cfa, const cie &c)
        {
                if (!cfa.defined)
                        // Cannot arise from parsed CFI: no instruction
                        // makes the CFA undefined again.
                        throw format_error("cannot encode undefined CFA rule");
                if (cfa.offset >= 0) {
                        w.u8(CFA_def_cfa);
                        w.uleb128(cfa.reg);
                        w.uleb128((uint64_t)cfa.offset);
                } else {
                        if (cfa.offset % c.data_align != 0)
                                throw format_error("cannot encode CFA offset");
                        w.u8(CFA_def_cfa_sf);
                        w.uleb128(cfa.reg);
                        w.sleb128(cfa.offset / c.data_align);
                }
        }

        void
        emit_reg_rule(writer &w, uint64_t reg, const frame_reg_rule &rule,
                      const cie &c)
        {
                switch (rule.kind) {
                case frame_reg_kind::undefined:
                        w.u8(CFA_undefined);
                        w.uleb128(reg);
                        break;
                case frame_reg_kind::same_value:
                        w.u8(CFA_same_value);
                        w.uleb128(reg);
                        break;
                case frame_reg_kind::reg:
                        w.u8(CFA_register);
                        w.uleb128(reg);
                        w.uleb128(rule.reg);
                        break;
                case frame_reg_kind::offset:
                case frame_reg_kind::val_offset: {
                        if (rule.offset % c.data_align != 0)
                                throw format_error("cannot encode rule offset");
                        int64_t n = rule.offset / c.data_align;
                        bool sf = n < 0;
                        ubyte plain_op, sf_op;
                        if (rule.kind == frame_reg_kind::offset) {
                                if (!sf && reg < 0x40) {
                                        w.u8(CFA_offset | (ubyte)reg);
                                        w.uleb128((uint64_t)n);
                                        return;
                                }
                                plain_op = CFA_offset_extended;
                                sf_op = CFA_offset_extended_sf;
                        } else {
                                plain_op = CFA_val_offset;
                                sf_op = CFA_val_offset_sf;
                        }
                        w.u8(sf ? sf_op : plain_op);
                        w.uleb128(reg);
                        if (sf)
                                w.sleb128(n);
                        else
                                w.uleb128((uint64_t)n);
                        break;
                }
                }
        }

        // Emit instructions establishing state `to`, given that
        // state `from` is in effect.
        void
        emit_state(writer &w, const rule_state &from, const frame_row &to,
                   const cie &c)
        {
                if (from.cfa != to.cfa)
                        emit_cfa(w, to.cfa, c);
                // Registers changed or removed
                for (auto &kv : to.regs) {
                        auto it = from.regs.find(kv.first);
                        if (it == from.regs.end() || it->second != kv.second)
                                emit_reg_rule(w, kv.first, kv.second, c);
                }
                for (auto &kv : from.regs)
                        if (!to.regs.count(kv.first))
                                emit_reg_rule(w, kv.first, frame_reg_rule(), c);
        }
}

relocated_frame
frame_table::relocate(const std::vector<frame_segment> &segments) const
{
        if (!m)
                throw logic_error("relocate on invalid frame table");

        // Validate the mapping.
        taddr addr_limit = m->addr_size == 4 ? (taddr)0xffffffff : ~0ULL;
        for (auto &s : segments) {
                if (s.length == 0)
                        throw invalid_argument("zero-length frame segment");
                if (s.old_start > ~0ULL - s.length ||
                    s.new_start > ~0ULL - s.length)
                        throw invalid_argument("frame segment address wraps");
                if (s.new_start + s.length - 1 > addr_limit)
                        throw invalid_argument("frame segment target does not "
                                               "fit the address size");
        }
        auto by_old = segments, by_new = segments;
        sort(by_old.begin(), by_old.end(), [](const frame_segment &a,
                                              const frame_segment &b) {
                     return a.old_start < b.old_start; });
        sort(by_new.begin(), by_new.end(), [](const frame_segment &a,
                                              const frame_segment &b) {
                     return a.new_start < b.new_start; });
        for (size_t i = 1; i < by_old.size(); i++)
                if (by_old[i].old_start < by_old[i-1].old_start + by_old[i-1].length)
                        throw invalid_argument("overlapping frame segment sources");
        for (size_t i = 1; i < by_new.size(); i++)
                if (by_new[i].new_start < by_new[i-1].new_start + by_new[i-1].length)
                        throw invalid_argument("overlapping frame segment targets");

        // Locate the FDE of each segment and clip its rows to the
        // segment, shifted to the new addresses.
        struct piece
        {
                const fde *f;
                frame_segment seg;
                std::vector<frame_row> rows;
        };
        std::vector<piece> pieces;
        for (auto &s : segments) {
                taddr old_end = s.old_start + s.length;
                const fde *containing = nullptr;
                for (auto &f : m->fdes) {
                        if (s.old_start >= f.begin && old_end <= f.end) {
                                if (containing)
                                        throw invalid_argument("frame segment "
                                                               "spans multiple FDEs");
                                containing = &f;
                        }
                }
                if (!containing)
                        throw invalid_argument("frame segment not contained in "
                                               "any FDE");
                piece p;
                p.f = containing;
                p.seg = s;
                for (auto &row : m->all_rows) {
                        taddr b = std::max(row.begin, s.old_start);
                        taddr e = std::min(row.end, old_end);
                        if (b >= e)
                                continue;
                        frame_row clipped = row;
                        clipped.begin = s.new_start + (b - s.old_start);
                        clipped.end = s.new_start + (e - s.old_start);
                        p.rows.push_back(std::move(clipped));
                }
                pieces.push_back(std::move(p));
        }

        // Emit a CIE for each distinct source CIE, in order of first
        // use, then one FDE per segment.
        std::vector<char> out;
        std::vector<section_offset> cie_offsets(m->cies.size(), ~(section_offset)0);
        writer w(out);
        for (auto &p : pieces) {
                size_t ci = p.f->cie_index;
                if (cie_offsets[ci] != ~(section_offset)0)
                        continue;
                const cie &c = m->cies[ci];
                cie_offsets[ci] = out.size();
                std::vector<char> body;
                writer bw(body);
                bw.u32(0xffffffff);             // CIE id
                bw.u8(3);                       // version
                bw.u8(0);                       // empty augmentation
                bw.uleb128(c.code_align);
                bw.sleb128(c.data_align);
                bw.uleb128(c.return_address_register);
                // Initial instructions establishing the CIE's rules
                if (c.cfa.defined)
                        emit_cfa(bw, c.cfa, c);
                for (auto &kv : c.regs)
                        emit_reg_rule(bw, kv.first, kv.second, c);
                if (body.size() > 0xffffffffULL - 4)
                        throw format_error("relocated CIE too large");
                w.u32((uint32_t)body.size());
                out.insert(out.end(), body.begin(), body.end());
        }

        std::vector<frame_row> out_rows;
        for (auto &p : pieces) {
                const cie &c = m->cies[p.f->cie_index];
                std::vector<char> body;
                writer bw(body);
                if (cie_offsets[p.f->cie_index] > 0xffffffffULL)
                        throw format_error("relocated CIE offset too large");
                bw.u32((uint32_t)cie_offsets[p.f->cie_index]);
                bw.address(p.seg.new_start, m->addr_size);
                bw.address(p.seg.length, m->addr_size);
                // Reproduce the clipped rows with absolute state
                // transitions.  Each piece starts from the CIE's
                // initial state with an empty remember_state stack.
                rule_state prev;
                prev.cfa = c.cfa;
                prev.regs = c.regs;
                taddr loc = p.seg.new_start;
                for (auto &row : p.rows) {
                        emit_advance(bw, loc, row.begin, c, m->addr_size);
                        emit_state(bw, prev, row, c);
                        prev.cfa = row.cfa;
                        prev.regs = row.regs;
                        loc = row.begin;
                }
                if (body.size() > 0xffffffffULL - 4)
                        throw format_error("relocated FDE too large");
                w.u32((uint32_t)body.size());
                out.insert(out.end(), body.begin(), body.end());
                out_rows.insert(out_rows.end(), p.rows.begin(), p.rows.end());
        }

        return relocated_frame(std::move(out), std::move(out_rows));
}

DWARFPP_END_NAMESPACE
