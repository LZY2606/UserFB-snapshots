// Contract tests for dwarf::at_location(): legacy .debug_loc location
// lists, cross-CU attribute ownership, DW_OP_fbreg frame bases, and
// structural error handling.  All DWARF sections are synthesized in
// memory; no compiler-generated DWARF is required.

#include "task_support/check.h"

#include "dwarf++.hh"

#include <cassert>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace dwarf;

// Inside the namespace the bare name "dwarf" collides with both the
// namespace and the dwarf::dwarf class; alias the class explicitly.
typedef ::dwarf::dwarf dwarf_file;

namespace {

typedef std::vector<unsigned char> bytes;

// ----------------------------------------------------------------
// Integer encoders (little endian)
//

void put_uleb(bytes &b, uint64_t v)
{
        do {
                uint8_t byte = v & 0x7f;
                v >>= 7;
                if (v)
                        byte |= 0x80;
                b.push_back(byte);
        } while (v);
}

void put_sleb(bytes &b, int64_t v)
{
        bool more = true;
        while (more) {
                uint8_t byte = v & 0x7f;
                v >>= 7;
                if ((v == 0 && !(byte & 0x40)) ||
                    (v == -1 && (byte & 0x40)))
                        more = false;
                else
                        byte |= 0x80;
                b.push_back(byte);
        }
}

void put_u16(bytes &b, uint16_t v)
{
        b.push_back(v & 0xff);
        b.push_back((v >> 8) & 0xff);
}

void put_u32(bytes &b, uint32_t v)
{
        for (int i = 0; i < 4; i++)
                b.push_back((v >> (8 * i)) & 0xff);
}

void put_n(bytes &b, uint64_t v, unsigned n)
{
        for (unsigned i = 0; i < n; i++)
                b.push_back((v >> (8 * i)) & 0xff);
}

uint64_t max_address(unsigned addr_size)
{
        return addr_size >= 8 ? ~(uint64_t)0 : ((1ULL << (8 * addr_size)) - 1);
}

// ----------------------------------------------------------------
// DWARF expression fragments
//

bytes expr_reg(unsigned regno)
{
        // DW_OP_reg0..31
        return { (unsigned char)(0x50 + regno) };
}

bytes expr_regx(unsigned regno)
{
        bytes e = { 0x90 };
        put_uleb(e, regno);
        return e;
}

bytes expr_fbreg(int64_t off)
{
        bytes e = { 0x91 };
        put_sleb(e, off);
        return e;
}

bytes expr_breg(unsigned regno, int64_t off)
{
        bytes e = { (unsigned char)(0x70 + regno) };
        put_sleb(e, off);
        return e;
}

bytes expr_lit_stackvalue(uint64_t lit)
{
        // DW_OP_litN ; DW_OP_stack_value
        return { (unsigned char)(0x30 + lit), 0x9f };
}

bytes expr_addr(uint64_t addr, unsigned addr_size)
{
        bytes e = { 0x03 };
        put_n(e, addr, addr_size);
        return e;
}

// ----------------------------------------------------------------
// .debug_abbrev builder
//

struct attr_spec { DW_AT at; DW_FORM form; };

struct abbrev_section
{
        bytes data;
        uint64_t next_code;

        section_offset begin_table()
        {
                next_code = 1;
                return (section_offset)data.size();
        }

        uint64_t add(DW_TAG tag, bool kids, std::vector<attr_spec> attrs)
        {
                uint64_t code = next_code++;
                put_uleb(data, code);
                put_uleb(data, (uint64_t)tag);
                data.push_back(kids ? 1 : 0);
                for (auto &a : attrs) {
                        put_uleb(data, (uint64_t)a.at);
                        put_uleb(data, (uint64_t)a.form);
                }
                put_uleb(data, 0);
                put_uleb(data, 0);
                return code;
        }

        void end_table() { data.push_back(0); }
};

// ----------------------------------------------------------------
// Compilation unit builder
//

bytes build_cu(unsigned addr_size, section_offset abbrev_off, const bytes &body)
{
        bytes cu;
        uint32_t unit_length = 2 + 4 + 1 + (uint32_t)body.size();
        put_u32(cu, unit_length);
        put_u16(cu, 4);                              // version
        put_u32(cu, (uint32_t)abbrev_off);          // .debug_abbrev offset
        cu.push_back((unsigned char)addr_size);
        cu.insert(cu.end(), body.begin(), body.end());
        return cu;
}

void emit_die(bytes &body, uint64_t code, const bytes &attr_bytes = {})
{
        put_uleb(body, code);
        body.insert(body.end(), attr_bytes.begin(), attr_bytes.end());
}

void die_terminator(bytes &body) { body.push_back(0); }

bytes f_addr(uint64_t v, unsigned addr_size)
{
        bytes b;
        put_n(b, v, addr_size);
        return b;
}

bytes f_sec_offset(uint32_t off)
{
        bytes b;
        put_u32(b, off);
        return b;
}

bytes f_ref_addr(uint32_t off) { return f_sec_offset(off); }

bytes f_exprloc(const bytes &e)
{
        bytes b;
        put_uleb(b, e.size());
        b.insert(b.end(), e.begin(), e.end());
        return b;
}

bytes f_block2(const bytes &e)
{
        bytes b;
        put_u16(b, (uint16_t)e.size());
        b.insert(b.end(), e.begin(), e.end());
        return b;
}

// ----------------------------------------------------------------
// .debug_loc builder
//

struct loc_builder
{
        bytes data;
        unsigned addr_size;

