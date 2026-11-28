// Tests for dwarf/frame.hh: parsing .debug_frame and relocating call
// frame information for moved code segments.  All fixtures are
// hand-built in C++; no external tools are involved.

#include "dwarf++.hh"
#include "frame.hh"
#include "elf++.hh"

#include "../support/cases.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

using dwarf::taddr;
using dwarf::frame_cfa_rule;
using dwarf::frame_reg_rule;
using dwarf::frame_row;
using dwarf::frame_segment;

//////////////////////////////////////////////////////////////////
// Byte builder
//

namespace
{
        struct fbuilder
        {
                std::vector<char> b;

                void u8(uint8_t v) { b.push_back((char)v); }
                void u16(uint16_t v) { u8(v & 0xff); u8((v >> 8) & 0xff); }
                void u32(uint32_t v)
                {
                        for (unsigned i = 0; i < 4; i++)
                                u8((v >> (8 * i)) & 0xff);
                }
                void u64(uint64_t v)
                {
                        for (unsigned i = 0; i < 8; i++)
                                u8((v >> (8 * i)) & 0xff);
                }
                void addr(uint64_t v, unsigned as)
                {
                        if (as == 4) u32((uint32_t)v); else u64(v);
                }
                void uleb(uint64_t v)
                {
                        do {
                                uint8_t x = v & 0x7f;
                                v >>= 7;
                                if (v) x |= 0x80;
                                u8(x);
                        } while (v);
                }
                void sleb(int64_t v)
                {
                        bool more;
                        do {
                                uint8_t x = v & 0x7f;
                                v >>= 7;
                                more = !((v == 0 && !(x & 0x40)) ||
                                         (v == -1 && (x & 0x40)));
                                if (more) x |= 0x80;
                                u8(x);
                        } while (more);
                }
                void bytes(std::initializer_list<uint8_t> l)
                {
                        for (auto x : l) u8(x);
                }
                void entry(const fbuilder &body)
                {
                        u32((uint32_t)body.b.size());
                        b.insert(b.end(), body.b.begin(), body.b.end());
                }
        };

        // CIE with version 3 and empty augmentation
        fbuilder cie3(uint64_t code_align, int64_t data_align, uint64_t ret_reg,
                      std::initializer_list<uint8_t> init)
        {
                fbuilder c;
                c.u32(0xffffffff);      // CIE id
                c.u8(3);                // version
                c.u8(0);                // empty augmentation
                c.uleb(code_align);
                c.sleb(data_align);
                c.uleb(ret_reg);
                c.bytes(init);
                return c;
        }

        fbuilder fde(uint32_t cie_off, uint64_t init_loc, uint64_t range,
                     unsigned addr_size, std::initializer_list<uint8_t> prog)
        {
                fbuilder f;
                f.u32(cie_off);
                f.addr(init_loc, addr_size);
                f.addr(range, addr_size);
                f.bytes(prog);
                return f;
        }
}

//////////////////////////////////////////////////////////////////
// Loaders
//

namespace
{
        struct vec_loader : dwarf::loader
        {
                std::vector<char> info, abbrev, frame;

                const void *load(dwarf::section_type s, size_t *n) override
                {
                        if (s == dwarf::section_type::info) {
                                *n = info.size();
                                return info.data();
                        }
                        if (s == dwarf::section_type::abbrev) {
                                *n = abbrev.size();
                                return abbrev.data();
                        }
                        if (s == dwarf::section_type::frame) {
                                *n = frame.size();
                                return frame.data();
                        }
                        *n = 0;
                        return nullptr;
                }
        };

        // A dwarf::dwarf whose .debug_frame is the given bytes (with a
        // minimal dummy CU, since libdwarf++ requires .debug_info).
        dwarf::dwarf make_dwarf(const std::vector<char> &frame)
        {
                auto l = std::make_shared<vec_loader>();
                l->info = {7, 0, 0, 0, 4, 0, 0, 0, 0, 0, 8};
                l->abbrev = {0};
                l->frame = frame;
                return dwarf::dwarf(l);
        }
}

//////////////////////////////////////////////////////////////////
// Fixtures
//

