// Contract tests for offline variable location and snapshot analysis.
//
// All synthetic DWARF and ELF data is built entirely in memory.  No
// analyzed program is ever executed and no process memory or
// registers are read: every target value comes from an explicit
// variable_snapshot.

#include "check.h"

#include "elf++.hh"
#include "dwarf/dwarf++.hh"
#include "dwarf/variable.hh"
#include "dwarf/internal.hh"

#include <fcntl.h>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace dwarf;
using std::string;
using std::vector;

typedef unsigned char u8;
typedef std::uint16_t u16;
typedef std::uint32_t u32;
typedef std::uint64_t u64;

//////////////////////////////////////////////////////////////////
// Byte emitters (all little-endian, matching the synthetic ELF)
//

static void
put_fixed(vector<u8> &v, u64 x, unsigned n)
{
        for (unsigned i = 0; i < n; i++)
                v.push_back((u8)(x >> (8 * i)));
}

static void
put_uleb(vector<u8> &v, u64 x)
{
        do {
                u8 b = x & 0x7f;
                x >>= 7;
                if (x)
                        b |= 0x80;
                v.push_back(b);
        } while (x);
}

static void
put_sleb(vector<u8> &v, int64_t x)
{
        bool more = true;
        while (more) {
                u8 b = x & 0x7f;
                bool sign = b & 0x40;
                x >>= 7;
                if ((x == 0 && !sign) || (x == -1 && sign))
                        more = false;
                else
                        b |= 0x80;
                v.push_back(b);
        }
}

static void
put_str(vector<u8> &v, const char *s)
{
        v.insert(v.end(), s, s + std::strlen(s) + 1);
}

static void
put_exprloc(vector<u8> &v, const vector<u8> &expr)
{
        put_uleb(v, expr.size());
        v.insert(v.end(), expr.begin(), expr.end());
}

//////////////////////////////////////////////////////////////////
// In-memory ELF builder
//

struct memory_loader : ::elf::loader
{
        vector<u8> bytes;

        const void *load(off_t off, size_t n) override
        {
                if (off < 0 || (size_t)off > bytes.size() ||
                    n > bytes.size() - (size_t)off)
                        throw std::range_error("memory loader out of bounds");
                return bytes.data() + off;
        }
};

struct named_section
{
        string name;
        vector<u8> data;
};

static std::shared_ptr<memory_loader>
build_elf(const vector<named_section> &secs)
{
        auto m = std::make_shared<memory_loader>();
        vector<u8> &b = m->bytes;

        // Section name string table.
        vector<u8> names{0};
        vector<u32> name_offs;
        for (auto &s : secs) {
                name_offs.push_back((u32)names.size());
                names.insert(names.end(), s.name.begin(), s.name.end());
                names.push_back(0);
        }

        auto align8 = [](size_t x) { return (x + 7) & ~size_t(7); };

        // Layout: ELF header (64), .shstrtab, then the sections.
        size_t cursor = align8(64 + names.size());
        vector<size_t> offsets;
        for (auto &s : secs) {
                offsets.push_back(cursor);
                cursor = align8(cursor + s.data.size());
        }
        size_t shnum = secs.size() + 2; // null + shstrtab + sections
        size_t shoff = align8(cursor);
        b.assign(shoff + shnum * 64, 0);

        auto put16 = [&](size_t o, u16 x) {
                b[o] = (u8)x; b[o+1] = (u8)(x >> 8);
        };
        auto put32 = [&](size_t o, u32 x) {
                for (int i = 0; i < 4; i++) b[o+i] = (u8)(x >> (8*i));
        };
        auto put64 = [&](size_t o, u64 x) {
                for (int i = 0; i < 8; i++) b[o+i] = (u8)(x >> (8*i));
        };

        // ELF64 little-endian header.
        b[0]=0x7f; b[1]='E'; b[2]='L'; b[3]='F'; b[4]=2; b[5]=1; b[6]=1;
        put16(16, 2);          // ET_EXEC
        put16(18, 62);         // x86-64
        put32(20, 1);          // version
        put64(40, shoff);
        put16(58, 64);         // shentsize
        put16(60, (u16)shnum);
        put16(62, 1);          // shstrndx

        std::memcpy(b.data() + 64, names.data(), names.size());
        for (size_t i = 0; i < secs.size(); i++)
                std::memcpy(b.data() + offsets[i], secs[i].data.data(),
                            secs[i].data.size());

        auto shdr = [&](size_t idx, u32 name, u32 type,
                        u64 offset, u64 size) {
                size_t o = shoff + idx * 64;
                put32(o, name);
                put32(o+4, type);      // 1=progbits, 3=strtab
                put64(o+8, 0);         // flags
                put64(o+16, 0);        // addr
                put64(o+24, offset);
                put64(o+32, size);
                put32(o+40, 0);        // link
                put32(o+44, 0);        // info
                put64(o+48, 1);        // addralign
                put64(o+56, 0);        // entsize
        };
        shdr(1, 0, 3, 64, names.size());
        for (size_t i = 0; i < secs.size(); i++)
                shdr(2 + i, name_offs[i], 1, offsets[i],
                     secs[i].data.size());
        return m;
}

//////////////////////////////////////////////////////////////////
// Synthetic DWARF image
//

struct image
{
        vector<u8> info;
        vector<u8> abbrev;
        vector<u8> loc;
        std::shared_ptr<memory_loader> loader;
        std::unique_ptr<::elf::elf> ef;
        std::unique_ptr<::dwarf::dwarf> dw;

        void open()
        {
                vector<named_section> secs = {
                        {".debug_info", info},
                        {".debug_abbrev", abbrev},
                };
                if (!loc.empty())
                        secs.push_back({".debug_loc", loc});
                loader = build_elf(secs);
                ef.reset(new ::elf::elf(loader));
                dw.reset(new ::dwarf::dwarf(::dwarf::elf::create_loader(*ef)));
        }
};

// Abbreviation codes used by the synthetic CUs.
enum abbr
{
        A_CU = 1,          // compile_unit: low_pc, name
        A_SUB_FB_EXPR = 2, // subprogram: name, low, high, frame_base exprloc
        A_PARAM_EXPR = 3,  // formal_parameter: name, location exprloc
        A_PARAM_LIST = 4,  // formal_parameter: name, location sec_offset
        A_SUB_FB_LIST = 5, // subprogram: name, low, high, frame_base sec_offset
        A_PARAM_AO = 6,    // formal_parameter: abstract_origin ref_addr
        A_PARAM_BLOCK = 7, // formal_parameter: name, location block1
        A_SUB_NOFB = 8,    // subprogram: name, low, high (no frame base)
};

