// Tests for dwarf::frame_table and frame_table::relocate.
//
// All frame data is hand-assembled in C++; no external tools are
// involved.  Relocated output is always read back through
// dwarf::frame_table and compared against the original rules along
// every original row boundary.

#include "dwarf/frame.hh"
#include "dwarf/dwarf++.hh"
#include "elf/elf++.hh"
#include "support/cases.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {

//////////////////////////////////////////////////////////////////
// Byte and record builders
//

// Little-endian byte vector builder.
struct B
{
        std::vector<unsigned char> d;

        size_t size() const { return d.size(); }

        void u8(uint64_t v) { d.push_back((unsigned char)v); }
        void u16(uint64_t v) { u8(v); u8(v >> 8); }
        void u32(uint64_t v) { u16(v); u16(v >> 16); }
        void u64(uint64_t v) { u32(v); u32(v >> 32); }

        void uleb(uint64_t v)
        {
                do {
                        unsigned char c = v & 0x7f;
                        v >>= 7;
                        u8(v ? c | 0x80 : c);
                } while (v);
        }

        void sleb(int64_t v)
        {
                for (;;) {
                        unsigned char c = v & 0x7f;
                        v >>= 7;
                        if ((v == 0 && !(c & 0x40)) ||
                            (v == -1 && (c & 0x40))) {
                                u8(c);
                                return;
                        }
                        u8(c | 0x80);
                }
        }

        void addr(uint64_t v, unsigned addr_size)
        {
                if (addr_size == 4)
                        u32(v);
                else
                        u64(v);
        }

        void to_bytes(const B &o)
        {
                d.insert(d.end(), o.d.begin(), o.d.end());
        }

        void patch32(size_t at, uint32_t v)
        {
                for (unsigned i = 0; i < 4; i++)
                        d[at + i] = (v >> (8 * i)) & 0xff;
        }
};

// Append a CIE record; returns its section offset.
size_t
emit_cie(B &out, uint64_t code_align, int64_t data_align, uint64_t ret_reg,
         const B &init, uint8_t version = 3, const char *aug = "")
{
        size_t at = out.size();
        out.u32(0);                     // length, patched below
        out.u32(0xffffffff);            // CIE id
        out.u8(version);
        for (const char *p = aug; *p; p++)
                out.u8((unsigned char)*p);
        out.u8(0);
        out.uleb(code_align);
        out.sleb(data_align);
        out.uleb(ret_reg);
        out.to_bytes(init);
        out.patch32(at, (uint32_t)(out.size() - at - 4));
        return at;
}

// Append an FDE record; returns its section offset.
size_t
emit_fde(B &out, uint32_t cie_off, uint64_t start, uint64_t range,
         unsigned addr_size, const B &program)
{
        size_t at = out.size();
        out.u32(0);                     // length, patched below
        out.u32(cie_off);
        out.addr(start, addr_size);
        out.addr(range, addr_size);
        out.to_bytes(program);
        out.patch32(at, (uint32_t)(out.size() - at - 4));
        return at;
}

// The initial instructions shared by most tests: CFA = r7 + 8,
// return address register 16 saved at CFA - 8.
B
std_init()
{
        B b;
        b.u8(0x0c); b.uleb(7); b.uleb(8);       // DW_CFA_def_cfa r7, 8
        b.u8(0x90); b.uleb(1);                  // DW_CFA_offset r16, 1
        return b;
}

//////////////////////////////////////////////////////////////////
// Loading
//

struct fixture : dwarf::loader
{
        std::vector<unsigned char> frame_data;
        unsigned char info[11];
        unsigned char abbrev[1];

        explicit fixture(std::vector<unsigned char> f)
                : frame_data(std::move(f))
        {
                // Same minimal CU header as support/baseline.cc.
                unsigned char i[11] = {7, 0, 0, 0, 4, 0, 0, 0, 0, 0, 8};
                memcpy(info, i, sizeof info);
                abbrev[0] = 0;
        }

        const void *load(dwarf::section_type s, size_t *n) override
        {
                if (s == dwarf::section_type::info) {
                        *n = sizeof info;
                        return info;
                }
                if (s == dwarf::section_type::abbrev) {
                        *n = sizeof abbrev;
                        return abbrev;
                }
                if (s == dwarf::section_type::frame) {
                        *n = frame_data.size();
                        return frame_data.data();
                }
                *n = 0;
                return nullptr;
        }
};

dwarf::frame_table
read_table(const B &frame, unsigned addr_size = 8)
{
        auto l = std::make_shared<fixture>(frame.d);
        dwarf::dwarf file(l);
        return dwarf::frame_table(file, addr_size);
}