// .debug_frame with two CIEs.  CIE A (offset 0): code_align 4,
// data_align -8, return register 16.  FDE A covers [0x1000, 0x1040)
// and exercises prologue changes, nested remember/restore_state,
// restore, and set_loc.  CIE B: code_align 1, data_align -4, ret 8.
// FDE B covers [0x2000, 0x2010).  Addresses are 4 bytes.
static std::vector<char>
fixture_ab_full()
{
        fbuilder f;
        fbuilder cieA = cie3(4, -8, 16, {
                0x0c, 0x07, 0x08, 0x90, 0x01,
        });
        f.entry(cieA);
        f.entry(fde(0, 0x1000, 0x40, 4, {
                0x00,
                0x41, 0x0e, 0x10, 0x88, 0x02, 0x0a,
                0x41, 0x0d, 0x06, 0x08, 0x03, 0x07, 0x09,
                0x09, 0x04, 0x02, 0x14, 0x05, 0x03, 0x0a,
                0x41, 0x0b, 0x0e, 0x20,
                0x41, 0xc8, 0x05, 0x14, 0x04,
                0x01, 0x20, 0x10, 0x00, 0x00,
                0x15, 0x15, 0x7e, 0x11, 0x16, 0x7f,
                0x12, 0x06, 0x7d,
                0x03, 0x04, 0x00,
                0x0b,
                0x04, 0x04, 0x00, 0x00, 0x00,
        }));
        fbuilder cieB = cie3(1, -4, 8, {
                0x0c, 0x07, 0x10, 0x08, 0x08,
        });
        uint32_t cieB_off = (uint32_t)f.b.size();
        f.entry(cieB);
        f.entry(fde(cieB_off, 0x2000, 0x10, 4, {
                0x0e, 0x20,             // def_cfa_offset 32
                0x8c, 0x02,             // offset r12, 2 -> -8
                0x02, 0x08,             // advance_loc1 8 -> 0x2008
                0x11, 0x0d, 0x03,       // offset_extended_sf r13, 3 -> -12
                0x02, 0x08,             // advance_loc1 8 -> 0x2010
        }));
        return f.b;
}

// Expected rows of fixture_ab_full().
static std::vector<frame_row>
fixture_ab_rows()
{
        std::vector<frame_row> rows;
        auto add = [&](taddr b, taddr e, uint64_t cfa_reg, int64_t cfa_off,
                       std::map<uint64_t, frame_reg_rule> regs) {
                frame_row r;
                r.begin = b; r.end = e;
                r.cfa = frame_cfa_rule(cfa_reg, cfa_off);
                r.regs = std::move(regs);
                rows.push_back(std::move(r));
        };
        auto off = &frame_reg_rule::make_offset;
        auto voff = &frame_reg_rule::make_val_offset;
        auto same = &frame_reg_rule::make_same_value;
        auto reg = &frame_reg_rule::make_reg;
        add(0x1000, 0x1004, 7, 8,
            {{16, off(-8)}});
        add(0x1004, 0x1008, 7, 16,
            {{8, off(-16)}, {16, off(-8)}});
        add(0x1008, 0x100c, 6, 16,
            {{3, same()}, {4, reg(2)}, {5, voff(-24)}, {8, off(-16)}, {16, off(-8)}});
        add(0x100c, 0x1010, 6, 32,
            {{3, same()}, {4, reg(2)}, {5, voff(-24)}, {8, off(-16)}, {16, off(-8)}});
        add(0x1010, 0x1020, 6, 32,
            {{3, same()}, {4, reg(2)}, {5, voff(-24)}, {16, off(-8)}, {20, off(-32)}});
        add(0x1020, 0x1030, 6, 24,
            {{3, same()}, {4, reg(2)}, {5, voff(-24)}, {16, off(-8)}, {20, off(-32)},
             {21, voff(16)}, {22, off(8)}});
        add(0x1030, 0x1040, 7, 16,
            {{8, off(-16)}, {16, off(-8)}});
        add(0x2000, 0x2008, 7, 32,
            {{8, same()}, {12, off(-8)}});
        add(0x2008, 0x2010, 7, 32,
            {{8, same()}, {12, off(-8)}, {13, off(-12)}});
        return rows;
}