        explicit loc_builder(unsigned addr_size) : addr_size(addr_size) { }

        section_offset offset() const { return (section_offset)data.size(); }

        void pair(uint64_t a, uint64_t b)
        {
                put_n(data, a, addr_size);
                put_n(data, b, addr_size);
        }

        void base_address(uint64_t addr) { pair(max_address(addr_size), addr); }
        void end_list() { pair(0, 0); }

        void entry(uint64_t low, uint64_t high, const bytes &expression)
        {
                pair(low, high);
                put_u16(data, (uint16_t)expression.size());
                data.insert(data.end(), expression.begin(), expression.end());
        }
};

// ----------------------------------------------------------------
// In-memory DWARF loader
//

struct mem_loader : loader
{
        std::shared_ptr<bytes> info;
        std::shared_ptr<bytes> abbrev;
        std::shared_ptr<bytes> loc;

        const void *load(section_type t, size_t *n) override
        {
                std::shared_ptr<bytes> v;
                if (t == section_type::info)
                        v = info;
                else if (t == section_type::abbrev)
                        v = abbrev;
                else if (t == section_type::loc)
                        v = loc;
                else
                        return nullptr;
                if (!v)
                        return nullptr;
                *n = v->size();
                return v->data();
        }
};

std::shared_ptr<dwarf_file>
make_dwarf(std::shared_ptr<bytes> info, std::shared_ptr<bytes> abbrev,
           std::shared_ptr<bytes> loc)
{
        auto l = std::make_shared<mem_loader>();
        l->info = info;
        l->abbrev = abbrev;
        l->loc = loc;
        return std::make_shared<dwarf_file>(l);
}

// Iterator DIEs are temporaries; copy the selected one out.
die nth_child(const die &parent, unsigned n)
{
        unsigned i = 0;
        for (auto c : parent) {
                if (i == n)
                        return c;
                ++i;
        }
        throw std::runtime_error("child not found");
}

// Expression context with observable register reads.
struct test_context : expr_context
{
        std::map<unsigned, uint64_t> regs;
        unsigned reg_calls = 0;
        int tracked_reg = -1;
        unsigned tracked_reads = 0;