template <typename F>
void
expect_format_error(F f)
{
        bool threw = false;
        try {
                f();
        } catch (const dwarf::format_error &) {
                threw = true;
        }
        TASK_CHECK(threw);
}

//////////////////////////////////////////////////////////////////
// Checkers
//

void
check_span(const dwarf::frame_row *row, uint64_t begin, uint64_t end)
{
        TASK_CHECK(row != nullptr);
        TASK_CHECK(row->begin == begin);
        TASK_CHECK(row->end == end);
}

void
check_cfa(const dwarf::frame_row *row, uint64_t reg, int64_t offset)
{
        TASK_CHECK(row != nullptr);
        TASK_CHECK(row->cfa.defined);
        TASK_CHECK(row->cfa.reg == reg);
        TASK_CHECK(row->cfa.offset == offset);
}

void
check_reg(const dwarf::frame_row *row, uint64_t reg,
          dwarf::frame_rule_type type, int64_t arg)
{
        TASK_CHECK(row != nullptr);
        auto it = row->registers.find(reg);
        TASK_CHECK(it != row->registers.end());
        TASK_CHECK(it->second.type == type);
        if (type == dwarf::frame_rule_type::offset ||
            type == dwarf::frame_rule_type::val_offset)
                TASK_CHECK(it->second.offset == arg);
        if (type == dwarf::frame_rule_type::reg)
                TASK_CHECK(it->second.reg == (uint64_t)arg);
}

void
check_no_reg(const dwarf::frame_row *row, uint64_t reg)
{
        TASK_CHECK(row != nullptr);
        TASK_CHECK(row->registers.find(reg) == row->registers.end());
}

// The output must contain exactly one FDE per segment, covering
// exactly the moved bytes, with rows tiling each FDE.
void
check_coverage(const dwarf::relocated_frame &rel,
               const std::vector<dwarf::frame_segment> &segs)
{
        TASK_CHECK(!rel.bytes().empty());
        TASK_CHECK(rel.table().fdes().size() == segs.size());
        for (auto &s : segs) {
                const dwarf::frame_table::fde *f =
                        rel.table().find_fde(s.new_start);
                TASK_CHECK(f != nullptr);
                TASK_CHECK(f->initial_location == s.new_start);
                TASK_CHECK(f->address_range == s.length);
                uint64_t at = f->initial_location;
                for (auto &row : f->rows) {
                        TASK_CHECK(row.begin == at);
                        at = row.end;
                }
                TASK_CHECK(at == f->end());
        }
        for (auto &f : rel.table().fdes()) {
                bool matched = false;
                for (auto &s : segs)
                        if (f.initial_location == s.new_start &&
                            f.address_range == s.length)
                                matched = true;
                TASK_CHECK(matched);
        }
}

// For every original row boundary inside the segment, the rules
// just before and just after the boundary must match between the
// old and new locations, and the new row must span exactly the
// clipped original row.
void
check_correspondence(const dwarf::frame_table &orig,
                     const dwarf::frame_table &moved,
                     const dwarf::frame_segment &s)
{
        uint64_t old_end = s.old_start + s.length;
        const dwarf::frame_table::fde *f = orig.find_fde(s.old_start);
        TASK_CHECK(f != nullptr);

        std::vector<uint64_t> addrs;
        addrs.push_back(s.old_start);
        addrs.push_back(old_end - 1);
        for (auto &row : f->rows) {
                if (row.begin > s.old_start && row.begin < old_end) {
                        addrs.push_back(row.begin - 1);
                        addrs.push_back(row.begin);
                }
        }
        for (uint64_t a : addrs) {
                const dwarf::frame_row *orow = orig.find_row(a);
                TASK_CHECK(orow != nullptr);
                uint64_t na = a - s.old_start + s.new_start;
                const dwarf::frame_row *nrow = moved.find_row(na);
                TASK_CHECK(nrow != nullptr);
                TASK_CHECK(nrow->cfa == orow->cfa);
                TASK_CHECK(nrow->registers == orow->registers);
                uint64_t clip_begin = orow->begin > s.old_start ?
                        orow->begin : s.old_start;
                uint64_t clip_end = orow->end < old_end ?
                        orow->end : old_end;
                TASK_CHECK(nrow->begin == clip_begin - s.old_start + s.new_start);
                TASK_CHECK(nrow->end == clip_end - s.old_start + s.new_start);
        }
}

//////////////////////////////////////////////////////////////////
// Cases
//