// .debug_frame with 8-byte addresses.  CIE D: code_align 2,
// data_align -8, ret 30.  FDE D covers [0x100000000, 0x100000020) and
// uses the _sf forms, remember/restore_state, and an unaligned
// set_loc.
static std::vector<char>
fixture_d()
{
        fbuilder f;
        fbuilder cieD = cie3(2, -8, 30, {
                0x0c, 0x07, 0x08,       // def_cfa 7, 8
                0x9e, 0x01,             // offset r30, 1 -> -8
        });
        f.entry(cieD);
        f.entry(fde(0, 0x100000000ULL, 0x20, 8, {
                0x42,                   // advance_loc 2 -> +4
                0x12, 0x07, 0x7e,       // def_cfa_sf 7, -2 -> 16
                0x11, 0x11, 0x7d,       // offset_extended_sf r17, -3 -> 24
                0x15, 0x12, 0x02,       // val_offset_sf r18, 2 -> -16
                0x04, 0x06, 0x00, 0x00, 0x00, // advance_loc4 6 -> +12
                0x0a,                   // remember_state
                0x0e, 0x18,             // def_cfa_offset 24
                0x42,                   // advance_loc 2 -> +4
                0x0b,                   // restore_state
                0x01, 0x19, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
                                        // set_loc 0x100000019 (unaligned)
                0x09, 0x02, 0x03,       // register r2 -> r3
                0x43,                   // advance_loc 3 -> +6
        }));
        return f.b;
}

// Expected rows of fixture_d().
static std::vector<frame_row>
fixture_d_rows()
{
        std::vector<frame_row> rows;
        auto add = [&](taddr b, taddr e, uint64_t cfa_reg, int64_t cfa_off,
                       std::map<uint64_t, frame_reg_rule> regs) {
                frame_row r;
                r.begin = b; r.end = e;
                r.cfa = frame_cfa_rule(cfa_reg, cfa_off);
                r.regs = std::move(regs);
                rows.push_back(std::move(r));
        };
        auto off = &frame_reg_rule::make_offset;
        auto voff = &frame_reg_rule::make_val_offset;
        add(0x100000000ULL, 0x100000004ULL, 7, 8,
            {{30, off(-8)}});
        add(0x100000004ULL, 0x100000010ULL, 7, 16,
            {{30, off(-8)}, {17, off(24)}, {18, voff(-16)}});
        add(0x100000010ULL, 0x100000014ULL, 7, 24,
            {{30, off(-8)}, {17, off(24)}, {18, voff(-16)}});
        add(0x100000014ULL, 0x100000019ULL, 7, 16,
            {{30, off(-8)}, {17, off(24)}, {18, voff(-16)}});
        add(0x100000019ULL, 0x100000020ULL, 7, 16,
            {{30, off(-8)}, {17, off(24)}, {18, voff(-16)}, {2, frame_reg_rule::make_reg(3)}});
        return rows;
}

//////////////////////////////////////////////////////////////////
// Checking helpers
//

static void
expect_row_at(const dwarf::frame_table &ft, taddr addr,
              uint64_t cfa_reg, int64_t cfa_off,
              std::map<uint64_t, frame_reg_rule> regs)
{
        const frame_row *r = ft.find_row(addr);
        TASK_CHECK(r != nullptr);
        TASK_CHECK(r->cfa.defined && r->cfa.reg == cfa_reg && r->cfa.offset == cfa_off);
        TASK_CHECK(r->regs == regs);
}