static vector<u8>
build_abbrev()
{
        vector<u8> a;
        auto attr = [&](u32 at, u32 form) {
                put_uleb(a, at);
                put_uleb(a, form);
        };
        auto end_attrs = [&]() { a.push_back(0); a.push_back(0); };

        // 1: compile_unit with children
        put_uleb(a, A_CU); put_uleb(a, 0x11); a.push_back(1);
        attr(0x11, 0x01); // low_pc, addr
        attr(0x03, 0x08); // name, string
        end_attrs();

        // 2: subprogram with exprloc frame_base
        put_uleb(a, A_SUB_FB_EXPR); put_uleb(a, 0x2e); a.push_back(1);
        attr(0x03, 0x08); // name
        attr(0x11, 0x01); // low_pc
        attr(0x12, 0x01); // high_pc
        attr(0x40, 0x18); // frame_base, exprloc
        end_attrs();

        // 3: parameter with exprloc location
        put_uleb(a, A_PARAM_EXPR); put_uleb(a, 0x05); a.push_back(0);
        attr(0x03, 0x08); // name
        attr(0x02, 0x18); // location, exprloc
        end_attrs();

        // 4: parameter with loclist location
        put_uleb(a, A_PARAM_LIST); put_uleb(a, 0x05); a.push_back(0);
        attr(0x03, 0x08);
        attr(0x02, 0x17); // location, sec_offset
        end_attrs();

        // 5: subprogram with loclist frame_base
        put_uleb(a, A_SUB_FB_LIST); put_uleb(a, 0x2e); a.push_back(1);
        attr(0x03, 0x08);
        attr(0x11, 0x01);
        attr(0x12, 0x01);
        attr(0x40, 0x17); // frame_base, sec_offset
        end_attrs();

        // 6: parameter with abstract_origin
        put_uleb(a, A_PARAM_AO); put_uleb(a, 0x05); a.push_back(0);
        attr(0x31, 0x10); // abstract_origin, ref_addr
        end_attrs();

        // 7: parameter with legacy block1 location
        put_uleb(a, A_PARAM_BLOCK); put_uleb(a, 0x05); a.push_back(0);
        attr(0x03, 0x08);
        attr(0x02, 0x0a); // location, block1
        end_attrs();

        // 8: subprogram without frame_base
        put_uleb(a, A_SUB_NOFB); put_uleb(a, 0x2e); a.push_back(1);
        attr(0x03, 0x08);
        attr(0x11, 0x01);
        attr(0x12, 0x01);
        end_attrs();

        a.push_back(0); // end of abbrev table
        return a;
}

// Emit a CU and return the absolute .debug_info offset of its start.
static section_offset
emit_cu(vector<u8> &info, unsigned addr_size, taddr low_pc,
        const vector<u8> &dies)
{
        section_offset start = info.size();
        vector<u8> body;
        put_fixed(body, 4, 2);             // version (uhalf, value 4)
        put_fixed(body, 0, 4);             // abbrev offset (uword, 0)
        body.push_back((u8)addr_size);     // address size
        body.insert(body.end(), dies.begin(), dies.end());
        body.push_back(0);                 // root null terminator

        put_fixed(info, body.size(), 4);   // unit length (uword)
        info.insert(info.end(), body.begin(), body.end());
        (void)low_pc;
        return start;
}

// Locate a DIE inside one CU's emitted byte stream by abbrev code
// occurrence index.  Returns the offset relative to the CU start.
static section_offset
find_die_offset(const vector<u8> &dies, unsigned addr_size,
                abbr wanted, unsigned occurrence = 0)
{
        // Walk the DIE stream at the attribute level using a minimal
        // form-aware scanner.
        size_t p = 0;
        unsigned seen = 0;
        auto at = [&](size_t i) -> u64 {
                // Read a ULEB at position i (only used for codes).
                u64 r = 0; unsigned sh = 0;
                while (i < dies.size()) {
                        u8 b = dies[i++];
                        r |= (u64)(b & 0x7f) << sh;
                        if (!(b & 0x80)) break;
                        sh += 7;
                }
                return r;
        };
        while (p < dies.size()) {
                size_t die_start = p;
                u64 code = at(p);
                // advance p past the code uleb
                while (p < dies.size() && (dies[p++] & 0x80)) {}
                if (code == 0) {
                        // null DIE
                        continue;
                }
                if (code == wanted && seen++ == occurrence)
                        return 11 + die_start;
                // Skip the DIE's attributes based on its abbrev.
                auto skip_addr = [&]() { p += addr_size; };
                auto skip_string = [&]() {
                        while (p < dies.size() && dies[p] != 0) p++;
                        p++;
                };
                auto skip_uleb = [&]() {
                        while (p < dies.size() && (dies[p++] & 0x80)) {}
                };
                auto skip_exprloc = [&]() {
                        size_t q = p; u64 len = 0; unsigned sh = 0;
                        while (q < dies.size()) {
                                u8 b = dies[q++];
                                len |= (u64)(b & 0x7f) << sh;
                                if (!(b & 0x80)) break;
                                sh += 7;
                        }
                        p = q + len;
                };
                switch ((abbr)code) {
                case A_CU:
                        skip_addr(); skip_string();
                        break;
                case A_SUB_FB_EXPR:
                        skip_string(); skip_addr(); skip_addr();
                        skip_exprloc();
                        break;
                case A_PARAM_EXPR:
                        skip_string(); skip_exprloc();
                        break;
                case A_PARAM_LIST:
                        skip_string(); p += 4;
                        break;
                case A_SUB_FB_LIST:
                        skip_string(); skip_addr(); skip_addr(); p += 4;
                        break;
                case A_PARAM_AO:
                        p += 4; // ref_addr in dwarf32
                        break;
                case A_PARAM_BLOCK: {
                        skip_string();
                        u8 len = dies[p++];
                        p += len;
                        break;
                }
                case A_SUB_NOFB:
                        skip_string(); skip_addr(); skip_addr();
                        break;
                }
        }
        throw std::runtime_error("DIE not found in synthetic CU");
}

static variable_snapshot
empty_snapshot(snapshot_endian order = snapshot_endian::little)
{
        return variable_snapshot(order);
}

//////////////////////////////////////////////////////////////////
// Group: elf (real golden file + controlled snapshots)
//