// A segment may start in the middle of a function, in the middle
// of a row; the first new row inherits the state in effect at the
// cut, including prologue changes to the CFA.
void
case_mid_prologue_cut()
{
        B prog;
        prog.u8(0x0e); prog.uleb(16);           // def_cfa_offset 16
        prog.u8(0x40 | 8);                      // row [0x1000,0x1008)
        prog.u8(0x88); prog.uleb(4);            // offset r8, factor 4
        prog.u8(0x40 | 8);                      // row [0x1008,0x1010)
        prog.u8(0x08); prog.uleb(9);            // same_value r9
        prog.u8(0x40 | 8);                      // row [0x1010,0x1018)
        prog.u8(0x09); prog.uleb(10); prog.uleb(7); // register r10 = r7
        prog.u8(0x40 | 8);                      // row [0x1018,0x1020)
        prog.u8(0x14); prog.uleb(11); prog.uleb(2); // val_offset r11, 2
        prog.u8(0x40 | 8);                      // row [0x1020,0x1028)
        prog.u8(0x07); prog.uleb(8);            // undefined r8
        prog.u8(0x40 | 8);                      // row [0x1028,0x1030)
        prog.u8(0x0c); prog.uleb(6); prog.uleb(24); // def_cfa r6, 24
        prog.u8(0x40 | 8);                      // row [0x1030,0x1038)

        B frame;
        size_t cie = emit_cie(frame, 1, -8, 16, std_init());
        emit_fde(frame, (uint32_t)cie, 0x1000, 0x40, 8, prog);

        // Parse in a scope so the result provably owns its rules.
        dwarf::frame_table orig;
        dwarf::relocated_frame rel;
        std::vector<dwarf::frame_segment> segs;
        segs.emplace_back(0x100c, 0x5000, 0x28);
        {
                orig = read_table(frame);
                TASK_CHECK(orig.fdes().size() == 1);
                const dwarf::frame_table::fde &f = orig.fdes()[0];
                TASK_CHECK(f.initial_location == 0x1000);
                TASK_CHECK(f.address_range == 0x40);
                TASK_CHECK(f.rows.size() == 7);
                check_cfa(&f.initial_state, 7, 8);
                check_reg(&f.initial_state, 16,
                          dwarf::frame_rule_type::offset, -8);
                const dwarf::frame_row *r = orig.find_row(0x1000);
                check_span(r, 0x1000, 0x1008);
                check_cfa(r, 7, 16);
                check_reg(r, 16, dwarf::frame_rule_type::offset, -8);
                check_no_reg(r, 8);
                r = orig.find_row(0x1008);
                check_reg(r, 8, dwarf::frame_rule_type::offset, -32);
                r = orig.find_row(0x1010);
                check_reg(r, 9, dwarf::frame_rule_type::same_value, 0);
                r = orig.find_row(0x1018);
                check_reg(r, 10, dwarf::frame_rule_type::reg, 7);
                r = orig.find_row(0x1020);
                check_reg(r, 11, dwarf::frame_rule_type::val_offset, -16);
                r = orig.find_row(0x1028);
                check_no_reg(r, 8);
                r = orig.find_row(0x1030);
                check_span(r, 0x1030, 0x1040);
                check_cfa(r, 6, 24);
                TASK_CHECK(orig.find_row(0x1040) == nullptr);

                rel = orig.relocate(segs);
        }

        check_coverage(rel, segs);
        check_correspondence(orig, rel.table(), segs[0]);

        TASK_CHECK(rel.table().fdes()[0].rows.size() == 6);
        // The cut at 0x100c lands mid-row; the first new row
        // inherits the CFA change from the middle of the prologue.
        const dwarf::frame_row *r = rel.find_row(0x5000);
        check_span(r, 0x5000, 0x5004);
        check_cfa(r, 7, 16);
        check_reg(r, 16, dwarf::frame_rule_type::offset, -8);
        check_reg(r, 8, dwarf::frame_rule_type::offset, -32);
        check_no_reg(r, 9);
        r = rel.find_row(0x500b);
        check_no_reg(r, 10);
        r = rel.find_row(0x500c);
        check_reg(r, 10, dwarf::frame_rule_type::reg, 7);
        r = rel.find_row(0x5014);
        check_reg(r, 11, dwarf::frame_rule_type::val_offset, -16);
        r = rel.find_row(0x501c);
        check_no_reg(r, 8);
        r = rel.find_row(0x5024);
        check_span(r, 0x5024, 0x5028);
        check_cfa(r, 6, 24);
        TASK_CHECK(rel.find_row(0x5028) == nullptr);
}