        taddr reg(unsigned regnum) override
        {
                ++reg_calls;
                if ((int)regnum == tracked_reg)
                        ++tracked_reads;
                auto it = regs.find(regnum);
                if (it == regs.end())
                        throw expr_error("register not provided by snapshot");
                return it->second;
        }
};

// ----------------------------------------------------------------
// Group: select
//

// One loclist exercising base-address switching, a zero-length
// range, an empty expression, and gaps; queried in reverse PC order.
int test_select_list()
{
        const unsigned AS = 4;

        abbrev_section ab;
        auto ab_off = ab.begin_table();
        auto cu_code = ab.add(DW_TAG::compile_unit, true,
                              {{DW_AT::low_pc, DW_FORM::addr}});
        auto sub_code = ab.add(DW_TAG::subprogram, true, {});
        auto var_loc_code = ab.add(DW_TAG::variable, false,
                                   {{DW_AT::location, DW_FORM::sec_offset}});
        auto var_expr_code = ab.add(DW_TAG::variable, false,
                                    {{DW_AT::location, DW_FORM::exprloc}});
        auto var_block_code = ab.add(DW_TAG::variable, false,
                                     {{DW_AT::location, DW_FORM::block2}});
        auto var_none_code = ab.add(DW_TAG::variable, false, {});
        ab.end_table();

        loc_builder loc(AS);
        section_offset list_off = loc.offset();
        loc.base_address(0x4000);
        loc.entry(0x10, 0x20, expr_reg(5));           // [0x4010,0x4020)
        loc.entry(0x20, 0x20, expr_reg(6));           // zero-length range
        loc.entry(0x30, 0x40, bytes());               // empty expression
        loc.base_address(0x8000);
        loc.entry(0x00, 0x10, expr_reg(7));           // [0x8000,0x8010)
        loc.end_list();

        bytes body;
        emit_die(body, cu_code, f_addr(0x1000, AS));       // CU low_pc
        emit_die(body, sub_code);                          // function (no frame base)
        {
                bytes attrs = f_sec_offset((uint32_t)list_off);
                emit_die(body, var_loc_code, attrs);       // variable: loclist
        }
        emit_die(body, var_expr_code, f_exprloc(expr_reg(3)));
        emit_die(body, var_block_code, f_block2(expr_reg(9)));
        emit_die(body, var_none_code);
        die_terminator(body);                         // end subprogram children
        die_terminator(body);                         // end CU children

        auto info = std::make_shared<bytes>(build_cu(AS, ab_off, body));
        auto abb = std::make_shared<bytes>(ab.data);
        auto locb = std::make_shared<bytes>(loc.data);
        auto d = make_dwarf(info, abb, locb);

        const die &root = d->compilation_units().at(0).root();
        const die &func = nth_child(root, 0);
        const die &var_list = nth_child(func, 0);
        const die &var_expr = nth_child(func, 1);
        const die &var_block = nth_child(func, 2);
        const die &var_none = nth_child(func, 3);

        test_context ctx;

        // Reverse-order queries against the same attribute.
        expr_result r;

        r = at_location(var_list, func, 0x8005, &ctx);
        TASK_CHECK(r.location_type == expr_result::type::reg && r.value == 7);

        r = at_location(var_list, func, 0x4035, &ctx);
        TASK_CHECK(r.location_type == expr_result::type::empty);

        r = at_location(var_list, func, 0x4025, &ctx);
        TASK_CHECK(r.location_type == expr_result::type::empty);

        // High boundary of the first range is excluded; the
        // zero-length entry can never match.
        r = at_location(var_list, func, 0x4020, &ctx);
        TASK_CHECK(r.location_type == expr_result::type::empty);

        // Low boundary is included; first matching entry wins.
        r = at_location(var_list, func, 0x4010, &ctx);
        TASK_CHECK(r.location_type == expr_result::type::reg && r.value == 5);
        r = at_location(var_list, func, 0x4015, &ctx);
        TASK_CHECK(r.location_type == expr_result::type::reg && r.value == 5);

        // Re-query an earlier PC to prove nothing is cached.
        r = at_location(var_list, func, 0x8005, &ctx);
        TASK_CHECK(r.location_type == expr_result::type::reg && r.value == 7);

        // Direct exprloc and legacy block locations, plus a variable
        // with no location at all.  None need a frame base.
        r = at_location(var_expr, func, 0x4015, &ctx);
        TASK_CHECK(r.location_type == expr_result::type::reg && r.value == 3);
        r = at_location(var_block, func, 0x4015, &ctx);
        TASK_CHECK(r.location_type == expr_result::type::reg && r.value == 9);
        r = at_location(var_none, func, 0x4015, &ctx);
        TASK_CHECK(r.location_type == expr_result::type::empty);

        // Register locations never touch the register snapshot.
        TASK_CHECK(ctx.reg_calls == 0);
        return 0;
}

// A direct DW_OP_addr location still resolves through the CU context.
int test_select_direct_addr()
{
        const unsigned AS = 4;
        abbrev_section ab;
        auto ab_off = ab.begin_table();
        auto cu_code = ab.add(DW_TAG::compile_unit, true,
                              {{DW_AT::low_pc, DW_FORM::addr}});
        auto sub_code = ab.add(DW_TAG::subprogram, true, {});
        auto var_code = ab.add(DW_TAG::variable, false,
                               {{DW_AT::location, DW_FORM::exprloc}});
        ab.end_table();

        bytes body;
        emit_die(body, cu_code, f_addr(0, AS));
        emit_die(body, sub_code);
        emit_die(body, var_code, f_exprloc(expr_addr(0xdeadbeef, AS)));
        die_terminator(body);
        die_terminator(body);

        auto d = make_dwarf(std::make_shared<bytes>(build_cu(AS, ab_off, body)),
                            std::make_shared<bytes>(ab.data), nullptr);
        const die &root = d->compilation_units().at(0).root();
        const die &func = nth_child(root, 0);
        const die &var = nth_child(func, 0);

        test_context ctx;
        expr_result r = at_location(var, func, 0x1234, &ctx);
        TASK_CHECK(r.location_type == expr_result::type::address);
        TASK_CHECK(r.value == 0xdeadbeef);
        return 0;
}

// When two entries cover the same PC and the first has an empty
// expression, the result is empty: scanning must not fall through to
// the later overlapping entry.
int test_select_empty_first_overlap()
{
        const unsigned AS = 4;
        abbrev_section ab;
        auto ab_off = ab.begin_table();
        auto cu_code = ab.add(DW_TAG::compile_unit, true,
                              {{DW_AT::low_pc, DW_FORM::addr}});
        auto sub_code = ab.add(DW_TAG::subprogram, true, {});
        auto var_code = ab.add(DW_TAG::variable, false,
                               {{DW_AT::location, DW_FORM::sec_offset}});
        ab.end_table();

        loc_builder loc(AS);
        loc.entry(0x1000, 0x1020, bytes());           // empty, broad range
        loc.entry(0x1010, 0x1020, expr_reg(7));       // overlaps, later
        loc.end_list();

        bytes body;
        emit_die(body, cu_code, f_addr(0, AS));
        emit_die(body, sub_code);
        emit_die(body, var_code, f_sec_offset(0));
        die_terminator(body);
        die_terminator(body);

        auto d = make_dwarf(std::make_shared<bytes>(build_cu(AS, ab_off, body)),
                            std::make_shared<bytes>(ab.data),
                            std::make_shared<bytes>(loc.data));
        const die &root = d->compilation_units().at(0).root();
        const die &func = nth_child(root, 0);
        const die &var = nth_child(func, 0);

        test_context ctx;
        // 0x1015 is in both ranges; the first (empty) wins.
        expr_result r = at_location(var, func, 0x1015, &ctx);
        TASK_CHECK(r.location_type == expr_result::type::empty);
        // Just before the second entry, still the empty first range.
        r = at_location(var, func, 0x1005, &ctx);
        TASK_CHECK(r.location_type == expr_result::type::empty);
        // Past both ranges there is no location either.
        r = at_location(var, func, 0x1025, &ctx);
        TASK_CHECK(r.location_type == expr_result::type::empty);
        return 0;
}

// ----------------------------------------------------------------
// Group: ownership
//

// A loclist attribute inherited across CUs via DW_AT_specification
// must be decoded with the owning CU's 2-byte address size.
int test_ownership_cross_cu()
{
        abbrev_section ab;

        // CU B (owner of the concrete variable, addr_size 2).
        auto ab_b = ab.begin_table();
        auto b_cu = ab.add(DW_TAG::compile_unit, true,
                           {{DW_AT::low_pc, DW_FORM::addr}});
        auto b_sub = ab.add(DW_TAG::subprogram, true, {});
        auto b_var = ab.add(DW_TAG::variable, false,
                            {{DW_AT::location, DW_FORM::sec_offset}});
        ab.end_table();

        // CU A (specification DIE, addr_size 4).
        auto ab_a = ab.begin_table();
        auto a_cu = ab.add(DW_TAG::compile_unit, true,
                           {{DW_AT::low_pc, DW_FORM::addr}});
        auto a_sub = ab.add(DW_TAG::subprogram, true, {});
        auto a_spec = ab.add(DW_TAG::variable, false,
                             {{DW_AT::specification, DW_FORM::ref_addr}});
        ab.end_table();

        // 2-byte-address location list owned by CU B.  If decoded as
        // 4-byte addresses these bytes cannot yield this entry.
        loc_builder loc(2);
        section_offset list_off = loc.offset();
        loc.base_address(0x2000);
        loc.entry(0x10, 0x20, expr_regx(8));          // [0x2010,0x2020)
        loc.end_list();

        const unsigned AS_B = 2, AS_A = 4;

        bytes body_b;
        emit_die(body_b, b_cu, f_addr(0x2000, AS_B));
        emit_die(body_b, b_sub);
        section_offset var_off_in_b = body_b.size();
        emit_die(body_b, b_var, f_sec_offset((uint32_t)list_off));
        die_terminator(body_b);
        die_terminator(body_b);

        static const unsigned HEADER = 4 + 2 + 4 + 1;
        uint32_t var_global = HEADER + (uint32_t)var_off_in_b; // CU B first

        bytes body_a;
        emit_die(body_a, a_cu, f_addr(0x1000, AS_A));
        emit_die(body_a, a_sub);
        emit_die(body_a, a_spec, f_ref_addr(var_global));
        die_terminator(body_a);
        die_terminator(body_a);

        bytes info = build_cu(AS_B, ab_b, body_b);
        bytes cu_a = build_cu(AS_A, ab_a, body_a);
        info.insert(info.end(), cu_a.begin(), cu_a.end());

        auto d = make_dwarf(std::make_shared<bytes>(info),
                            std::make_shared<bytes>(ab.data),
                            std::make_shared<bytes>(loc.data));

        TASK_CHECK(d->compilation_units().size() == 2);
        const die &root_a = d->compilation_units().at(1).root();
        const die &func_a = nth_child(root_a, 0);
        const die &spec_var = nth_child(func_a, 0);
        TASK_CHECK(!spec_var.has(DW_AT::location));

        test_context ctx;
        expr_result r = at_location(spec_var, func_a, 0x2015, &ctx);
        TASK_CHECK(r.location_type == expr_result::type::reg && r.value == 8);

        // A PC outside the 2-byte-decoded range is an empty result.
        r = at_location(spec_var, func_a, 0x9999, &ctx);
        TASK_CHECK(r.location_type == expr_result::type::empty);
        return 0;
}

// Two independent DWARF files use the same numeric .debug_loc offset
// for different expressions and different address widths; the bytes
// at that same offset in .debug_info are unrelated DIE data.
int test_ownership_same_offset_two_sections()
{
        auto build_one = [](unsigned as, unsigned regno, uint64_t base)
                -> std::shared_ptr<dwarf_file>
        {
                abbrev_section ab;
                auto ab_off = ab.begin_table();
                auto cu_code = ab.add(DW_TAG::compile_unit, true,
                                      {{DW_AT::low_pc, DW_FORM::addr}});
                auto sub_code = ab.add(DW_TAG::subprogram, true, {});
                auto var_code = ab.add(DW_TAG::variable, false,
                                       {{DW_AT::location,
                                         DW_FORM::sec_offset}});
                ab.end_table();

                loc_builder loc(as);
                // Both lists start at the same numeric offset, yet
                // encode different expressions for different widths.
                assert(loc.offset() == 0);
                loc.base_address(base);
                loc.entry(0x10, 0x20, expr_reg(regno));
                loc.end_list();

                bytes body;
                emit_die(body, cu_code, f_addr(base, as));
                emit_die(body, sub_code);
                emit_die(body, var_code, f_sec_offset(0));
                die_terminator(body);
                die_terminator(body);

                return make_dwarf(
                        std::make_shared<bytes>(build_cu(as, ab_off, body)),
                        std::make_shared<bytes>(ab.data),
                        std::make_shared<bytes>(loc.data));
        };

        auto d4 = build_one(4, 5, 0x4000);
        auto d2 = build_one(2, 9, 0x2000);

        const die &root4 = d4->compilation_units().at(0).root();
        const die &func4 = nth_child(root4, 0);
        const die &var4 = nth_child(func4, 0);
        const die &root2 = d2->compilation_units().at(0).root();
        const die &func2 = nth_child(root2, 0);
        const die &var2 = nth_child(func2, 0);

        test_context ctx;
        expr_result r4 = at_location(var4, func4, 0x4015, &ctx);
        TASK_CHECK(r4.location_type == expr_result::type::reg && r4.value == 5);
        expr_result r2 = at_location(var2, func2, 0x2015, &ctx);
        TASK_CHECK(r2.location_type == expr_result::type::reg && r2.value == 9);
        return 0;
}

// ----------------------------------------------------------------
// Group: frame
//

// Frame base register changes with PC; the variable uses a negative
// DW_OP_fbreg offset and the register callback is counted.
int test_frame_register_by_pc()
{
        const unsigned AS = 4;
        abbrev_section ab;
        auto ab_off = ab.begin_table();
        auto cu_code = ab.add(DW_TAG::compile_unit, true,
                              {{DW_AT::low_pc, DW_FORM::addr}});
        auto sub_code = ab.add(DW_TAG::subprogram, true,
                               {{DW_AT::frame_base, DW_FORM::sec_offset}});
        auto var_code = ab.add(DW_TAG::variable, false,
                               {{DW_AT::location, DW_FORM::sec_offset}});
        ab.end_table();

        loc_builder loc(AS);
        section_offset fb_list = loc.offset();
        loc.entry(0x1000, 0x1010, expr_reg(4));        // frame base = reg 4
        loc.entry(0x1010, 0x1020, expr_reg(5));        // frame base = reg 5
        loc.end_list();
        section_offset var_list = loc.offset();
        loc.entry(0x1000, 0x1020, expr_fbreg(-8));     // fbreg - 8
        loc.end_list();

        bytes body;
        emit_die(body, cu_code, f_addr(0, AS));
        bytes sub_attrs = f_sec_offset((uint32_t)fb_list);
        emit_die(body, sub_code, sub_attrs);
        emit_die(body, var_code, f_sec_offset((uint32_t)var_list));
        die_terminator(body);
        die_terminator(body);

        auto d = make_dwarf(std::make_shared<bytes>(build_cu(AS, ab_off, body)),
                            std::make_shared<bytes>(ab.data),
                            std::make_shared<bytes>(loc.data));
        const die &root = d->compilation_units().at(0).root();
        const die &func = nth_child(root, 0);
        const die &var = nth_child(func, 0);

        test_context ctx;
        ctx.regs[4] = 0x100;
        ctx.regs[5] = 0x200;

        ctx.tracked_reg = 4;
        expr_result r = at_location(var, func, 0x1005, &ctx);
        TASK_CHECK(r.location_type == expr_result::type::address);
        TASK_CHECK(r.value == 0x100 - 8);              // 0xf8, no dereference
        TASK_CHECK(ctx.reg_calls == 1);
        TASK_CHECK(ctx.tracked_reads == 1);

        ctx.reg_calls = 0;
        ctx.tracked_reg = 5;
        ctx.tracked_reads = 0;
        r = at_location(var, func, 0x1015, &ctx);
        TASK_CHECK(r.location_type == expr_result::type::address);
        TASK_CHECK(r.value == 0x200 - 8);              // 0x1f8
        TASK_CHECK(ctx.reg_calls == 1);
        TASK_CHECK(ctx.tracked_reads == 1);
        return 0;
}

// A literal frame base is used as a numeric address directly, with no
// register read and no extra dereference.
int test_frame_literal()
{
        const unsigned AS = 4;
        abbrev_section ab;
        auto ab_off = ab.begin_table();
        auto cu_code = ab.add(DW_TAG::compile_unit, true, {});
        auto sub_code = ab.add(DW_TAG::subprogram, true,
                               {{DW_AT::frame_base, DW_FORM::exprloc}});
        auto var_code = ab.add(DW_TAG::variable, false,
                               {{DW_AT::location, DW_FORM::exprloc}});
        ab.end_table();

        bytes body;
        emit_die(body, cu_code);
        // DW_OP_lit7 ; DW_OP_stack_value -> literal 7
        emit_die(body, sub_code, f_exprloc(expr_lit_stackvalue(7)));
        emit_die(body, var_code, f_exprloc(expr_fbreg(3)));
        die_terminator(body);
        die_terminator(body);

        auto d = make_dwarf(std::make_shared<bytes>(build_cu(AS, ab_off, body)),
                            std::make_shared<bytes>(ab.data), nullptr);
        const die &root = d->compilation_units().at(0).root();
        const die &func = nth_child(root, 0);
        const die &var = nth_child(func, 0);

        test_context ctx;
        expr_result r = at_location(var, func, 0x1, &ctx);
        TASK_CHECK(r.location_type == expr_result::type::address);
        TASK_CHECK(r.value == 10);
        TASK_CHECK(ctx.reg_calls == 0);                // literal needs no register
        return 0;
}

// A register variable does not require any frame base, even though
// the function provides none.
int test_frame_not_needed_for_reg()
{
        const unsigned AS = 4;
        abbrev_section ab;
        auto ab_off = ab.begin_table();
        auto cu_code = ab.add(DW_TAG::compile_unit, true, {});
        auto sub_code = ab.add(DW_TAG::subprogram, true, {});
        auto var_code = ab.add(DW_TAG::variable, false,
                               {{DW_AT::location, DW_FORM::exprloc}});
        ab.end_table();

        bytes body;
        emit_die(body, cu_code);
        emit_die(body, sub_code);
        emit_die(body, var_code, f_exprloc(expr_breg(2, 4)));
        die_terminator(body);
        die_terminator(body);

        auto d = make_dwarf(std::make_shared<bytes>(build_cu(AS, ab_off, body)),
                            std::make_shared<bytes>(ab.data), nullptr);
        const die &root = d->compilation_units().at(0).root();
        const die &func = nth_child(root, 0);
        const die &var = nth_child(func, 0);

        test_context ctx;
        ctx.regs[2] = 0x50;
        expr_result r = at_location(var, func, 0x1, &ctx);
        TASK_CHECK(r.location_type == expr_result::type::address);
        TASK_CHECK(r.value == 0x54);
        TASK_CHECK(ctx.reg_calls == 1);
        return 0;
}

// fbreg without a frame base is an error, not a crash or fake address.
int test_frame_missing()
{
        const unsigned AS = 4;
        abbrev_section ab;
        auto ab_off = ab.begin_table();
        auto cu_code = ab.add(DW_TAG::compile_unit, true, {});
        auto sub_code = ab.add(DW_TAG::subprogram, true, {});
        auto var_code = ab.add(DW_TAG::variable, false,
                               {{DW_AT::location, DW_FORM::exprloc}});
        ab.end_table();

        bytes body;
        emit_die(body, cu_code);
        emit_die(body, sub_code);
        emit_die(body, var_code, f_exprloc(expr_fbreg(0)));
        die_terminator(body);
        die_terminator(body);

        auto d = make_dwarf(std::make_shared<bytes>(build_cu(AS, ab_off, body)),
                            std::make_shared<bytes>(ab.data), nullptr);
        const die &root = d->compilation_units().at(0).root();
        const die &func = nth_child(root, 0);
        const die &var = nth_child(func, 0);

        test_context ctx;
        bool caught = false;
        try {
                at_location(var, func, 0x1, &ctx);
        } catch (const expr_error &) {
                caught = true;
        }
        TASK_CHECK(caught);
        return 0;
}

// A frame base with an unsuitable result type is an error.  An empty
// frame-base expression evaluates to expr_result::type::empty, which
// cannot serve as a frame base address.
int test_frame_unsuitable_type()
{
        const unsigned AS = 4;
        abbrev_section ab;
        auto ab_off = ab.begin_table();
        auto cu_code = ab.add(DW_TAG::compile_unit, true, {});
        auto sub_code = ab.add(DW_TAG::subprogram, true,
                               {{DW_AT::frame_base, DW_FORM::exprloc}});
        auto var_code = ab.add(DW_TAG::variable, false,
                               {{DW_AT::location, DW_FORM::exprloc}});
        ab.end_table();

        bytes body;
        emit_die(body, cu_code);
        // Zero-length frame-base expression -> expr_result::type::empty.
        emit_die(body, sub_code, f_exprloc(bytes()));
        emit_die(body, var_code, f_exprloc(expr_fbreg(0)));
        die_terminator(body);
        die_terminator(body);

        auto d = make_dwarf(std::make_shared<bytes>(build_cu(AS, ab_off, body)),
                            std::make_shared<bytes>(ab.data), nullptr);
        const die &root = d->compilation_units().at(0).root();
        const die &func = nth_child(root, 0);
        const die &var = nth_child(func, 0);

        test_context ctx;
        bool caught = false;
        try {
                at_location(var, func, 0x1, &ctx);
        } catch (const expr_error &e) {
                caught = std::string(e.what()).find("unsuitable") !=
                         std::string::npos;
        }
        TASK_CHECK(caught);
        return 0;
}

// ----------------------------------------------------------------
// Group: errors
//

// Shared good CU used by the structural-error cases.
struct error_fixture
{
        abbrev_section ab;
        section_offset ab_off;
        uint64_t cu_code, sub_code, var_code;
        static const unsigned AS = 4;