// Re-parse relocated output and check that every mapped address has
// the same rules as the corresponding original address, that row
// boundaries map exactly, and that nothing outside the segments was
// emitted.
static void
check_relocation(const dwarf::frame_table &orig,
                 const dwarf::relocated_frame &rel,
                 const std::vector<frame_segment> &segs,
                 unsigned addr_size)
{
        auto dw = make_dwarf(rel.bytes());
        dwarf::frame_table out(dw, addr_size);

        // The rows reported by relocate must match what the emitted
        // bytes decode to.
        TASK_CHECK(out.rows() == rel.rows());

        // Every output row must lie entirely within one segment's new
        // range (nothing extra emitted).
        for (auto &r : out.rows()) {
                bool inside = false;
                for (auto &s : segs)
                        if (r.begin >= s.new_start &&
                            r.end <= s.new_start + s.length)
                                inside = true;
                TASK_CHECK(inside);
        }

        for (auto &s : segs) {
                taddr old_end = s.old_start + s.length;
                for (auto &r : orig.rows()) {
                        taddr b = std::max(r.begin, s.old_start);
                        taddr e = std::min(r.end, old_end);
                        if (b >= e)
                                continue;
                        // Check both edges of the clipped row.
                        for (taddr a : {b, e - 1}) {
                                taddr na = s.new_start + (a - s.old_start);
                                const frame_row *nr = out.find_row(na);
                                TASK_CHECK(nr != nullptr);
                                TASK_CHECK(nr->same_state(r));
                        }
                        // Row boundaries strictly inside a segment
                        // must be reproduced exactly (no rounding).
                        for (taddr bnd : {r.begin, r.end}) {
                                if (bnd <= s.old_start || bnd >= old_end)
                                        continue;
                                taddr nb = s.new_start + (bnd - s.old_start);
                                const frame_row *before = out.find_row(nb - 1);
                                const frame_row *after = out.find_row(nb);
                                TASK_CHECK(before != nullptr && after != nullptr);
                                TASK_CHECK(before->end == nb && after->begin == nb);
                        }
                }
        }
}

static void
expect_format_error(const std::vector<char> &frame, unsigned addr_size)
{
        bool caught = false;
        std::string msg;
        try {
                auto dw = make_dwarf(frame);
                dwarf::frame_table ft(dw, addr_size);
        } catch (const dwarf::format_error &e) {
                caught = true;
                msg = e.what();
        }
        TASK_CHECK(caught);
        // Errors must carry a section offset
        TASK_CHECK(msg.find(".debug_frame+0x") != std::string::npos);
}

static void
expect_invalid_mapping(const dwarf::frame_table &ft,
                       std::initializer_list<frame_segment> segs)
{
        bool caught = false;
        try {
                ft.relocate(std::vector<frame_segment>(segs));
        } catch (const std::invalid_argument &) {
                caught = true;
        }
        TASK_CHECK(caught);
}

static uint32_t
rd32(const std::vector<char> &b, size_t off)
{
        return (uint32_t)(uint8_t)b[off] |
                ((uint32_t)(uint8_t)b[off+1] << 8) |
                ((uint32_t)(uint8_t)b[off+2] << 16) |
                ((uint32_t)(uint8_t)b[off+3] << 24);
}

// Count CIEs and FDEs in a .debug_frame image.
static void
count_entries(const std::vector<char> &bytes, unsigned &cies, unsigned &fdes)
{
        cies = fdes = 0;
        size_t pos = 0;
        while (pos < bytes.size()) {
                uint32_t len = rd32(bytes, pos);
                TASK_CHECK(len != 0 && pos + 4 + len <= bytes.size());
                if (rd32(bytes, pos + 4) == 0xffffffff)
                        cies++;
                else
                        fdes++;
                pos += 4 + len;
        }
        TASK_CHECK(pos == bytes.size());
}

//////////////////////////////////////////////////////////////////
// Cases
//