// remember_state/restore_state may nest; a cut must not share the
// state stack with the code before it.
void
case_nested_state_save()
{
        B prog;
        prog.u8(0x0a);                          // remember_state
        prog.u8(0x0e); prog.uleb(32);           // def_cfa_offset 32
        prog.u8(0x8c); prog.uleb(2);            // offset r12, factor 2
        prog.u8(0x40 | 8);                      // row [0x2000,0x2008)
        prog.u8(0x0a);                          // remember_state
        prog.u8(0x0e); prog.uleb(48);           // def_cfa_offset 48
        prog.u8(0x08); prog.uleb(13);           // same_value r13
        prog.u8(0x40 | 8);                      // row [0x2008,0x2010)
        prog.u8(0x0b);                          // restore_state
        prog.u8(0x40 | 8);                      // row [0x2010,0x2018)
        prog.u8(0x0b);                          // restore_state
        prog.u8(0x40 | 8);                      // row [0x2018,0x2020)

        B frame;
        size_t cie = emit_cie(frame, 1, -8, 16, std_init());
        emit_fde(frame, (uint32_t)cie, 0x2000, 0x30, 8, prog);

        dwarf::frame_table orig = read_table(frame);
        TASK_CHECK(orig.fdes().size() == 1);
        TASK_CHECK(orig.fdes()[0].rows.size() == 4);
        const dwarf::frame_row *r = orig.find_row(0x2000);
        check_cfa(r, 7, 32);
        check_reg(r, 12, dwarf::frame_rule_type::offset, -16);
        r = orig.find_row(0x2008);
        check_cfa(r, 7, 48);
        check_reg(r, 13, dwarf::frame_rule_type::same_value, 0);
        r = orig.find_row(0x2010);
        check_cfa(r, 7, 32);
        check_no_reg(r, 13);
        check_reg(r, 12, dwarf::frame_rule_type::offset, -16);
        r = orig.find_row(0x2018);
        check_span(r, 0x2018, 0x2030);
        check_cfa(r, 7, 8);
        check_no_reg(r, 12);
        check_reg(r, 16, dwarf::frame_rule_type::offset, -8);

        // The cut starts inside the doubly-saved region.
        std::vector<dwarf::frame_segment> segs;
        segs.emplace_back(0x200c, 0x9000, 0x14);
        dwarf::relocated_frame rel = orig.relocate(segs);

        check_coverage(rel, segs);
        check_correspondence(orig, rel.table(), segs[0]);

        TASK_CHECK(rel.table().fdes()[0].rows.size() == 3);
        r = rel.find_row(0x9000);
        check_span(r, 0x9000, 0x9004);
        check_cfa(r, 7, 48);
        check_reg(r, 12, dwarf::frame_rule_type::offset, -16);
        check_reg(r, 13, dwarf::frame_rule_type::same_value, 0);
        r = rel.find_row(0x9004);
        check_cfa(r, 7, 32);
        check_no_reg(r, 13);
        r = rel.find_row(0x900c);
        check_span(r, 0x900c, 0x9014);
        check_cfa(r, 7, 8);
        check_no_reg(r, 12);
        check_reg(r, 16, dwarf::frame_rule_type::offset, -8);
}

// A restore inside the segment returns to the CIE's initial rules,
// not to the state at the segment's first address.
void
case_restore_after_cut()
{
        B init = std_init();
        init.u8(0x08); init.uleb(14);           // same_value r14

        B prog;
        prog.u8(0x8e); prog.uleb(3);            // offset r14, factor 3
        prog.u8(0x40 | 8);                      // row [0x3000,0x3008)
        prog.u8(0xc0 | 14);                     // restore r14
        prog.u8(0x40 | 8);                      // row [0x3008,0x3010)
        prog.u8(0x07); prog.uleb(16);           // undefined r16
        prog.u8(0x40 | 8);                      // row [0x3010,0x3018)
        prog.u8(0x06); prog.uleb(16);           // restore_extended r16
        prog.u8(0x40 | 8);                      // row [0x3018,0x3020)

        B frame;
        size_t cie = emit_cie(frame, 1, -8, 16, init);
        emit_fde(frame, (uint32_t)cie, 0x3000, 0x20, 8, prog);

        dwarf::frame_table orig = read_table(frame);
        TASK_CHECK(orig.fdes()[0].rows.size() == 4);
        const dwarf::frame_row *r = orig.find_row(0x3000);
        check_reg(r, 14, dwarf::frame_rule_type::offset, -24);
        r = orig.find_row(0x3008);
        check_reg(r, 14, dwarf::frame_rule_type::same_value, 0);
        r = orig.find_row(0x3010);
        check_no_reg(r, 16);
        r = orig.find_row(0x3018);
        check_reg(r, 16, dwarf::frame_rule_type::offset, -8);

        // The cut starts where r14 has its modified rule; the
        // restore at 0x3008 must still produce the CIE's rule.
        std::vector<dwarf::frame_segment> segs;
        segs.emplace_back(0x3004, 0x7000, 0x1c);
        dwarf::relocated_frame rel = orig.relocate(segs);

        check_coverage(rel, segs);
        check_correspondence(orig, rel.table(), segs[0]);

        TASK_CHECK(rel.table().fdes()[0].rows.size() == 4);
        r = rel.find_row(0x7000);
        check_reg(r, 14, dwarf::frame_rule_type::offset, -24);
        r = rel.find_row(0x7004);
        check_reg(r, 14, dwarf::frame_rule_type::same_value, 0);
        r = rel.find_row(0x700c);
        check_no_reg(r, 16);
        r = rel.find_row(0x7014);
        check_reg(r, 16, dwarf::frame_rule_type::offset, -8);
}

