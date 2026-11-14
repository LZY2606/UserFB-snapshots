// Copyright (c) 2013 Austin T. Clements. All rights reserved.
// Use of this source code is governed by an MIT license
// that can be found in the LICENSE file.

#include "internal.hh"

using namespace std;

DWARFPP_BEGIN_NAMESPACE

//////////////////////////////////////////////////////////////////
// Legacy .debug_loc location lists (DWARF 2-4, section 2.6.2)
//

static format_error
loc_error(section_type st, section_offset entry_off, const string &msg)
{
        return format_error(string(elf::section_type_to_name(st)) +
                            ": " + msg + " at entry offset 0x" +
                            to_hex((uint64_t)entry_off));
}

// Fetch .debug_loc for a list, turning a missing section into an
// error that still names both the section and the list pointer.
static shared_ptr<section>
get_loc_section(const unit *u, section_offset list_off)
{
        try {
                return u->get_dwarf().get_section(section_type::loc);
        } catch (const format_error &) {
                throw format_error(
                        string(elf::section_type_to_name(section_type::loc)) +
                        " section missing for location list at offset 0x" +
                        to_hex((uint64_t)list_off));
        }
}

loclist_selection
select_loclist_entry(const std::shared_ptr<section> &sec,
                     section_offset list_off, unsigned addr_size,
                     taddr cu_low_pc, taddr pc, section_type sec_type)
{
        // Work on a slice beginning at the list pointer so cursor
        // offsets are relative to it; the owning unit's address size
        // (and the section's byte order) drive every decode.
        shared_ptr<section> sub = sec->slice(list_off, ~(section_length)0,
                                             format::unknown, addr_size);
        cursor cur(sub);

        const taddr addr_mask =
                addr_size >= sizeof(taddr) ? ~(taddr)0
                : (((taddr)1 << (8 * addr_size)) - 1);

        taddr base_addr = cu_low_pc;
        loclist_selection sel;
        sel.found = false;
        sel.empty_expr = false;
        sel.expr_offset = 0;
        sel.expr_len = 0;

        auto remaining = [&]() -> section_offset {
                if (cur.pos >= cur.sec->end)
                        return 0;
                return (section_offset)(cur.sec->end - cur.pos);
        };
        auto read_address = [&](section_offset entry_off) -> taddr {
                if (remaining() < addr_size)
                        throw loc_error(sec_type, list_off + entry_off,
                                        "truncated address pair");
                return cur.address();
        };

        while (true) {
                section_offset rel_entry = cur.get_section_offset();

                // Every entry (and the terminator) starts with a
                // pair of target addresses.
                if (remaining() < 2 * (section_offset)addr_size)
                        throw loc_error(sec_type, list_off + rel_entry,
                                        "unterminated location list");
                taddr a = read_address(rel_entry);
                taddr b = read_address(rel_entry);

                if (a == 0 && b == 0) {
                        // End of list.  Stop here; the bytes that
                        // follow belong to another list.
                        break;
                }
                if (a == addr_mask) {
                        // Base address selection entry.
                        base_addr = b;
                        continue;
                }

                // Regular location list entry.
                if (a > b)
                        throw loc_error(sec_type, list_off + rel_entry,
                                        "reversed location range");
                if (a > addr_mask - base_addr || b > addr_mask - base_addr)
                        throw loc_error(sec_type, list_off + rel_entry,
                                        "base address addition wraps");
                taddr low = base_addr + a;
                taddr high = base_addr + b;

                // The expression length is always a uint16.
                if (remaining() < sizeof(uhalf))
                        throw loc_error(sec_type, list_off + rel_entry,
                                        "truncated location expression length");
                uhalf expr_len = cur.fixed<uhalf>();
                section_offset expr_off = list_off + cur.get_section_offset();
                if (remaining() < expr_len)
                        throw loc_error(sec_type, list_off + rel_entry,
                                        "truncated location expression");
                cur += expr_len;

                // First matching entry wins; keep scanning so the
                // whole list (through its terminator) is validated.
                if (!sel.found && low <= pc && pc < high) {
                        sel.found = true;
                        sel.empty_expr = (expr_len == 0);
                        sel.expr_offset = expr_off;
                        sel.expr_len = expr_len;
                }
        }

        return sel;
}

//////////////////////////////////////////////////////////////////
// DW_OP_fbreg support
//

namespace {

// Resolves the frame base for one at_location query.  Evaluation is
// lazy: the frame base is only computed when the location expression
// actually executes DW_OP_fbreg.  The active flag turns a frame base
// that itself depends on a frame base into a reported circular
// dependency instead of unbounded recursion.
struct frame_resolver : frame_base_resolver
{
        const die &function;
        taddr pc;
        expr_context *ctx;
        bool active;