static int
elf_golden_snapshots()
{
        int fd = open("test/golden-gcc-4.9.2/example", O_RDONLY);
        TASK_CHECK(fd >= 0);
        ::elf::elf ef(::elf::create_mmap_loader(fd));
        ::dwarf::dwarf dw(::dwarf::elf::create_loader(ef));

        const compilation_unit &cu = compilation_unit_at(dw, 0);
        die func = die_at(cu, 0x2b);
        die var = die_at(cu, 0x4c);
        TASK_CHECK(func.tag == DW_TAG::subprogram);
        TASK_CHECK(var.tag == DW_TAG::formal_parameter);

        const taddr pc = 0x4004bd;

        // Snapshot a: CFA 0x7000, bytes 2a000000 at 0x6fdc -> 42.
        {
                variable_snapshot snap(empty_snapshot());
                snap.set_cfa(0x7000);
                snap.add_memory_segment(0x6fdc,
                                        vector<u8>{0x2a, 0, 0, 0});
                variable_result r = inspect_variable(var, func, pc, 4, snap);
                TASK_CHECK(r.status == variable_status::value);
                TASK_CHECK(r.value == 42);
                TASK_CHECK((r.raw_bytes == vector<u8>{0x2a,0,0,0}));
                TASK_CHECK(r.has_location_attr);
                TASK_CHECK(r.location_die_offset == 0x4c);
                TASK_CHECK(r.frame_base_used);
                TASK_CHECK(r.frame_base_die_offset == 0x2b);
                TASK_CHECK(r.cfa_accessed);
                TASK_CHECK(r.cfa_value == 0x7000);
                TASK_CHECK(r.registers.empty());
                TASK_CHECK(r.memory.size() == 1);
                TASK_CHECK(r.memory[0].address == 0x6fdc);
                TASK_CHECK((r.memory[0].bytes == vector<u8>{0x2a,0,0,0}));
                TASK_CHECK(r.has_value_address && r.value_address == 0x6fdc);
                TASK_CHECK(r.expr_section == section_type::info);

                // The public at_location entry gives the raw address.
                class c : public expr_context {
                public:
                        taddr cfa() override { return 0x7000; }
                } ctx;
                expr_result er = at_location(var, func, pc, &ctx);
                TASK_CHECK(er.location_type == expr_result::type::address);
                TASK_CHECK(er.value == 0x6fdc);
        }

        // Snapshot b: shifted CFA and bytes, value 17000000 -> 23.
        {
                variable_snapshot snap(empty_snapshot());
                snap.set_cfa(0x8000);
                snap.add_memory_segment(0x7fdc,
                                        vector<u8>{0x17, 0, 0, 0});
                variable_result r = inspect_variable(var, func, pc, 4, snap);
                TASK_CHECK(r.status == variable_status::value);
                TASK_CHECK(r.value == 23);
                TASK_CHECK((r.raw_bytes == vector<u8>{0x17,0,0,0}));
                TASK_CHECK(r.value_address == 0x7fdc);
                TASK_CHECK(r.cfa_value == 0x8000);
        }

        // Drop one required byte: missing_memory, no half value.
        {
                variable_snapshot snap(empty_snapshot());
                snap.set_cfa(0x7000);
                snap.add_memory_segment(0x6fdc, vector<u8>{0x2a, 0, 0});
                variable_result r = inspect_variable(var, func, pc, 4, snap);
                TASK_CHECK(r.status == variable_status::missing_memory);
                TASK_CHECK(r.raw_bytes.empty());
        }

        // No CFA: the fbreg/call_frame_cfa chain cannot proceed.
        {
                variable_snapshot snap(empty_snapshot());
                snap.add_memory_segment(0x6fdc,
                                        vector<u8>{0x2a, 0, 0, 0});
                variable_result r = inspect_variable(var, func, pc, 4, snap);
                TASK_CHECK(r.status == variable_status::missing_cfa);
        }
        return 0;
}

//////////////////////////////////////////////////////////////////
// Group: locations
//

// Build the standard 8-byte-address CU with a loclist parameter.
static void
emit_loclist_cu(image &im, taddr cu_low, taddr fn_low, taddr fn_high,
                section_offset list_off,
                vector<section_offset> *offs,
                abbr sub_code = A_SUB_FB_EXPR,
                const vector<u8> &frame_base_expr = vector<u8>{0x9c})
{
        vector<u8> dies;
        put_uleb(dies, A_CU);
        put_fixed(dies, cu_low, 8);
        put_str(dies, "cu");

        put_uleb(dies, sub_code);
        put_str(dies, "fn");
        put_fixed(dies, fn_low, 8);
        put_fixed(dies, fn_high, 8);
        if (sub_code == A_SUB_FB_EXPR)
                put_exprloc(dies, frame_base_expr);
        else
                put_fixed(dies, list_off, 4);

        put_uleb(dies, A_PARAM_LIST);
        put_str(dies, "v");
        put_fixed(dies, list_off, 4);
        dies.push_back(0); // children null

        offs->push_back(find_die_offset(dies, 8, A_CU));
        offs->push_back(find_die_offset(dies, 8, A_SUB_FB_EXPR == sub_code
                                        ? A_SUB_FB_EXPR : A_SUB_FB_LIST));
        offs->push_back(find_die_offset(dies, 8, A_PARAM_LIST));
        emit_cu(im.info, 8, cu_low, dies);
}

static int
locations_base_change_gap_first_match()
{
        image im;
        im.abbrev = build_abbrev();

        // Location list with a base-address selection, three regular
        // entries (register, literal, empty expression) and a second
        // register entry overlapping the first to test first-match.
        const taddr base = 0x400000;
        auto addr = [&](taddr x) { put_fixed(im.loc, x, 8); };
        addr(~(taddr)0); addr(base);                 // base selection
        addr(0x10); addr(0x20);                      // entry 1 range
        put_fixed(im.loc, 1, 2); im.loc.push_back(0x53); // DW_OP_reg3
        addr(0x14); addr(0x18);                      // overlapping entry
        put_fixed(im.loc, 1, 2); im.loc.push_back(0x54); // DW_OP_reg4
        addr(0x20); addr(0x30);                      // literal entry
        {
                vector<u8> e{0x37, 0x9f};            // lit7; stack_value
                put_fixed(im.loc, e.size(), 2);
                im.loc.insert(im.loc.end(), e.begin(), e.end());
        }
        addr(0x30); addr(0x40);                      // empty expression
        put_fixed(im.loc, 0, 2);
        addr(0); addr(0);                            // terminator

        vector<section_offset> offs;
        emit_loclist_cu(im, 0x1000, base, base + 0x100, 0, &offs);
        im.open();

        const compilation_unit &cu = im.dw->compilation_units().at(0);
        die fn = die_at(cu, offs[1]);
        die var = die_at(cu, offs[2]);

        // First matching entry wins (reg3, not the overlapping reg4).
        {
                variable_snapshot s(empty_snapshot());
                s.set_cfa(base);
                s.set_register(3, 0xaaaa);
                s.set_register(4, 0xbbbb);
                variable_result r = inspect_variable(var, fn, base + 0x15, 4, s);
                TASK_CHECK(r.status == variable_status::value);
                TASK_CHECK(r.value == 0xaaaa);
                TASK_CHECK(r.registers.size() == 1);
                TASK_CHECK(r.registers[0].regnum == 3);
                TASK_CHECK(r.location_is_list);
                TASK_CHECK(r.selected);
                TASK_CHECK(r.range_low == base + 0x10);
                TASK_CHECK(r.range_high == base + 0x20);
                TASK_CHECK(r.expr_section == section_type::loc);
        }

        // Literal (stack value) entry: memory must not be touched.
        {
                variable_snapshot s(empty_snapshot());
                s.set_cfa(base);
                s.add_memory_segment(base + 0x20, vector<u8>{1,2,3,4});
                variable_result r = inspect_variable(var, fn, base + 0x25, 4, s);
                TASK_CHECK(r.status == variable_status::value);
                TASK_CHECK(r.value == 7);
                TASK_CHECK(r.memory.empty());
        }

        // Empty expression entry: unavailable.
        {
                variable_snapshot s(empty_snapshot());
                s.set_cfa(base);
                variable_result r = inspect_variable(var, fn, base + 0x35, 4, s);
                TASK_CHECK(r.status == variable_status::unavailable);
                TASK_CHECK(r.expr_empty);
        }

        // Gap after the last range: unavailable, nothing selected.
        {
                variable_snapshot s(empty_snapshot());
                s.set_cfa(base);
                variable_result r = inspect_variable(var, fn, base + 0x45, 4, s);
                TASK_CHECK(r.status == variable_status::unavailable);
                TASK_CHECK(!r.selected);
        }
        return 0;
}