// Segments may be reordered: their targets need not follow the
// source order, and several segments may come from one FDE.
void
case_reordered_segments()
{
        B p1;
        p1.u8(0x0e); p1.uleb(16);               // def_cfa_offset 16
        p1.u8(0x40 | 8);                        // row [0x1000,0x1008)
        p1.u8(0x88); p1.uleb(2);                // offset r8, factor 2
        p1.u8(0x40 | 8);                        // row [0x1008,0x1010)

        B p2;
        p2.u8(0x0e); p2.uleb(24);               // def_cfa_offset 24
        p2.u8(0x01); p2.addr(0x2010, 8);        // set_loc 0x2010
        p2.u8(0x08); p2.uleb(9);                // same_value r9
        p2.u8(0x02); p2.u8(0x10);               // advance_loc1 16

        B frame;
        size_t cie = emit_cie(frame, 1, -8, 16, std_init());
        emit_fde(frame, (uint32_t)cie, 0x1000, 0x10, 8, p1);
        emit_fde(frame, (uint32_t)cie, 0x2000, 0x20, 8, p2);

        dwarf::frame_table orig = read_table(frame);
        TASK_CHECK(orig.fdes().size() == 2);

        std::vector<dwarf::frame_segment> segs;
        segs.emplace_back(0x1008, 0x9000, 8);   // second half of FDE1
        segs.emplace_back(0x2000, 0x8000, 0x20); // all of FDE2, lower
        segs.emplace_back(0x1000, 0xa000, 8);   // first half of FDE1
        dwarf::relocated_frame rel = orig.relocate(segs);

        check_coverage(rel, segs);
        for (auto &s : segs)
                check_correspondence(orig, rel.table(), s);

        const dwarf::frame_row *r = rel.find_row(0x8000);
        check_cfa(r, 7, 24);
        r = rel.find_row(0x8010);
        check_reg(r, 9, dwarf::frame_rule_type::same_value, 0);
        r = rel.find_row(0x9000);
        check_cfa(r, 7, 16);
        check_reg(r, 8, dwarf::frame_rule_type::offset, -16);
        r = rel.find_row(0xa000);
        check_cfa(r, 7, 16);
        check_no_reg(r, 8);

        // An empty mapping relocates to an empty section.
        dwarf::relocated_frame rel0 =
                orig.relocate(std::vector<dwarf::frame_segment>());
        TASK_CHECK(rel0.bytes().empty());
        TASK_CHECK(rel0.table().fdes().empty());
}

