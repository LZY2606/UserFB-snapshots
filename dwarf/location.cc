// Copyright (c) 2013 Austin T. Clements. All rights reserved.
// Use of this source code is governed by an MIT license
// that can be found in the LICENSE file.

#include "internal.hh"

using namespace std;

DWARFPP_BEGIN_NAMESPACE

namespace {

// Mirror die::resolve's inheritance order and return the DIE that
// actually supplies attr.  Returns an invalid die if d has no such
// attribute.
static die
resolved_provider(const die &d, DW_AT attr)
{
        if (d.has(attr))
                return d;
        if (d.has(DW_AT::abstract_origin)) {
                die ao = d[DW_AT::abstract_origin].as_reference();
                if (ao.has(attr))
                        return ao;
                if (ao.has(DW_AT::specification)) {
                        die s = ao[DW_AT::specification].as_reference();
                        if (s.has(attr))
                                return s;
                }
        } else if (d.has(DW_AT::specification)) {
                die s = d[DW_AT::specification].as_reference();
                if (s.has(attr))
                        return s;
        }
        return die();
}

// Evaluate a location-valued attribute (exprloc, legacy block, or
// loclist) at pc.
static expr_result
evaluate_location_value(const value &v, taddr pc, expr_context *ctx,
                        bool *is_list, bool *selected,
                        taddr *range_low, taddr *range_high,
                        expr *selected_expr)
{
        *is_list = (v.get_type() == value::type::loclist);
        *selected = false;
        *range_low = 0;
        *range_high = 0;

        if (!*is_list) {
                // Direct expression (exprloc form or a legacy block
                // that as_exprloc knows how to coerce).
                expr e = v.as_exprloc();
                *selected = true;
                if (selected_expr)
                        *selected_expr = e;
                return e.evaluate(ctx);
        }

        section_offset off = v.as_sec_offset();
        const unit &u = v.get_unit();
        auto sec = u.get_dwarf().get_section(section_type::loc);
        location_list list(sec, off, u);

        for (auto ent : list) {
                if (ent.low <= pc && pc < ent.high) {
                        *selected = true;
                        *range_low = ent.low;
                        *range_high = ent.high;
                        if (selected_expr)
                                *selected_expr = ent.location;
                        if (ent.location.empty()) {
                                // Empty location description: the
                                // object is unavailable over this
                                // range.
                                expr_result r;
                                r.location_type = expr_result::type::empty;
                                r.value = 0;
                                r.implicit = nullptr;
                                r.implicit_len = 0;
                                return r;
                        }
                        return ent.location.evaluate(ctx);
                }
        }

        // pc falls in a gap of the list.
        expr_result r;
        r.location_type = expr_result::type::empty;
        r.value = 0;
        r.implicit = nullptr;
        r.implicit_len = 0;
        return r;
}

// Shared state for lazy DW_AT_frame_base evaluation.
struct frame_base_state
{
        const die *function;
        taddr pc;
        expr_context *upstream;
        bool evaluating;
        location_evidence *ev;
};

/**
 * expr_context that resolves DW_OP_fbreg through the enclosing
 * function's DW_AT_frame_base at the current pc and delegates every
 * other operation to the caller-provided context.
 */
class frame_base_context : public expr_context
{
public:
        frame_base_context(const die &function, taddr pc,
                           expr_context *upstream, location_evidence *ev)
        {
                st.function = &function;
                st.pc = pc;
                st.upstream = upstream;
                st.evaluating = false;
                st.ev = ev;
        }

        taddr reg(unsigned regnum) override
        {
                return st.upstream->reg(regnum);
        }

        taddr deref_size(taddr address, unsigned size) override
        {
                return st.upstream->deref_size(address, size);
        }

        taddr xderef_size(taddr address, taddr asid, unsigned size) override
        {
                return st.upstream->xderef_size(address, asid, size);
        }

        taddr form_tls_address(taddr address) override
        {
                return st.upstream->form_tls_address(address);
        }

        taddr cfa() override
        {
                return st.upstream->cfa();
        }