static int
locations_same_offset_different_sections()
{
        image im;
        im.abbrev = build_abbrev();

        // Build a CU whose parameter exprloc (in .debug_info) contains
        // DW_OP_reg3, and record that expression's section offset K.
        vector<u8> dies;
        put_uleb(dies, A_CU);
        put_fixed(dies, 0x1000, 8);
        put_str(dies, "cu");
        put_uleb(dies, A_SUB_FB_EXPR);
        put_str(dies, "fn");
        put_fixed(dies, 0x1000, 8);
        put_fixed(dies, 0x1100, 8);
        put_exprloc(dies, vector<u8>{0x9c}); // frame_base = call_frame_cfa
        put_uleb(dies, A_PARAM_EXPR);
        put_str(dies, "v");
        put_exprloc(dies, vector<u8>{0x53}); // DW_OP_reg3 in .debug_info
        dies.push_back(0);

        section_offset cu_start = emit_cu(im.info, 8, 0x1000, dies);
        // K is the absolute .debug_info offset of the DW_OP_reg3
        // expression byte.  In this small image that opcode is unique.
        section_offset k = 0;
        unsigned hits = 0;
        for (size_t i = 0; i < im.info.size(); i++) {
                if (im.info[i] == 0x53) {
                        k = i;
                        hits++;
                }
        }
        TASK_CHECK(hits == 1);
        (void)cu_start;

        // Build the loclist entry first.  Its layout is range(8) +
        // range(8) + length(2) + expression, so the expression byte
        // sits 18 bytes after the start of the list.  Position the
        // list so that its expression lands at the SAME numeric
        // section offset K as the DW_OP_reg3 byte in .debug_info,
        // while holding a different opcode (DW_OP_reg5).
        vector<u8> entry;
        put_fixed(entry, 0x10, 8);
        put_fixed(entry, 0x20, 8);
        put_fixed(entry, 1, 2);
        entry.push_back(0x55); // DW_OP_reg5
        put_fixed(entry, 0, 8);
        put_fixed(entry, 0, 8); // end of list
        const section_offset header_bytes = 18;
        TASK_CHECK(k >= header_bytes);
        section_offset list_start = k - header_bytes;
        while (im.loc.size() < list_start)
                im.loc.push_back(0);
        im.loc.insert(im.loc.end(), entry.begin(), entry.end());

        // Second parameter DIE referring to the loclist at offset K.
        vector<u8> dies2;
        put_uleb(dies2, A_CU);
        put_fixed(dies2, 0x2000, 8);
        put_str(dies2, "cu2");
        put_uleb(dies2, A_SUB_FB_EXPR);
        put_str(dies2, "fn2");
        put_fixed(dies2, 0x2000, 8);
        put_fixed(dies2, 0x2100, 8);
        put_exprloc(dies2, vector<u8>{0x9c});
        put_uleb(dies2, A_PARAM_LIST);
        put_str(dies2, "v2");
        put_fixed(dies2, list_start, 4);
        dies2.push_back(0);
        vector<section_offset> offs2;
        offs2.push_back(find_die_offset(dies2, 8, A_CU));
        offs2.push_back(find_die_offset(dies2, 8, A_SUB_FB_EXPR));
        offs2.push_back(find_die_offset(dies2, 8, A_PARAM_LIST));
        emit_cu(im.info, 8, 0x2000, dies2);
        im.open();

        const compilation_unit &cu2 = im.dw->compilation_units().at(1);
        die fn = die_at(cu2, offs2[1]);
        die var = die_at(cu2, offs2[2]);

        // Only reg5 is provided.  A loclist read from .debug_info by
        // mistake would demand reg3 and fail.  The list's range is
        // [cu_low_pc + 0x10, cu_low_pc + 0x20) with no base change.
        variable_snapshot s(empty_snapshot());
        s.set_cfa(0x2000);
        s.set_register(5, 0x777);
        variable_result r = inspect_variable(var, fn, 0x2010, 4, s);
        TASK_CHECK(r.status == variable_status::value);
        TASK_CHECK(r.value == 0x777);
        TASK_CHECK(r.expr_section == section_type::loc);
        TASK_CHECK(r.expr_offset == k);
        return 0;
}