// Mid-function cut into the middle of an FDE, landing on an unaligned
// new address.  The segment starts in the middle of the FDE's second
// CIE, so the first output row must inherit the state in effect at
// the cut point.
static void
case_mid_cut_unaligned()
{
        auto dw = make_dwarf(fixture_ab_full());
        dwarf::frame_table ft(dw, 4);
        TASK_CHECK(ft.valid());

        // The full original table must decode to exactly these rows.
        TASK_CHECK(ft.rows() == fixture_ab_rows());

        std::vector<frame_segment> segs = {
                {0x1008, 0x5003, 5},
        };
        auto rel = ft.relocate(segs);
        check_relocation(ft, rel, segs, 4);

        // Re-read the output and check the exact rows.
        auto dw2 = make_dwarf(rel.bytes());
        dwarf::frame_table out(dw2, 4);
        unsigned cies, fdes;
        count_entries(rel.bytes(), cies, fdes);
        TASK_CHECK(cies == 1 && fdes == 1);
        // The segment starts in the middle of the FDE's address range,
        // so the first row must reproduce the state in effect at the
        // cut point (cfa reg 6, not the CIE's cfa reg 7).
        expect_row_at(out, 0x5003, 6, 16,
                      {{3, frame_reg_rule::make_same_value()},
                       {4, frame_reg_rule::make_reg(2)},
                       {5, frame_reg_rule::make_val_offset(-24)},
                       {8, frame_reg_rule::make_offset(-16)},
                       {16, frame_reg_rule::make_offset(-8)}});
        expect_row_at(out, 0x5007, 6, 32,
                      {{3, frame_reg_rule::make_same_value()},
                       {4, frame_reg_rule::make_reg(2)},
                       {5, frame_reg_rule::make_val_offset(-24)},
                       {8, frame_reg_rule::make_offset(-16)},
                       {16, frame_reg_rule::make_offset(-8)}});
        // Only the moved bytes are covered.
        TASK_CHECK(out.find_row(0x5002) == nullptr);
        TASK_CHECK(out.find_row(0x5008) == nullptr);
}

// A segment whose interior contains a restore_state and a restore;
// the restore must go back to the CIE's initial rules, not to the
// state at the segment start.
static void
case_nested_state_restore_after_cut()
{
        auto dw = make_dwarf(fixture_ab_full());
        dwarf::frame_table ft(dw, 4);

        std::vector<frame_segment> segs = {
                {0x100c, 0x6000, 8},
        };
        auto rel = ft.relocate(segs);
        check_relocation(ft, rel, segs, 4);

        auto dw2 = make_dwarf(rel.bytes());
        dwarf::frame_table out(dw2, 4);
        // At the segment start the state is the one in effect at
        // 0x100c in the original: cfa(6,32) with r8 still defined.
        expect_row_at(out, 0x6000, 6, 32,
                      {{3, frame_reg_rule::make_same_value()},
                       {4, frame_reg_rule::make_reg(2)},
                       {5, frame_reg_rule::make_val_offset(-24)},
                       {8, frame_reg_rule::make_offset(-16)},
                       {16, frame_reg_rule::make_offset(-8)}});
        // After the restore at 0x1010 (mapped to 0x6004), r8 must go
        // back to the CIE's initial rule for r8 (undefined), not to
        // the segment-start rule.
        expect_row_at(out, 0x6004, 6, 32,
                      {{3, frame_reg_rule::make_same_value()},
                       {4, frame_reg_rule::make_reg(2)},
                       {5, frame_reg_rule::make_val_offset(-24)},
                       {16, frame_reg_rule::make_offset(-8)},
                       {20, frame_reg_rule::make_offset(-32)}});
        TASK_CHECK(out.find_row(0x6007)->regs.count(8) == 0);
}