        taddr frame_base() override
        {
                if (st.evaluating)
                        throw expr_error("recursive DW_AT_frame_base evaluation "
                                         "(DW_OP_fbreg used within DW_AT_frame_base)");

                st.ev->frame_base_used = true;
                die provider = resolved_provider(*st.function, DW_AT::frame_base);
                if (!provider.valid())
                        throw frame_base_missing(
                                "DW_OP_fbreg used but the function has no "
                                "DW_AT_frame_base attribute");
                st.ev->frame_base_die_offset = provider.get_section_offset();

                value v = st.function->resolve(DW_AT::frame_base);
                struct evaluating_guard {
                        bool &flag;
                        explicit evaluating_guard(bool &flag) : flag(flag)
                        {
                                flag = true;
                        }
                        ~evaluating_guard() { flag = false; }
                } guard(st.evaluating);
                bool is_list = false, selected = false;
                taddr rl = 0, rh = 0;
                expr chosen;
                expr_result r = evaluate_location_value(v, st.pc, this,
                                                        &is_list, &selected,
                                                        &rl, &rh, &chosen);
                st.ev->frame_base_is_list = is_list;

                switch (r.location_type) {
                case expr_result::type::reg:
                        // A register frame base requires reading that
                        // register; the register value is the frame
                        // base.
                        return st.upstream->reg((unsigned)r.value);
                case expr_result::type::address:
                case expr_result::type::literal:
                        // Address and literal frame bases are used
                        // directly; they are never dereferenced.
                        return r.value;
                case expr_result::type::empty:
                        throw frame_base_missing(
                                "DW_AT_frame_base is an empty location "
                                "description at the requested program counter");
                case expr_result::type::implicit:
                        throw expr_error(
                                "implicit DW_AT_frame_base location descriptions "
                                "are not supported");
                }
                throw expr_error("unexpected DW_AT_frame_base result type");
        }

private:
        frame_base_state st;
};

} // anonymous namespace

// Core implementation shared by the public at_location entry point
// and the offline variable analysis layer.
expr_result
resolve_variable_location(const die &variable, const die &function,
                          taddr pc, expr_context *ctx,
                          location_evidence *ev)
{
        location_evidence local_ev;
        if (!ev)
                ev = &local_ev;

        value v = variable.resolve(DW_AT::location);
        if (!v.valid()) {
                ev->has_location = false;
                expr_result r;
                r.location_type = expr_result::type::empty;
                r.value = 0;
                r.implicit = nullptr;
                r.implicit_len = 0;
                return r;
        }
        ev->has_location = true;

        die provider = resolved_provider(variable, DW_AT::location);
        if (provider.valid())
                ev->location_die_offset = provider.get_section_offset();

        frame_base_context fbc(function, pc, ctx, ev);
        bool selected = false;
        taddr rl = 0, rh = 0;
        expr chosen;
        expr_result r = evaluate_location_value(v, pc, &fbc,
                                                &ev->is_list, &selected,
                                                &rl, &rh, &chosen);
        ev->selected = selected;
        ev->range_low = rl;
        ev->range_high = rh;
        if (selected) {
                ev->expr_section = chosen.source_section();
                ev->expr_offset = chosen.source_offset();
                ev->expr_length = chosen.source_length();
                ev->expr_empty = chosen.empty();
        }
        return r;
}

expr_result
at_location(const die &variable, const die &function,
            taddr pc, expr_context *ctx)
{
        if (!ctx)
                ctx = &no_expr_context;
        return resolve_variable_location(variable, function, pc, ctx, nullptr);
}

die
die_at(const unit &u, section_offset unit_offset)
{
        if (unit_offset >= u.data()->size())
                throw format_error("DIE offset 0x" + to_hex(unit_offset) +
                                   " is outside of its compilation unit");
        die d(&u);
        try {
                d.read(unit_offset);
        } catch (const underflow_error &e) {
                throw format_error(string("malformed DIE at offset 0x") +
                                   to_hex(unit_offset) + ": " + e.what());
        }
        if (!d.valid())
                throw format_error("offset 0x" + to_hex(unit_offset) +
                                   " does not contain a DIE");
        return d;
}

const compilation_unit &
compilation_unit_at(const dwarf &file, section_offset section_off)
{
        const compilation_unit *found = nullptr;
        for (auto &cu : file.compilation_units()) {
                section_offset start = cu.get_section_offset();
                section_offset end = start + cu.data()->size();
                if (start <= section_off && section_off < end) {
                        found = &cu;
                        break;
                }
        }
        if (!found)
                throw out_of_range("no compilation unit contains .debug_info "
                                   "offset 0x" + to_hex(section_off));
        return *found;
}

DWARFPP_END_NAMESPACE