static int
locations_cross_cu_abstract_origin()
{
        image im;
        im.abbrev = build_abbrev();

        // CU1: abstract parameter carrying an exprloc location.
        vector<u8> dies1;
        put_uleb(dies1, A_CU);
        put_fixed(dies1, 0x1000, 8);
        put_str(dies1, "cu1");
        put_uleb(dies1, A_SUB_FB_EXPR);
        put_str(dies1, "fn1");
        put_fixed(dies1, 0x1000, 8);
        put_fixed(dies1, 0x1100, 8);
        put_exprloc(dies1, vector<u8>{0x9c});
        put_uleb(dies1, A_PARAM_EXPR);
        put_str(dies1, "abs");
        put_exprloc(dies1, vector<u8>{0x52}); // DW_OP_reg2
        dies1.push_back(0);
        section_offset cu1 = emit_cu(im.info, 8, 0x1000, dies1);
        section_offset param_abs = cu1 + find_die_offset(dies1, 8, A_PARAM_EXPR);

        // CU2: concrete parameter inherits the location via a
        // cross-CU DW_AT_abstract_origin (ref_addr).
        vector<u8> dies2;
        put_uleb(dies2, A_CU);
        put_fixed(dies2, 0x2000, 8);
        put_str(dies2, "cu2");
        put_uleb(dies2, A_SUB_FB_EXPR);
        put_str(dies2, "fn2");
        put_fixed(dies2, 0x2000, 8);
        put_fixed(dies2, 0x2100, 8);
        put_exprloc(dies2, vector<u8>{0x9c});
        put_uleb(dies2, A_PARAM_AO);
        put_fixed(dies2, param_abs, 4);
        dies2.push_back(0);
        vector<section_offset> offs2;
        offs2.push_back(find_die_offset(dies2, 8, A_CU));
        offs2.push_back(find_die_offset(dies2, 8, A_SUB_FB_EXPR));
        offs2.push_back(find_die_offset(dies2, 8, A_PARAM_AO));
        emit_cu(im.info, 8, 0x2000, dies2);
        im.open();

        const compilation_unit &cu2 = im.dw->compilation_units().at(1);
        die fn = die_at(cu2, offs2[1]);
        die var = die_at(cu2, offs2[2]);

        variable_snapshot s(empty_snapshot());
        s.set_cfa(0x2000);
        s.set_register(2, 0x4242);
        variable_result r = inspect_variable(var, fn, 0x2010, 4, s);
        TASK_CHECK(r.status == variable_status::value);
        TASK_CHECK(r.value == 0x4242);
        // The location was supplied by the abstract DIE in CU1.
        TASK_CHECK(r.location_die_offset == param_abs);
        TASK_CHECK(r.registers[0].regnum == 2);
        return 0;
}

static int
locations_four_byte_cu_and_legacy_block()
{
        image im;
        im.abbrev = build_abbrev();

        // 4-byte-address CU with its own low_pc and a loclist that
        // uses a base-address selection.
        const taddr base = 0x30000;
        auto addr4 = [&](taddr x) { put_fixed(im.loc, x, 4); };
        addr4(0xffffffff); addr4(base);       // base selection
        addr4(0x10); addr4(0x20);
        put_fixed(im.loc, 1, 2);
        im.loc.push_back(0x57);               // DW_OP_reg7
        addr4(0); addr4(0);                   // terminator

        vector<u8> dies;
        put_uleb(dies, A_CU);
        put_fixed(dies, 0x20000, 4);
        put_str(dies, "cu");
        put_uleb(dies, A_SUB_FB_EXPR);
        put_str(dies, "fn");
        put_fixed(dies, 0x20000, 4);
        put_fixed(dies, 0x20100, 4);
        put_exprloc(dies, vector<u8>{0x9c});
        put_uleb(dies, A_PARAM_LIST);
        put_str(dies, "v");
        put_fixed(dies, 0, 4);
        dies.push_back(0);
        vector<section_offset> offs;
        offs.push_back(find_die_offset(dies, 4, A_CU));
        offs.push_back(find_die_offset(dies, 4, A_SUB_FB_EXPR));
        offs.push_back(find_die_offset(dies, 4, A_PARAM_LIST));
        emit_cu(im.info, 4, 0x20000, dies);

        // A second CU exercising a legacy DW_FORM_block1 location.
        vector<u8> dies2;
        put_uleb(dies2, A_CU);
        put_fixed(dies2, 0x40000, 8);
        put_str(dies2, "cu2");
        put_uleb(dies2, A_SUB_FB_EXPR);
        put_str(dies2, "fn2");
        put_fixed(dies2, 0x40000, 8);
        put_fixed(dies2, 0x40100, 8);
        put_exprloc(dies2, vector<u8>{0x9c});
        put_uleb(dies2, A_PARAM_BLOCK);
        put_str(dies2, "b");
        dies2.push_back(1);                   // block1 length
        dies2.push_back(0x51);                // DW_OP_reg1
        dies2.push_back(0);
        vector<section_offset> offs2;
        offs2.push_back(find_die_offset(dies2, 8, A_CU));
        offs2.push_back(find_die_offset(dies2, 8, A_SUB_FB_EXPR));
        offs2.push_back(find_die_offset(dies2, 8, A_PARAM_BLOCK));
        emit_cu(im.info, 8, 0x40000, dies2);
        im.open();

        // 4-byte CU list interpreted with the CU's address size.
        {
                const compilation_unit &cu = im.dw->compilation_units().at(0);
                die fn = die_at(cu, offs[1]);
                die var = die_at(cu, offs[2]);
                variable_snapshot s(empty_snapshot());
                s.set_cfa(base);
                s.set_register(7, 0x333);
                variable_result r = inspect_variable(var, fn, base + 0x15, 4, s);
                TASK_CHECK(r.status == variable_status::value);
                TASK_CHECK(r.value == 0x333);
                TASK_CHECK(r.expr_section == section_type::loc);
        }

        // Legacy block location.
        {
                const compilation_unit &cu = im.dw->compilation_units().at(1);
                die fn = die_at(cu, offs2[1]);
                die var = die_at(cu, offs2[2]);
                variable_snapshot s(empty_snapshot());
                s.set_cfa(0x40000);
                s.set_register(1, 0x999);
                variable_result r = inspect_variable(var, fn, 0x40010, 4, s);
                TASK_CHECK(r.status == variable_status::value);
                TASK_CHECK(r.value == 0x999);
        }
        return 0;
}

//////////////////////////////////////////////////////////////////
// Group: context
//

// CU with one subprogram/parameter whose location and frame_base
// expressions are supplied by the caller.
static void
emit_expr_cu(image &im, unsigned addr_size, const vector<u8> &fb_expr,
             const vector<u8> &loc_expr, abbr loc_code = A_PARAM_EXPR,
             vector<section_offset> *offs = nullptr)
{
        vector<u8> dies;
        put_uleb(dies, A_CU);
        put_fixed(dies, 0x1000, addr_size);
        put_str(dies, "cu");
        put_uleb(dies, A_SUB_FB_EXPR);
        put_str(dies, "fn");
        put_fixed(dies, 0x1000, addr_size);
        put_fixed(dies, 0x1100, addr_size);
        put_exprloc(dies, fb_expr);
        put_uleb(dies, loc_code);
        put_str(dies, "v");
        put_exprloc(dies, loc_expr);
        dies.push_back(0);
        if (offs) {
                offs->push_back(find_die_offset(dies, addr_size, A_CU));
                offs->push_back(find_die_offset(dies, addr_size, A_SUB_FB_EXPR));
                offs->push_back(find_die_offset(dies, addr_size, loc_code));
        }
        emit_cu(im.info, addr_size, 0x1000, dies);
}