        frame_resolver(const die &function, taddr pc, expr_context *ctx)
                : function(function), pc(pc), ctx(ctx), active(false) { }

        struct guard
        {
                bool &flag;
                explicit guard(bool &flag) : flag(flag) { flag = true; }
                ~guard() { flag = false; }
        };

        taddr resolve() override
        {
                if (active)
                        throw expr_error("circular frame base dependency "
                                         "evaluating DW_OP_fbreg");
                guard g(active);

                value fv = function.resolve(DW_AT::frame_base);
                if (!fv.valid())
                        throw expr_error("DW_OP_fbreg requires "
                                         "DW_AT_frame_base, which is absent");

                expr_result fb;
                switch (fv.get_type()) {
                case value::type::exprloc:
                        fb = fv.as_exprloc().evaluate_with_frame(
                                ctx, {}, this);
                        break;
                case value::type::loclist: {
                        const unit *u = &fv.get_unit();
                        auto cusec = u->data();
                        die cudie = u->root();
                        taddr cu_low_pc = cudie.has(DW_AT::low_pc)
                                ? at_low_pc(cudie) : 0;
                        shared_ptr<section> locsec =
                                get_loc_section(u, fv.as_sec_offset());
                        loclist_selection s = select_loclist_entry(
                                locsec, fv.as_sec_offset(),
                                cusec->addr_size, cu_low_pc, pc,
                                section_type::loc);
                        if (!s.found || s.empty_expr)
                                throw expr_error("DW_AT_frame_base has no "
                                                 "location for this PC");
                        expr e = make_section_expr(u, locsec,
                                                   s.expr_offset, s.expr_len);
                        fb = e.evaluate_with_frame(ctx, {}, this);
                        break;
                }
                default:
                        throw expr_error("DW_AT_frame_base has unsupported "
                                         "value type " + to_string(fv.get_type()));
                }

                switch (fb.location_type) {
                case expr_result::type::reg:
                        // The register number is not an address; ask
                        // the context for the register's contents.
                        return ctx->reg((unsigned)fb.value);
                case expr_result::type::address:
                case expr_result::type::literal:
                        // Use the numeric value directly, without an
                        // extra memory dereference.
                        return fb.value;
                default:
                        throw expr_error("DW_AT_frame_base result has "
                                         "unsuitable type " +
                                         to_string(fb.location_type));
                }
        }
};

expr_result
empty_result()
{
        expr_result r;
        r.location_type = expr_result::type::empty;
        r.value = 0;
        r.implicit = nullptr;
        r.implicit_len = 0;
        return r;
}

} // namespace

//////////////////////////////////////////////////////////////////
// at_location
//

expr_result
at_location(const die &variable, const die &function,
            taddr pc, expr_context *ctx)
{
        // die::resolve preserves the concrete / abstract_origin /
        // specification precedence and may return a value owned by a
        // different compilation unit.
        value lv = variable.resolve(DW_AT::location);
        if (!lv.valid())
                return empty_result();

        frame_resolver resolver(function, pc, ctx);

        switch (lv.get_type()) {
        case value::type::exprloc:
                // as_exprloc also coerces legacy block forms.
                return lv.as_exprloc().evaluate_with_frame(
                        ctx, {}, &resolver);

        case value::type::loclist: {
                // Every decode must use the unit that owns the
                // attribute, not the unit of the variable DIE.
                const unit *u = &lv.get_unit();
                auto cusec = u->data();
                die cudie = u->root();
                taddr cu_low_pc = cudie.has(DW_AT::low_pc)
                        ? at_low_pc(cudie) : 0;

                // A missing .debug_loc section throws format_error.
                shared_ptr<section> locsec =
                        get_loc_section(u, lv.as_sec_offset());

                loclist_selection sel = select_loclist_entry(
                        locsec, lv.as_sec_offset(), cusec->addr_size,
                        cu_low_pc, pc, section_type::loc);

                // No matching range, or a matching entry with an
                // empty expression: a genuine empty location.
                if (!sel.found || sel.empty_expr)
                        return empty_result();

                // The expression bytes come from .debug_loc; the
                // offset must not be interpreted as a .debug_info
                // offset.  The unit still supplies target context.
                expr e = make_section_expr(u, locsec,
                                           sel.expr_offset, sel.expr_len);
                return e.evaluate_with_frame(ctx, {}, &resolver);
        }

        default:
                throw format_error("DW_AT_location has unexpected value type " +
                                   to_string(lv.get_type()));
        }
}

DWARFPP_END_NAMESPACE