// Multiple segments reordered: a later FDE's bytes placed at a lower
// address, segments from two different CIEs interleaved.
static void
case_reorder_segments()
{
        auto dw = make_dwarf(fixture_ab_full());
        dwarf::frame_table ft(dw, 4);

        std::vector<frame_segment> segs = {
                {0x1010, 0x7000, 8},    // FDE A tail -> highest
                {0x1000, 0x6000, 8},    // FDE A head -> middle
                {0x2004, 0x5000, 8},    // FDE B middle -> lowest
        };
        auto rel = ft.relocate(segs);
        check_relocation(ft, rel, segs, 4);

        auto dw2 = make_dwarf(rel.bytes());
        dwarf::frame_table out(dw2, 4);
        unsigned cies, fdes;
        count_entries(rel.bytes(), cies, fdes);
        TASK_CHECK(cies == 2 && fdes == 3);

        // Segment 1 (from CIE A): the cut inherits the state after
        // restore r8, so r8 is undefined here even though the segment
        // starts mid-function.
        expect_row_at(out, 0x7000, 6, 32,
                      {{3, frame_reg_rule::make_same_value()},
                       {4, frame_reg_rule::make_reg(2)},
                       {5, frame_reg_rule::make_val_offset(-24)},
                       {16, frame_reg_rule::make_offset(-8)},
                       {20, frame_reg_rule::make_offset(-32)}});
        // Segment 2 starts at the FDE's beginning: CIE initial state.
        expect_row_at(out, 0x6000, 7, 8,
                      {{16, frame_reg_rule::make_offset(-8)}});
        expect_row_at(out, 0x6004, 7, 16,
                      {{8, frame_reg_rule::make_offset(-16)},
                       {16, frame_reg_rule::make_offset(-8)}});
        // Segment 3 comes from CIE B's FDE.
        expect_row_at(out, 0x5000, 7, 32,
                      {{8, frame_reg_rule::make_same_value()},
                       {12, frame_reg_rule::make_offset(-8)}});
        expect_row_at(out, 0x5004, 7, 32,
                      {{8, frame_reg_rule::make_same_value()},
                       {12, frame_reg_rule::make_offset(-8)},
                       {13, frame_reg_rule::make_offset(-12)}});
        // Gaps between segments stay empty.
        TASK_CHECK(out.find_row(0x4fff) == nullptr);
        TASK_CHECK(out.find_row(0x5008) == nullptr);
        TASK_CHECK(out.find_row(0x5fff) == nullptr);
        TASK_CHECK(out.find_row(0x6008) == nullptr);
        TASK_CHECK(out.find_row(0x6fff) == nullptr);
        TASK_CHECK(out.find_row(0x7008) == nullptr);
}

// 64-bit addresses, _sf encodings, and an unaligned set_loc target
// inside the moved segment.
static void
case_addr64_sf_forms()
{
        auto dw = make_dwarf(fixture_d());
        dwarf::frame_table ft(dw, 8);
        TASK_CHECK(ft.rows() == fixture_d_rows());

        std::vector<frame_segment> segs = {
                {0x100000014ULL, 0x800000001ULL, 0xc},
        };
        auto rel = ft.relocate(segs);
        check_relocation(ft, rel, segs, 8);

        auto dw2 = make_dwarf(rel.bytes());
        dwarf::frame_table out8(dw2, 8);
        expect_row_at(out8, 0x800000001ULL, 7, 16,
                      {{30, frame_reg_rule::make_offset(-8)},
                       {17, frame_reg_rule::make_offset(24)},
                       {18, frame_reg_rule::make_val_offset(-16)}});
        expect_row_at(out8, 0x800000006ULL, 7, 16,
                      {{30, frame_reg_rule::make_offset(-8)},
                       {17, frame_reg_rule::make_offset(24)},
                       {18, frame_reg_rule::make_val_offset(-16)},
                       {2, frame_reg_rule::make_reg(3)}});
        TASK_CHECK(out8.find_row(0x800000000ULL) == nullptr);
        TASK_CHECK(out8.find_row(0x80000000dULL) == nullptr);
}