static int
context_register_and_literal_ignore_memory()
{
        image im;
        im.abbrev = build_abbrev();
        // DW_OP_reg2
        emit_expr_cu(im, 8, vector<u8>{0x9c}, vector<u8>{0x52});
        im.open();
        const compilation_unit &cu = im.dw->compilation_units().at(0);
        die fn = *cu.root().begin();
        die var = *fn.begin();

        variable_snapshot s(empty_snapshot());
        s.set_cfa(0x5000);
        s.set_register(2, 0x11223344);
        // Memory that must never be consulted for a register location.
        s.add_memory_segment(0x9000, vector<u8>{0xff, 0xff, 0xff, 0xff});

        // Width 2 truncates to the low 16 bits.
        variable_result r = inspect_variable(var, fn, 0x1010, 2, s);
        TASK_CHECK(r.status == variable_status::value);
        TASK_CHECK(r.value == 0x3344);
        TASK_CHECK((r.raw_bytes == vector<u8>{0x44, 0x33}));
        TASK_CHECK(r.memory.empty());
        TASK_CHECK(r.has_value_address == false);
        TASK_CHECK(r.registers.size() == 1 && r.registers[0].regnum == 2);
        // A plain register location must not touch the CFA or frame base.
        TASK_CHECK(r.cfa_accessed == false);
        TASK_CHECK(r.frame_base_used == false);

        // Literal stack value also ignores memory.
        image im2;
        im2.abbrev = build_abbrev();
        // DW_OP_lit5; DW_OP_stack_value
        emit_expr_cu(im2, 8, vector<u8>{0x9c}, vector<u8>{0x35, 0x9f});
        im2.open();
        die fn2 = *im2.dw->compilation_units().at(0).root().begin();
        die var2 = *fn2.begin();
        variable_snapshot s2(empty_snapshot());
        s2.set_cfa(0x5000);
        s2.add_memory_segment(0, vector<u8>{1, 2, 3, 4});
        variable_result r2 = inspect_variable(var2, fn2, 0x1010, 4, s2);
        TASK_CHECK(r2.status == variable_status::value);
        TASK_CHECK(r2.value == 5);
        TASK_CHECK(r2.memory.empty());
        TASK_CHECK(r2.registers.empty());
        TASK_CHECK(r2.cfa_accessed == false);
        TASK_CHECK(r2.frame_base_used == false);
        return 0;
}

static int
context_negative_fbreg_and_register_frame_base()
{
        image im;
        im.abbrev = build_abbrev();
        // frame_base = DW_OP_reg6; location = DW_OP_fbreg(-4)
        emit_expr_cu(im, 8, vector<u8>{0x56}, vector<u8>{0x91, 0x7c});
        im.open();
        die fn = *im.dw->compilation_units().at(0).root().begin();
        die var = *fn.begin();

        variable_snapshot s(empty_snapshot());
        s.set_register(6, 0x2000);
        s.add_memory_segment(0x1ffc, vector<u8>{0x0a, 0, 0, 0});
        variable_result r = inspect_variable(var, fn, 0x1010, 4, s);
        TASK_CHECK(r.status == variable_status::value);
        TASK_CHECK(r.value == 10);
        TASK_CHECK(r.value_address == 0x1ffc);
        // The frame base required reading register 6.
        TASK_CHECK(r.registers.size() == 1 && r.registers[0].regnum == 6);
        TASK_CHECK(r.registers[0].value == 0x2000);
        TASK_CHECK(r.memory.size() == 1 && r.memory[0].address == 0x1ffc);

        // Missing register 6: missing_register, no zero fill.
        variable_snapshot s2(empty_snapshot());
        s2.add_memory_segment(0x1ffc, vector<u8>{0x0a, 0, 0, 0});
        variable_result r2 = inspect_variable(var, fn, 0x1010, 4, s2);
        TASK_CHECK(r2.status == variable_status::missing_register);
        return 0;
}

static int
context_literal_frame_base_not_dereferenced()
{
        image im;
        im.abbrev = build_abbrev();
        // frame_base = DW_OP_lit1; DW_OP_stack_value (literal base 1)
        // location = DW_OP_fbreg(8) -> address 9
        emit_expr_cu(im, 8, vector<u8>{0x31, 0x9f}, vector<u8>{0x91, 0x08});
        im.open();
        die fn = *im.dw->compilation_units().at(0).root().begin();
        die var = *fn.begin();

        variable_snapshot s(empty_snapshot());
        // Put a decoy byte at address 1 (the frame base itself).
        s.add_memory_segment(1, vector<u8>{0xee});
        s.add_memory_segment(9, vector<u8>{0x77, 0, 0, 0});
        variable_result r = inspect_variable(var, fn, 0x1010, 4, s);
        TASK_CHECK(r.status == variable_status::value);
        TASK_CHECK(r.value == 0x77);
        TASK_CHECK(r.value_address == 9);
        // Only the variable's own address was read.
        TASK_CHECK(r.memory.size() == 1 && r.memory[0].address == 9);
        return 0;
}

static int
context_cross_segment_and_breg()
{
        image im;
        im.abbrev = build_abbrev();
        // location = DW_OP_addr 0x1000 (8-byte address)
        vector<u8> loc{0x03};
        put_fixed(loc, 0x1000, 8);
        emit_expr_cu(im, 8, vector<u8>{0x9c}, loc);
        im.open();
        die fn = *im.dw->compilation_units().at(0).root().begin();
        die var = *fn.begin();

        // Two adjacent segments concatenate into one 4-byte value.
        variable_snapshot s(empty_snapshot());
        s.set_cfa(0x5000);
        s.add_memory_segment(0x1000, vector<u8>{0xaa, 0xbb});
        s.add_memory_segment(0x1002, vector<u8>{0xcc, 0xdd});
        variable_result r = inspect_variable(var, fn, 0x1010, 4, s);
        TASK_CHECK(r.status == variable_status::value);
        TASK_CHECK(r.value == 0xddccbbaaULL);
        TASK_CHECK((r.raw_bytes == vector<u8>{0xaa,0xbb,0xcc,0xdd}));

        // Remove one byte: missing_memory and no half value.
        variable_snapshot s2(empty_snapshot());
        s2.set_cfa(0x5000);
        s2.add_memory_segment(0x1000, vector<u8>{0xaa, 0xbb});
        variable_result r2 = inspect_variable(var, fn, 0x1010, 4, s2);
        TASK_CHECK(r2.status == variable_status::missing_memory);
        TASK_CHECK(r2.raw_bytes.empty());

        // DW_OP_breg0(+16): address from register plus offset.
        image im2;
        im2.abbrev = build_abbrev();
        vector<u8> loc2{0x70}; put_sleb(loc2, 16); // breg0, +16
        emit_expr_cu(im2, 8, vector<u8>{0x9c}, loc2);
        im2.open();
        die fn2 = *im2.dw->compilation_units().at(0).root().begin();
        die var2 = *fn2.begin();
        variable_snapshot s3(empty_snapshot());
        s3.set_register(0, 0x3000);
        s3.add_memory_segment(0x3010, vector<u8>{0x01, 0, 0, 0});
        variable_result r3 = inspect_variable(var2, fn2, 0x1010, 4, s3);
        TASK_CHECK(r3.status == variable_status::value);
        TASK_CHECK(r3.value == 1);
        TASK_CHECK(r3.value_address == 0x3010);
        TASK_CHECK(r3.registers[0].regnum == 0);
        return 0;
}