// New addresses need not be multiples of the code alignment
// factor; rule boundaries must land exactly, never rounded.
void
case_unaligned_target()
{
        B prog;
        prog.u8(0x0e); prog.uleb(16);           // def_cfa_offset 16
        prog.u8(0x40 | 4);                      // advance 4*4 = 16
        prog.u8(0x08); prog.uleb(9);            // same_value r9
        prog.u8(0x02); prog.u8(2);              // advance_loc1 2*4 = 8
        prog.u8(0x0e); prog.uleb(24);           // def_cfa_offset 24
        prog.u8(0x04); prog.u32(6);             // advance_loc4 6*4 = 24

        B frame;
        size_t cie = emit_cie(frame, 4, -8, 16, std_init());
        emit_fde(frame, (uint32_t)cie, 0x4000, 0x40, 8, prog);

        dwarf::frame_table orig = read_table(frame);
        TASK_CHECK(orig.fdes()[0].rows.size() == 3);
        const dwarf::frame_row *r = orig.find_row(0x4000);
        check_span(r, 0x4000, 0x4010);
        check_cfa(r, 7, 16);
        r = orig.find_row(0x4010);
        check_span(r, 0x4010, 0x4018);
        check_reg(r, 9, dwarf::frame_rule_type::same_value, 0);
        r = orig.find_row(0x4018);
        check_span(r, 0x4018, 0x4040);
        check_cfa(r, 7, 24);

        // 0x6003 is not a multiple of the code alignment factor.
        std::vector<dwarf::frame_segment> segs;
        segs.emplace_back(0x4004, 0x6003, 0x2c);
        dwarf::relocated_frame rel = orig.relocate(segs);

        check_coverage(rel, segs);
        check_correspondence(orig, rel.table(), segs[0]);

        TASK_CHECK(rel.table().fdes()[0].initial_location == 0x6003);
        TASK_CHECK(rel.table().fdes()[0].rows.size() == 3);
        r = rel.find_row(0x6003);
        check_span(r, 0x6003, 0x600f);
        check_cfa(r, 7, 16);
        r = rel.find_row(0x600f);
        check_span(r, 0x600f, 0x6017);
        check_reg(r, 9, dwarf::frame_rule_type::same_value, 0);
        r = rel.find_row(0x6017);
        check_span(r, 0x6017, 0x602f);
        check_cfa(r, 7, 24);
}

// .debug_frame can also be read through the ELF loader.  The ELF
// file is assembled in memory; no external tools are used.
void
case_elf_loader_roundtrip()
{
        B prog;
        prog.u8(0x0e); prog.uleb(16);           // def_cfa_offset 16
        prog.u8(0x40 | 8);                      // row [0x1000,0x1008)
        prog.u8(0x88); prog.uleb(2);            // offset r8, factor 2
        prog.u8(0x40 | 8);                      // row [0x1008,0x1010)

        B frame;
        size_t cie = emit_cie(frame, 1, -8, 16, std_init());
        emit_fde(frame, (uint32_t)cie, 0x1000, 0x10, 8, prog);

        // Minimal ELF64 relocatable with .debug_info, .debug_abbrev,
        // .debug_frame and .shstrtab.
        B info;
        info.d = {7, 0, 0, 0, 4, 0, 0, 0, 0, 0, 8};
        B abbrev;
        abbrev.u8(0);
        B strtab;
        strtab.u8(0);
        auto intern = [&strtab](const char *s) -> uint32_t {
                uint32_t at = (uint32_t)strtab.size();
                for (const char *p = s; *p; p++)
                        strtab.u8((unsigned char)*p);
                strtab.u8(0);
                return at;
        };
        uint32_t n_info = intern(".debug_info");
        uint32_t n_abbrev = intern(".debug_abbrev");
        uint32_t n_frame = intern(".debug_frame");
        uint32_t n_strtab = intern(".shstrtab");

        B e;
        e.u8(0x7f); e.u8('E'); e.u8('L'); e.u8('F');
        e.u8(2); e.u8(1); e.u8(1); e.u8(0);     // 64-bit, LSB, v1
        e.u64(0);                               // ABI + padding
        e.u16(1);                               // ET_REL
        e.u16(62);                              // EM_X86_64
        e.u32(1);                               // version
        e.u64(0);                               // entry
        e.u64(0);                               // phoff
        size_t shoff_at = e.size();
        e.u64(0);                               // shoff, patched
        e.u32(0);                               // flags
        e.u16(64);                              // ehsize
        e.u16(0); e.u16(0);                     // phentsize, phnum
        e.u16(64);                              // shentsize
        e.u16(5);                               // shnum
        e.u16(4);                               // shstrndx

        uint64_t info_off = e.size();
        e.to_bytes(info);
        uint64_t abbrev_off = e.size();
        e.to_bytes(abbrev);
        uint64_t frame_off = e.size();
        e.to_bytes(frame);
        uint64_t strtab_off = e.size();
        e.to_bytes(strtab);
        while (e.size() % 8)
                e.u8(0);
        uint64_t shoff = e.size();
        e.patch32(shoff_at, (uint32_t)shoff);
        e.patch32(shoff_at + 4, 0);

        auto shdr = [&e](uint32_t name, uint32_t type, uint64_t off,
                         uint64_t sz) {
                e.u32(name); e.u32(type);
                e.u64(0); e.u64(0);
                e.u64(off); e.u64(sz);
                e.u32(0); e.u32(0);
                e.u64(1); e.u64(0);
        };
        for (int i = 0; i < 8; i++)
                e.u64(0);                       // null section
        shdr(n_info, 1, info_off, info.size());
        shdr(n_abbrev, 1, abbrev_off, abbrev.size());
        shdr(n_frame, 1, frame_off, frame.size());
        shdr(n_strtab, 3, strtab_off, strtab.size());

        struct mem_loader : elf::loader
        {
                std::vector<unsigned char> file;
                const void *load(off_t offset, size_t size) override
                {
                        if (offset < 0 ||
                            (uint64_t)offset + size > file.size())
                                throw std::runtime_error("elf loader eof");
                        return file.data() + offset;
                }
        };
        auto ml = std::make_shared<mem_loader>();
        ml->file = e.d;
        elf::elf ef(ml);
        dwarf::dwarf dw(dwarf::elf::create_loader(ef));
        dwarf::frame_table orig(dw, 8);

        TASK_CHECK(orig.fdes().size() == 1);
        const dwarf::frame_row *r = orig.find_row(0x1008);
        check_cfa(r, 7, 16);
        check_reg(r, 8, dwarf::frame_rule_type::offset, -16);

        std::vector<dwarf::frame_segment> segs;
        segs.emplace_back(0x1000, 0x4000, 0x10);
        dwarf::relocated_frame rel = orig.relocate(segs);
        check_coverage(rel, segs);
        check_correspondence(orig, rel.table(), segs[0]);
}