// Read .debug_frame through the real ELF loader path: build a minimal
// ELF64 file by hand, load it with elf::create_mmap_loader, and
// relocate from that.
static void
case_elf_loader()
{
        auto frame_bytes = fixture_d();

        // Minimal ELF64 (little-endian) with .debug_info, .debug_abbrev,
        // .debug_frame, and .shstrtab.
        const char shstr[] = "\0.debug_info\0.debug_abbrev\0.debug_frame\0.shstrtab\0";
        const size_t shstr_len = sizeof(shstr) - 1;
        const uint32_t name_info = 1, name_abbrev = 13, name_frame = 27,
                name_shstr = 40;

        std::vector<char> info = {7, 0, 0, 0, 4, 0, 0, 0, 0, 0, 8};
        std::vector<char> abbrev = {0};

        fbuilder elf;
        // Placeholder ELF header; patched below.
        for (int i = 0; i < 64; i++)
                elf.u8(0);
        uint64_t info_off = elf.b.size();
        elf.b.insert(elf.b.end(), info.begin(), info.end());
        uint64_t abbrev_off = elf.b.size();
        elf.b.insert(elf.b.end(), abbrev.begin(), abbrev.end());
        uint64_t frame_off = elf.b.size();
        elf.b.insert(elf.b.end(), frame_bytes.begin(), frame_bytes.end());
        uint64_t shstr_off = elf.b.size();
        elf.b.insert(elf.b.end(), shstr, shstr + shstr_len);
        uint64_t shoff = elf.b.size();

        auto shdr = [&](uint32_t name, uint32_t type, uint64_t off, uint64_t size) {
                elf.u32(name);
                elf.u32(type);
                elf.u64(0);             // flags
                elf.u64(0);             // addr
                elf.u64(off);
                elf.u64(size);
                elf.u32(0);             // link
                elf.u32(0);             // info
                elf.u64(1);             // addralign
                elf.u64(0);             // entsize
        };
        shdr(0, 0, 0, 0);                       // null
        shdr(name_info, 1, info_off, info.size());
        shdr(name_abbrev, 1, abbrev_off, abbrev.size());
        shdr(name_frame, 1, frame_off, frame_bytes.size());
        shdr(name_shstr, 3, shstr_off, shstr_len);

        // ELF header
        fbuilder &eh = elf;
        uint8_t ident[16] = {0x7f, 'E', 'L', 'F', 2, 1, 1, 0,
                             0, 0, 0, 0, 0, 0, 0, 0};
        for (int i = 0; i < 16; i++)
                eh.b[i] = (char)ident[i];
        auto patch16 = [&](size_t off, uint16_t v) {
                eh.b[off] = (char)(v & 0xff);
                eh.b[off+1] = (char)((v >> 8) & 0xff);
        };
        auto patch32 = [&](size_t off, uint32_t v) {
                for (int i = 0; i < 4; i++)
                        eh.b[off+i] = (char)((v >> (8*i)) & 0xff);
        };
        auto patch64 = [&](size_t off, uint64_t v) {
                for (int i = 0; i < 8; i++)
                        eh.b[off+i] = (char)((v >> (8*i)) & 0xff);
        };
        patch16(16, 1);                 // e_type = ET_REL
        patch16(18, 62);                // e_machine = EM_X86_64
        patch32(20, 1);                 // e_version
        patch64(40, shoff);             // e_shoff
        patch16(52, 64);                // e_ehsize
        patch16(58, 64);                // e_shentsize
        patch16(60, 5);                 // e_shnum
        patch16(62, 4);                 // e_shstrndx

        // Write to a temp file and load through the mmap loader.
        char path[] = "./frame_move_elfXXXXXX";
        int fd = mkstemp(path);
        TASK_CHECK(fd >= 0);
        size_t done = 0;
        while (done < elf.b.size()) {
                ssize_t n = write(fd, elf.b.data() + done, elf.b.size() - done);
                TASK_CHECK(n > 0);
                done += (size_t)n;
        }

        elf::elf ef(elf::create_mmap_loader(fd)); // takes ownership of fd
        dwarf::dwarf dw(dwarf::elf::create_loader(ef));
        dwarf::frame_table ft(dw, 8);
        unlink(path);

        TASK_CHECK(ft.rows() == fixture_d_rows());

        std::vector<frame_segment> segs = {
                {0x100000014ULL, 0x800000001ULL, 0xc},
        };
        auto rel = ft.relocate(segs);
        check_relocation(ft, rel, segs, 8);
}