static int
context_result_survives_released_inputs()
{
        image im;
        im.abbrev = build_abbrev();
        emit_expr_cu(im, 8, vector<u8>{0x9c}, vector<u8>{0x52});
        im.open();
        die fn = *im.dw->compilation_units().at(0).root().begin();
        die var = *fn.begin();

        variable_snapshot s(empty_snapshot());
        s.set_cfa(0x5000);
        s.set_register(2, 0x6677);
        variable_result r = inspect_variable(var, fn, 0x1010, 4, s);
        TASK_CHECK(r.status == variable_status::value);

        // Destroy every borrowed input and mutate the source storage.
        im.dw.reset();
        im.ef.reset();
        im.loader.reset();
        im.info.assign(im.info.size(), 0);
        im.abbrev.assign(im.abbrev.size(), 0);
        s.set_register(2, 0);

        // The owning result is still fully inspectable.
        TASK_CHECK(r.value == 0x6677);
        TASK_CHECK((r.raw_bytes == vector<u8>{0x77, 0x66, 0, 0}));
        TASK_CHECK(r.registers.size() == 1 && r.registers[0].value == 0x6677);
        TASK_CHECK(r.error.empty() || !r.error.empty());
        TASK_CHECK(r.location_die_offset != 0);
        return 0;
}

//////////////////////////////////////////////////////////////////
// Group: errors
//

static image
make_loclist_image(const vector<u8> &loc_bytes, vector<section_offset> &offs,
                   taddr cu_low = 0x1000)
{
        image im;
        im.abbrev = build_abbrev();
        im.loc = loc_bytes;
        emit_loclist_cu(im, cu_low, cu_low, cu_low + 0x100, 0, &offs);
        im.open();
        return im;
}

static int
errors_truncated_and_bad_ranges()
{
        const taddr base = 0x400000;

        // Truncated: a pair of addresses with no terminator in sight.
        {
                vector<u8> loc;
                put_fixed(loc, ~(taddr)0, 8); put_fixed(loc, base, 8);
                put_fixed(loc, 0x10, 8); put_fixed(loc, 0x20, 8);
                put_fixed(loc, 1, 2); loc.push_back(0x53);
                // Section ends here: no terminator.
                vector<section_offset> offs;
                image im = make_loclist_image(loc, offs);
                const compilation_unit &cu = im.dw->compilation_units().at(0);
                die fn = die_at(cu, offs[1]);
                die var = die_at(cu, offs[2]);
                variable_snapshot s(empty_snapshot());
                s.set_register(3, 1);
                variable_result r = inspect_variable(var, fn, base + 0x15, 4, s);
                TASK_CHECK(r.status == variable_status::malformed);
                TASK_CHECK(r.error.find(".debug_loc") != string::npos);
                TASK_CHECK(r.error.find("offset") != string::npos);
        }

        // Reversed range (high <= low).
        {
                vector<u8> loc;
                put_fixed(loc, 0x20, 8); put_fixed(loc, 0x10, 8);
                put_fixed(loc, 1, 2); loc.push_back(0x53);
                put_fixed(loc, 0, 8); put_fixed(loc, 0, 8);
                vector<section_offset> offs;
                image im = make_loclist_image(loc, offs);
                const compilation_unit &cu = im.dw->compilation_units().at(0);
                die fn = die_at(cu, offs[1]);
                die var = die_at(cu, offs[2]);
                variable_snapshot s(empty_snapshot());
                variable_result r = inspect_variable(var, fn, 0x1010, 4, s);
                TASK_CHECK(r.status == variable_status::malformed);
                TASK_CHECK(r.error.find("reversed") != string::npos);
        }

        // Base-address wraparound.
        {
                vector<u8> loc;
                put_fixed(loc, ~(taddr)0, 8);
                put_fixed(loc, ~(taddr)0 - 1, 8); // base near the top
                put_fixed(loc, 0x10, 8); put_fixed(loc, 0x20, 8);
                put_fixed(loc, 1, 2); loc.push_back(0x53);
                put_fixed(loc, 0, 8); put_fixed(loc, 0, 8);
                vector<section_offset> offs;
                image im = make_loclist_image(loc, offs);
                const compilation_unit &cu = im.dw->compilation_units().at(0);
                die fn = die_at(cu, offs[1]);
                die var = die_at(cu, offs[2]);
                variable_snapshot s(empty_snapshot());
                variable_result r = inspect_variable(var, fn, 0x1010, 4, s);
                TASK_CHECK(r.status == variable_status::malformed);
                TASK_CHECK(r.error.find("wraps") != string::npos);
        }
        return 0;
}

static int
errors_recursive_missing_unsupported()
{
        // Recursive frame base: DW_AT_frame_base itself uses fbreg.
        {
                image im;
                im.abbrev = build_abbrev();
                emit_expr_cu(im, 8,
                             vector<u8>{0x91, 0x00},       // fbreg 0
                             vector<u8>{0x91, 0x00});
                im.open();
                die fn = *im.dw->compilation_units().at(0).root().begin();
                die var = *fn.begin();
                variable_snapshot s(empty_snapshot());
                variable_result r = inspect_variable(var, fn, 0x1010, 4, s);
                TASK_CHECK(r.status == variable_status::malformed);
                TASK_CHECK(r.error.find("recursive") != string::npos);
        }

        // Missing frame base: subprogram without DW_AT_frame_base.
        {
                image im;
                im.abbrev = build_abbrev();
                vector<u8> dies;
                put_uleb(dies, A_CU);
                put_fixed(dies, 0x1000, 8);
                put_str(dies, "cu");
                put_uleb(dies, A_SUB_NOFB);
                put_str(dies, "fn");
                put_fixed(dies, 0x1000, 8);
                put_fixed(dies, 0x1100, 8);
                put_uleb(dies, A_PARAM_EXPR);
                put_str(dies, "v");
                put_exprloc(dies, vector<u8>{0x91, 0x00}); // fbreg 0
                dies.push_back(0);
                vector<section_offset> offs;
                offs.push_back(find_die_offset(dies, 8, A_CU));
                offs.push_back(find_die_offset(dies, 8, A_SUB_NOFB));
                offs.push_back(find_die_offset(dies, 8, A_PARAM_EXPR));
                emit_cu(im.info, 8, 0x1000, dies);
                im.open();
                const compilation_unit &cu = im.dw->compilation_units().at(0);
                die fn = die_at(cu, offs[1]);
                die var = die_at(cu, offs[2]);
                variable_snapshot s(empty_snapshot());
                variable_result r = inspect_variable(var, fn, 0x1010, 4, s);
                TASK_CHECK(r.status == variable_status::unavailable);
        }

        // Unsupported operation is reported, not treated as empty.
        {
                image im;
                im.abbrev = build_abbrev();
                emit_expr_cu(im, 8, vector<u8>{0x9c},
                             vector<u8>{0x93, 0x01}); // DW_OP_piece 1
                im.open();
                die fn = *im.dw->compilation_units().at(0).root().begin();
                die var = *fn.begin();
                variable_snapshot s(empty_snapshot());
                variable_result r = inspect_variable(var, fn, 0x1010, 4, s);
                TASK_CHECK(r.status == variable_status::unsupported);
                TASK_CHECK(r.status != variable_status::unavailable);
        }
        return 0;
}