        error_fixture()
        {
                ab_off = ab.begin_table();
                cu_code = ab.add(DW_TAG::compile_unit, true,
                                 {{DW_AT::low_pc, DW_FORM::addr}});
                sub_code = ab.add(DW_TAG::subprogram, true, {});
                var_code = ab.add(DW_TAG::variable, false,
                                  {{DW_AT::location, DW_FORM::sec_offset}});
                ab.end_table();
        }

        std::shared_ptr<dwarf_file> build(std::shared_ptr<bytes> loc,
                                     uint32_t list_off)
        {
                bytes body;
                emit_die(body, cu_code, f_addr(0x1000, AS));
                emit_die(body, sub_code);
                emit_die(body, var_code, f_sec_offset(list_off));
                die_terminator(body);
                die_terminator(body);
                return make_dwarf(
                        std::make_shared<bytes>(build_cu(AS, ab_off, body)),
                        std::make_shared<bytes>(ab.data), loc);
        }
};

bool throws_loc_format_error(std::shared_ptr<dwarf_file> d, taddr pc)
{
        const die &root = d->compilation_units().at(0).root();
        const die &func = nth_child(root, 0);
        const die &var = nth_child(func, 0);
        test_context ctx;
        try {
                at_location(var, func, pc, &ctx);
        } catch (const format_error &e) {
                std::string w = e.what();
                return w.find(".debug_loc") != std::string::npos &&
                       w.find("offset") != std::string::npos;
        }
        return false;
}

int test_errors_missing_section()
{
        error_fixture fx;
        // loc == nullptr makes the loader report the section absent.
        auto d = fx.build(nullptr, 0);
        TASK_CHECK(throws_loc_format_error(d, 0x1005));
        return 0;
}

int test_errors_bad_tail_after_match()
{
        error_fixture fx;
        loc_builder loc(fx.AS);
        // Relative to CU low_pc 0x1000: absolute [0x1000,0x1010), so the
        // first entry really does match the queried PC (empty expression).
        loc.entry(0x0000, 0x0010, bytes());
        loc.data.push_back(0x12);                     // truncated next entry
        loc.data.push_back(0x34);
        loc.data.push_back(0x56);
        auto d = fx.build(std::make_shared<bytes>(loc.data), 0);
        // PC matches the good first entry, but the bad tail must still
        // be validated and raise.
        TASK_CHECK(throws_loc_format_error(d, 0x1005));
        return 0;
}

int test_errors_truncated_expression()
{
        error_fixture fx;
        loc_builder loc(fx.AS);
        loc.pair(0x0000, 0x0010);                     // absolute [0x1000,0x1010)
        put_u16(loc.data, 5);                         // claims 5 expression bytes
        loc.data.push_back(0x96);                     // only 2 present
        loc.data.push_back(0x96);
        auto d = fx.build(std::make_shared<bytes>(loc.data), 0);
        TASK_CHECK(throws_loc_format_error(d, 0x1005));
        return 0;
}

int test_errors_unterminated()
{
        error_fixture fx;
        loc_builder loc(fx.AS);
        loc.entry(0x0000, 0x0010, expr_reg(3));
        loc.entry(0x0020, 0x0030, expr_reg(4));
        // No (0,0) terminator.
        auto d = fx.build(std::make_shared<bytes>(loc.data), 0);
        TASK_CHECK(throws_loc_format_error(d, 0x1005));
        return 0;
}

int test_errors_reversed_range()
{
        error_fixture fx;
        loc_builder loc(fx.AS);
        loc.entry(0x1030, 0x1020, expr_reg(3));       // low > high
        loc.end_list();
        auto d = fx.build(std::make_shared<bytes>(loc.data), 0);
        TASK_CHECK(throws_loc_format_error(d, 0x1005));
        return 0;
}

int test_errors_base_wrap()
{
        error_fixture fx;
        loc_builder loc(fx.AS);
        loc.base_address(0xfffffff0);
        loc.entry(0x20, 0x30, expr_reg(3));           // base + low wraps
        loc.end_list();
        auto d = fx.build(std::make_shared<bytes>(loc.data), 0);
        TASK_CHECK(throws_loc_format_error(d, 0x1005));
        return 0;
}

// A complete, terminated list followed by garbage must not let the
// scan cross the terminator.
int test_errors_stops_at_terminator()
{
        error_fixture fx;
        loc_builder loc(fx.AS);
        // Relative to the CU low_pc (0x1000): absolute [0x1000,0x1010).
        loc.entry(0x0000, 0x0010, expr_reg(6));
        loc.end_list();
        loc.data.push_back(0xff);                     // another, broken list
        loc.data.push_back(0xff);
        auto d = fx.build(std::make_shared<bytes>(loc.data), 0);

        const die &root = d->compilation_units().at(0).root();
        const die &func = nth_child(root, 0);
        const die &var = nth_child(func, 0);
        test_context ctx;
        expr_result r = at_location(var, func, 0x1005, &ctx);
        TASK_CHECK(r.location_type == expr_result::type::reg && r.value == 6);
        return 0;
}

// A frame base that itself uses DW_OP_fbreg is a circular dependency.
// After the error, a normal query on the same DWARF object still works.
int test_errors_frame_cycle()
{
        const unsigned AS = 4;
        abbrev_section ab;
        auto ab_off = ab.begin_table();
        auto cu_code = ab.add(DW_TAG::compile_unit, true, {});
        auto sub_code = ab.add(DW_TAG::subprogram, true,
                               {{DW_AT::frame_base, DW_FORM::exprloc}});
        auto var_code = ab.add(DW_TAG::variable, false,
                               {{DW_AT::location, DW_FORM::exprloc}});
        auto var_reg_code = ab.add(DW_TAG::variable, false,
                                   {{DW_AT::location, DW_FORM::exprloc}});
        ab.end_table();

        bytes body;
        emit_die(body, cu_code);
        emit_die(body, sub_code, f_exprloc(expr_fbreg(0)));
        emit_die(body, var_code, f_exprloc(expr_fbreg(0)));
        emit_die(body, var_reg_code, f_exprloc(expr_reg(2))); // unaffected var
        die_terminator(body);
        die_terminator(body);

        auto d = make_dwarf(std::make_shared<bytes>(build_cu(AS, ab_off, body)),
                            std::make_shared<bytes>(ab.data), nullptr);
        const die &root = d->compilation_units().at(0).root();
        const die &func = nth_child(root, 0);
        const die &var = nth_child(func, 0);
        const die &var_reg = nth_child(func, 1);

        test_context ctx;
        bool caught = false;
        try {
                at_location(var, func, 0x1, &ctx);
        } catch (const expr_error &e) {
                caught = std::string(e.what()).find("circular") !=
                         std::string::npos;
        }
        TASK_CHECK(caught);

        // The failed frame resolution left no cached state: the
        // register variable and even the original fbreg variable's
        // frame failure are independently reproducible.
        expr_result r = at_location(var_reg, func, 0x1, &ctx);
        TASK_CHECK(r.location_type == expr_result::type::reg && r.value == 2);

        bool caught_again = false;
        try {
                at_location(var, func, 0x1, &ctx);
        } catch (const expr_error &e) {
                caught_again = std::string(e.what()).find("circular") !=
                               std::string::npos;
        }
        TASK_CHECK(caught_again);
        return 0;
}

// After the error cases, a clean query still works (no cached state).
int test_errors_recover_normal()
{
        error_fixture fx;
        loc_builder loc(fx.AS);
        // Relative to CU low_pc 0x1000: absolute [0x1000,0x1010).
        loc.entry(0x0000, 0x0010, expr_reg(11));
        loc.end_list();
        auto d = fx.build(std::make_shared<bytes>(loc.data), 0);

        const die &root = d->compilation_units().at(0).root();
        const die &func = nth_child(root, 0);
        const die &var = nth_child(func, 0);
        test_context ctx;
        expr_result r = at_location(var, func, 0x1005, &ctx);
        TASK_CHECK(r.location_type == expr_result::type::reg && r.value == 11);
        r = at_location(var, func, 0x2000, &ctx);
        TASK_CHECK(r.location_type == expr_result::type::empty);
        return 0;
}

} // namespace