// 32-bit addresses work, and targets that do not fit the address
// size are rejected.
void
case_address_size_4()
{
        B prog;
        prog.u8(0x0e); prog.uleb(16);           // def_cfa_offset 16
        prog.u8(0x40 | 8);                      // row [+0,+8)

        B frame;
        size_t cie = emit_cie(frame, 1, -8, 16, std_init());
        emit_fde(frame, (uint32_t)cie, 0x80001000, 0x20, 4, prog);

        dwarf::frame_table orig = read_table(frame, 4);
        TASK_CHECK(orig.fdes().size() == 1);
        check_cfa(orig.find_row(0x80001000), 7, 16);

        std::vector<dwarf::frame_segment> segs;
        segs.emplace_back(0x80001000, 0x80002000, 0x20);
        dwarf::relocated_frame rel = orig.relocate(segs);
        check_coverage(rel, segs);
        check_correspondence(orig, rel.table(), segs[0]);
        TASK_CHECK(rel.table().address_size() == 4);

        // A target above 4GB does not fit the address size.
        expect_format_error([&] {
                orig.relocate({{0x80001000, 0x100000000, 8}});
        });
}

// Malformed inputs must throw format_error, not return partial
// results.
void
case_malformed_inputs()
{
        B init = std_init();

        // Record length runs past the end of the section
        {
                B b;
                b.u32(100); b.u32(0xffffffff); b.u8(3);
                expect_format_error([&] { read_table(b); });
        }
        // Zero-length and reserved record lengths
        {
                B b;
                b.u32(0);
                expect_format_error([&] { read_table(b); });
        }
        {
                B b;
                b.u32(0xfffffff0);
                expect_format_error([&] { read_table(b); });
        }
        // DWARF64 records are not supported
        {
                B b;
                b.u32(0xffffffff); b.u64(16);
                expect_format_error([&] { read_table(b); });
        }
        // Trailing garbage after the last record
        {
                B frame;
                emit_cie(frame, 1, -8, 16, init);
                frame.u8(0); frame.u8(0);
                expect_format_error([&] { read_table(frame); });
        }
        // Instruction stream truncated mid-operand
        {
                B prog;
                prog.u8(0x0c); prog.uleb(7);    // def_cfa, missing offset
                B frame;
                size_t cie = emit_cie(frame, 1, -8, 16, init);
                emit_fde(frame, (uint32_t)cie, 0x1000, 0x10, 8, prog);
                expect_format_error([&] { read_table(frame); });
        }
        // CIE reference past the end of the section
        {
                B prog;
                B frame;
                emit_cie(frame, 1, -8, 16, init);
                emit_fde(frame, 0x1234, 0x1000, 0x10, 8, prog);
                expect_format_error([&] { read_table(frame); });
        }
        // CIE reference that names an FDE
        {
                B prog;
                B frame;
                size_t cie = emit_cie(frame, 1, -8, 16, init);
                size_t fde1 = emit_fde(frame, (uint32_t)cie, 0x1000,
                                       0x10, 8, prog);
                emit_fde(frame, (uint32_t)fde1, 0x2000, 0x10, 8, prog);
                expect_format_error([&] { read_table(frame); });
        }
        // Unsupported CIE versions
        {
                B frame;
                emit_cie(frame, 1, -8, 16, init, 4);
                expect_format_error([&] { read_table(frame); });
        }
        {
                B frame;
                emit_cie(frame, 1, -8, 16, init, 1);
                expect_format_error([&] { read_table(frame); });
        }
        // Non-empty augmentation
        {
                B frame;
                emit_cie(frame, 1, -8, 16, init, 3, "z");
                expect_format_error([&] { read_table(frame); });
        }
        // set_loc moving backward
        {
                B prog;
                prog.u8(0x01); prog.addr(0x1008, 8);
                prog.u8(0x01); prog.addr(0x1004, 8);
                B frame;
                size_t cie = emit_cie(frame, 1, -8, 16, init);
                emit_fde(frame, (uint32_t)cie, 0x1000, 0x20, 8, prog);
                expect_format_error([&] { read_table(frame); });
        }
        // set_loc past the end of the FDE
        {
                B prog;
                prog.u8(0x01); prog.addr(0x2000, 8);
                B frame;
                size_t cie = emit_cie(frame, 1, -8, 16, init);
                emit_fde(frame, (uint32_t)cie, 0x1000, 0x20, 8, prog);
                expect_format_error([&] { read_table(frame); });
        }
        // Advance past the end of the FDE
        {
                B prog;
                prog.u8(0x40 | 0x20);
                B frame;
                size_t cie = emit_cie(frame, 1, -8, 16, init);
                emit_fde(frame, (uint32_t)cie, 0x1000, 0x10, 8, prog);
                expect_format_error([&] { read_table(frame); });
        }
        // State stack underflow
        {
                B prog;
                prog.u8(0x0b);
                B frame;
                size_t cie = emit_cie(frame, 1, -8, 16, init);
                emit_fde(frame, (uint32_t)cie, 0x1000, 0x10, 8, prog);
                expect_format_error([&] { read_table(frame); });
        }
        // Expression rules are rejected
        for (uint8_t op : {0x0f, 0x10, 0x16}) {
                B prog;
                prog.u8(op);
                B frame;
                size_t cie = emit_cie(frame, 1, -8, 16, init);
                emit_fde(frame, (uint32_t)cie, 0x1000, 0x10, 8, prog);
                expect_format_error([&] { read_table(frame); });
        }
        // Unknown opcode
        {
                B prog;
                prog.u8(0x3f);
                B frame;
                size_t cie = emit_cie(frame, 1, -8, 16, init);
                emit_fde(frame, (uint32_t)cie, 0x1000, 0x10, 8, prog);
                expect_format_error([&] { read_table(frame); });
        }

        // A valid table with two adjacent FDEs for relocate errors
        B prog1;
        prog1.u8(0x0e); prog1.uleb(16);
        prog1.u8(0x40 | 8);
        B frame;
        size_t cie = emit_cie(frame, 1, -8, 16, init);
        emit_fde(frame, (uint32_t)cie, 0x1000, 0x10, 8, prog1);
        emit_fde(frame, (uint32_t)cie, 0x1010, 0x10, 8, prog1);
        dwarf::frame_table t = read_table(frame);
        TASK_CHECK(t.fdes().size() == 2);

        // Overlapping source ranges
        expect_format_error([&] {
                t.relocate({{0x1000, 0x5000, 8}, {0x1004, 0x6000, 8}});
        });
        // Overlapping target ranges
        expect_format_error([&] {
                t.relocate({{0x1000, 0x5000, 8}, {0x1008, 0x5004, 8}});
        });
        // Segment outside every FDE
        expect_format_error([&] {
                t.relocate({{0x9000, 0x5000, 8}});
        });
        // Segment spanning two FDEs
        expect_format_error([&] {
                t.relocate({{0x1008, 0x5000, 0x10}});
        });
        // Address arithmetic wrap-around
        expect_format_error([&] {
                t.relocate({{UINT64_MAX - 4, 0x5000, 8}});
        });
        expect_format_error([&] {
                t.relocate({{0x1000, UINT64_MAX - 4, 8}});
        });
        // Empty segment
        expect_format_error([&] {
                t.relocate({{0x1000, 0x5000, 0}});
        });
}

} // namespace

void
register_cases(std::vector<OfflineCase> &cases)
{
        cases.push_back({"mid_prologue_cut", &case_mid_prologue_cut});
        cases.push_back({"nested_state_save", &case_nested_state_save});
        cases.push_back({"restore_after_cut", &case_restore_after_cut});
        cases.push_back({"reordered_segments", &case_reordered_segments});
        cases.push_back({"unaligned_target", &case_unaligned_target});
        cases.push_back({"elf_loader_roundtrip", &case_elf_loader_roundtrip});
        cases.push_back({"address_size_4", &case_address_size_4});
        cases.push_back({"malformed_inputs", &case_malformed_inputs});
}