static int
errors_missing_context_then_recovery()
{
        image im;
        im.abbrev = build_abbrev();
        emit_expr_cu(im, 8, vector<u8>{0x9c}, vector<u8>{0x52}); // reg2
        im.open();
        die fn = *im.dw->compilation_units().at(0).root().begin();
        die var = *fn.begin();

        // Missing register.
        {
                variable_snapshot s(empty_snapshot());
                variable_result r = inspect_variable(var, fn, 0x1010, 4, s);
                TASK_CHECK(r.status == variable_status::missing_register);
        }

        // Missing CFA on a call_frame_cfa-dependent location.
        {
                image im2;
                im2.abbrev = build_abbrev();
                emit_expr_cu(im2, 8, vector<u8>{0x9c},
                             vector<u8>{0x91, 0x00}); // fbreg 0
                im2.open();
                die fn2 = *im2.dw->compilation_units().at(0).root().begin();
                die var2 = *fn2.begin();
                variable_snapshot s(empty_snapshot());
                variable_result r = inspect_variable(var2, fn2, 0x1010, 4, s);
                TASK_CHECK(r.status == variable_status::missing_cfa);

                // After the failure, supplying the CFA makes a normal
                // query on the same inputs succeed (no poisoned state).
                variable_snapshot s2(empty_snapshot());
                s2.set_cfa(0x7000);
                s2.add_memory_segment(0x7000,
                                      vector<u8>{0x2a, 0, 0, 0});
                variable_result r2 = inspect_variable(var2, fn2, 0x1010, 4, s2);
                TASK_CHECK(r2.status == variable_status::value);
                TASK_CHECK(r2.value == 42);
        }

        // Recovery after an unsupported query.
        {
                variable_snapshot s(empty_snapshot());
                s.set_register(2, 0x1234);
                variable_result r = inspect_variable(var, fn, 0x1010, 4, s);
                TASK_CHECK(r.status == variable_status::value);
                TASK_CHECK(r.value == 0x1234);
        }
        return 0;
}

static int
errors_snapshot_validation_and_endian()
{
        // Overlapping segments are rejected up front.
        {
                variable_snapshot s(empty_snapshot());
                s.add_memory_segment(0x1000, vector<u8>{1, 2, 3, 4});
                bool threw = false;
                try {
                        s.add_memory_segment(0x1002, vector<u8>{9, 9});
                } catch (const std::invalid_argument &) {
                        threw = true;
                }
                TASK_CHECK(threw);
        }

        // A segment that wraps the address space is rejected.
        {
                variable_snapshot s(empty_snapshot());
                bool threw = false;
                try {
                        s.add_memory_segment(~(taddr)0 - 0x10,
                                             vector<u8>(0x30, 0));
                } catch (const std::invalid_argument &) {
                        threw = true;
                }
                TASK_CHECK(threw);
        }

        // Endianness mismatch with the (little-endian) ELF.
        {
                image im;
                im.abbrev = build_abbrev();
                emit_expr_cu(im, 8, vector<u8>{0x9c}, vector<u8>{0x52});
                im.open();
                die fn = *im.dw->compilation_units().at(0).root().begin();
                die var = *fn.begin();
                variable_snapshot s(snapshot_endian::big);
                s.set_register(2, 1);
                variable_result r = inspect_variable(var, fn, 0x1010, 4, s);
                TASK_CHECK(r.status == variable_status::malformed);
                TASK_CHECK(r.error.find("byte order") != string::npos);
        }

        // An address+width that wraps is a malformed query.
        {
                image im;
                im.abbrev = build_abbrev();
                vector<u8> loc{0x03};
                put_fixed(loc, ~(taddr)0 - 2, 8);
                emit_expr_cu(im, 8, vector<u8>{0x9c}, loc);
                im.open();
                die fn = *im.dw->compilation_units().at(0).root().begin();
                die var = *fn.begin();
                variable_snapshot s(empty_snapshot());
                variable_result r = inspect_variable(var, fn, 0x1010, 8, s);
                TASK_CHECK(r.status == variable_status::malformed);
        }
        return 0;
}

//////////////////////////////////////////////////////////////////
// Registration
//

const task_case task_cases[] = {
        {"elf", "golden_snapshots", elf_golden_snapshots},

        {"locations", "base_change_gap_first_match",
         locations_base_change_gap_first_match},
        {"locations", "same_offset_different_sections",
         locations_same_offset_different_sections},
        {"locations", "cross_cu_abstract_origin",
         locations_cross_cu_abstract_origin},
        {"locations", "four_byte_cu_and_legacy_block",
         locations_four_byte_cu_and_legacy_block},

        {"context", "register_and_literal_ignore_memory",
         context_register_and_literal_ignore_memory},
        {"context", "negative_fbreg_register_frame_base",
         context_negative_fbreg_and_register_frame_base},
        {"context", "literal_frame_base_not_dereferenced",
         context_literal_frame_base_not_dereferenced},
        {"context", "cross_segment_and_breg",
         context_cross_segment_and_breg},
        {"context", "result_survives_released_inputs",
         context_result_survives_released_inputs},

        {"errors", "truncated_and_bad_ranges",
         errors_truncated_and_bad_ranges},
        {"errors", "recursive_missing_unsupported",
         errors_recursive_missing_unsupported},
        {"errors", "missing_context_then_recovery",
         errors_missing_context_then_recovery},
        {"errors", "snapshot_validation_and_endian",
         errors_snapshot_validation_and_endian},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