const struct task_case task_cases[] = {
        {"select", "loclist_base_switch_and_gaps", test_select_list},
        {"select", "direct_addr_expression", test_select_direct_addr},
        {"select", "empty_first_overlap_does_not_fall_through",
         test_select_empty_first_overlap},

        {"ownership", "cross_cu_specification_owns_location",
         test_ownership_cross_cu},
        {"ownership", "same_offset_different_sections",
         test_ownership_same_offset_two_sections},

        {"frame", "register_frame_base_changes_by_pc",
         test_frame_register_by_pc},
        {"frame", "literal_frame_base_used_directly", test_frame_literal},
        {"frame", "register_location_needs_no_frame_base",
         test_frame_not_needed_for_reg},
        {"frame", "missing_frame_base_is_error", test_frame_missing},
        {"frame", "unsuitable_frame_base_type_is_error",
         test_frame_unsuitable_type},

        {"errors", "missing_debug_loc_section", test_errors_missing_section},
        {"errors", "bad_tail_validated_even_when_first_entry_matches",
         test_errors_bad_tail_after_match},
        {"errors", "truncated_expression", test_errors_truncated_expression},
        {"errors", "unterminated_list", test_errors_unterminated},
        {"errors", "reversed_range", test_errors_reversed_range},
        {"errors", "base_address_wrap", test_errors_base_wrap},
        {"errors", "scan_stops_at_terminator", test_errors_stops_at_terminator},
        {"errors", "circular_frame_base", test_errors_frame_cycle},
        {"errors", "normal_query_after_errors", test_errors_recover_normal},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