// Malformed inputs must throw format_error (with a section offset),
// never return partial results.
static void
case_malformed_inputs()
{
        // Truncated entry: length runs past the section.
        {
                fbuilder f;
                f.u32(100);
                f.bytes({0xff, 0xff, 0xff, 0xff, 3, 0});
                expect_format_error(f.b, 4);
        }
        // DWARF64 initial length.
        {
                fbuilder f;
                f.u32(0xffffffff);
                f.u64(20);
                expect_format_error(f.b, 4);
        }
        // Unsupported CIE version.
        {
                fbuilder f, c;
                c.u32(0xffffffff);
                c.u8(4);                // version 4
                c.u8(0);
                c.uleb(1); c.sleb(-8); c.uleb(16);
                f.entry(c);
                expect_format_error(f.b, 4);
        }
        // Non-empty augmentation.
        {
                fbuilder f, c;
                c.u32(0xffffffff);
                c.u8(3);
                c.bytes({'z', 0});
                c.uleb(1); c.sleb(-8); c.uleb(16);
                f.entry(c);
                expect_format_error(f.b, 4);
        }
        // FDE referencing a nonexistent CIE.
        {
                fbuilder f;
                f.entry(cie3(4, -8, 16, {0x0c, 0x07, 0x08}));
                f.entry(fde(0x40, 0x1000, 0x10, 4, {}));
                expect_format_error(f.b, 4);
        }
        // Location moving backward.
        {
                fbuilder f;
                f.entry(cie3(4, -8, 16, {0x0c, 0x07, 0x08}));
                f.entry(fde(0, 0x1000, 0x10, 4, {
                        0x42,                           // -> 0x1008
                        0x01, 0x04, 0x10, 0x00, 0x00,   // set_loc 0x1004
                }));
                expect_format_error(f.b, 4);
        }
        // Location past the end of the FDE.
        {
                fbuilder f;
                f.entry(cie3(4, -8, 16, {0x0c, 0x07, 0x08}));
                f.entry(fde(0, 0x1000, 0x10, 4, {
                        0x04, 0x10, 0x00, 0x00, 0x00,   // advance_loc4 16
                }));
                expect_format_error(f.b, 4);
        }
        // State stack underflow.
        {
                fbuilder f;
                f.entry(cie3(4, -8, 16, {0x0c, 0x07, 0x08}));
                f.entry(fde(0, 0x1000, 0x10, 4, {0x0b}));
                expect_format_error(f.b, 4);
        }
        // Expression rules are rejected.
        {
                fbuilder f;
                f.entry(cie3(4, -8, 16, {0x0c, 0x07, 0x08}));
                f.entry(fde(0, 0x1000, 0x10, 4, {0x0f, 0x00}));
                expect_format_error(f.b, 4);
        }
        // Truncated mid-instruction: the CIE is cut off inside its
        // initial instructions.
        {
                fbuilder f;
                fbuilder c = cie3(4, -8, 16, {0x0c, 0x07, 0x08, 0x90});
                // Corrupt the length to claim more than present.
                f.u32((uint32_t)c.b.size() + 10);
                f.b.insert(f.b.end(), c.b.begin(), c.b.end());
                expect_format_error(f.b, 4);
        }
}

// Invalid segment mappings must throw invalid_argument.
static void
case_bad_mappings()
{
        auto dw = make_dwarf(fixture_ab_full());
        dwarf::frame_table ft(dw, 4);

        expect_invalid_mapping(ft, {{0x1000, 0x5000, 0}});        // zero length
        expect_invalid_mapping(ft, {{0x1000, 0x5000, 8},
                                    {0x1004, 0x6000, 8}});        // source overlap
        expect_invalid_mapping(ft, {{0x1000, 0x5000, 8},
                                    {0x1010, 0x5004, 8}});        // target overlap
        expect_invalid_mapping(ft, {{0x9000, 0x5000, 8}});        // outside any FDE
        expect_invalid_mapping(ft, {{0x1030, 0x5000, 0x1000}});   // spans FDEs
        expect_invalid_mapping(ft, {{0x1000, ~0ULL - 2, 8}});     // target wraps
        expect_invalid_mapping(ft, {{0x1000, 0x100000000ULL, 8}});// too big for 32-bit

        // An empty mapping relocates to an empty section.
        auto rel = ft.relocate({});
        TASK_CHECK(rel.bytes().empty());
        TASK_CHECK(rel.rows().empty());
}

void
register_cases(std::vector<OfflineCase> &cases)
{
        cases.push_back({"mid_cut_unaligned", case_mid_cut_unaligned});
        cases.push_back({"nested_state_restore_after_cut",
                         case_nested_state_restore_after_cut});
        cases.push_back({"reorder_segments", case_reorder_segments});
        cases.push_back({"addr64_sf_forms", case_addr64_sf_forms});
        cases.push_back({"elf_loader", case_elf_loader});
        cases.push_back({"malformed_inputs", case_malformed_inputs});
        cases.push_back({"bad_mappings", case_bad_mappings});
}
