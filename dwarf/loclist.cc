// Copyright (c) 2013 Austin T. Clements. All rights reserved.
// Use of this source code is governed by an MIT license
// that can be found in the LICENSE file.

#include "internal.hh"

#include <stdexcept>

using namespace std;

DWARFPP_BEGIN_NAMESPACE

// Format a section-relative offset for diagnostics.
static string
at_offset(section_offset off)
{
        return "offset 0x" + to_hex(off);
}

location_list::location_list(const std::shared_ptr<section> &src,
                             section_offset off, const unit &cu)
{
        if (!src || off > src->size())
                throw format_error(".debug_loc location list at " + at_offset(off) +
                                   " is outside of the .debug_loc section");

        auto cusec = cu.data();
        section_length remaining = src->size() - off;

        // Interpret the list with the byte order, format and address
        // size of the CU that owns the referring attribute.
        auto sec = make_shared<section>(
                section_type::loc, src->begin + off, remaining,
                cusec->ord, cusec->fmt, cusec->addr_size);

        taddr base_addr = 0;
        die cudie = cu.root();
        if (cudie.valid() && cudie.has(DW_AT::low_pc))
                base_addr = at_low_pc(cudie);

        taddr largest_offset = ~(taddr)0;
        if (sec->addr_size < sizeof(taddr))
                largest_offset = ((taddr)1 << (8 * sec->addr_size)) - 1;

        cursor cur(sec);
        while (true) {
                section_offset rel = cur.get_section_offset();
                section_offset entry_off = off + rel;
                taddr low, high;
                try {
                        low = cur.address();
                        high = cur.address();
                } catch (const underflow_error &) {
                        throw format_error(".debug_loc location list at " +
                                           at_offset(off) + " is truncated before an "
                                           "end-of-list terminator (entry at " +
                                           at_offset(entry_off) + ")");
                }

                if (low == 0 && high == 0) {
                        // End of list.
                        break;
                }

                if (low == largest_offset) {
                        // Base address selection entry.
                        base_addr = high;
                        continue;
                }

                // Ordinary range entry.  Ranges are half-open and
                // must be strictly increasing.
                if (high <= low)
                        throw format_error(".debug_loc entry at " + at_offset(entry_off) +
                                           " has a reversed range [0x" + to_hex(low) +
                                           ", 0x" + to_hex(high) + ")");

                taddr adj_low, adj_high;
                if (base_addr != 0 && low > largest_offset - base_addr)
                        throw format_error(".debug_loc entry at " + at_offset(entry_off) +
                                           " low address wraps the address space when "
                                           "added to base address 0x" + to_hex(base_addr));
                adj_low = base_addr + low;
                if (base_addr != 0 && high > largest_offset - base_addr)
                        throw format_error(".debug_loc entry at " + at_offset(entry_off) +
                                           " high address wraps the address space when "
                                           "added to base address 0x" + to_hex(base_addr));
                adj_high = base_addr + high;
                if (adj_high <= adj_low)
                        throw format_error(".debug_loc entry at " + at_offset(entry_off) +
                                           " has a reversed adjusted range");

                section_length expr_len;
                try {
                        expr_len = cur.fixed<uhalf>();
                } catch (const underflow_error &) {
                        throw format_error(".debug_loc entry at " + at_offset(entry_off) +
                                           " is truncated before its expression length");
                }

                section_offset expr_rel = cur.get_section_offset();
                try {
                        cur.ensure(expr_len);
                } catch (const underflow_error &) {
                        throw format_error(".debug_loc entry at " + at_offset(entry_off) +
                                           " expression of " + std::to_string(expr_len) +
                                           " bytes is truncated");
                }

                auto esec = sec->slice(expr_rel, expr_len);
                expr e(&cu, esec, off + expr_rel, expr_len);
                entries.push_back(stored_entry{adj_low, adj_high, std::move(e)});
                cur += expr_len;
        }
}

location_list::iterator
location_list::begin() const
{
        return iterator(this, 0);
}

location_list::iterator
location_list::end() const
{
        return iterator(this, entries.size());
}

location_entry
location_list::iterator::operator*() const
{
        const stored_entry &e = list->entries[pos];
        return location_entry{e.low, e.high, e.location};
}

DWARFPP_END_NAMESPACE
