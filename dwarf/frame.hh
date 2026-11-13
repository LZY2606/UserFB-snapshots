// Copyright (c) 2013 Austin T. Clements. All rights reserved.
// Use of this source code is governed by an MIT license
// that can be found in the LICENSE file.

#ifndef _DWARFPP_FRAME_HH_
#define _DWARFPP_FRAME_HH_

#include "dwarf++.hh"

#include <map>
#include <vector>

DWARFPP_BEGIN_NAMESPACE

//////////////////////////////////////////////////////////////////
// Call frame information (.debug_frame)
//

/**
 * How a register's value is recovered at a row of a frame table.
 */
enum class frame_rule_type
{
        /**
         * The register has no recoverable value.  This is the
         * default for registers that have no rule.
         */
        undefined,
        /**
         * The register holds the same value it held in the caller.
         */
        same_value,
        /**
         * The register's previous value is stored at CFA + offset.
         */
        offset,
        /**
         * The register's previous value is CFA + offset.
         */
        val_offset,
        /**
         * The register's previous value is held in another register.
         */
        reg,
};

/**
 * A rule describing how to recover a single register at a row of a
 * frame table.
 */
struct frame_register_rule
{
        frame_rule_type type;
        /**
         * For type::reg, the register holding the value.
         */
        uint64_t reg;
        /**
         * For type::offset and type::val_offset, the byte offset
         * from the CFA.  The data alignment factor is already
         * multiplied in.
         */
        int64_t offset;

        frame_register_rule()
                : type(frame_rule_type::undefined), reg(0), offset(0) { }

        static frame_register_rule undefined()
        {
                return frame_register_rule();
        }

        static frame_register_rule same_value()
        {
                frame_register_rule r;
                r.type = frame_rule_type::same_value;
                return r;
        }

        static frame_register_rule at_offset(int64_t offset)
        {
                frame_register_rule r;
                r.type = frame_rule_type::offset;
                r.offset = offset;
                return r;
        }

        static frame_register_rule at_val_offset(int64_t offset)
        {
                frame_register_rule r;
                r.type = frame_rule_type::val_offset;
                r.offset = offset;
                return r;
        }

        static frame_register_rule in_register(uint64_t reg)
        {
                frame_register_rule r;
                r.type = frame_rule_type::reg;
                r.reg = reg;
                return r;
        }
};

inline bool
operator==(const frame_register_rule &a, const frame_register_rule &b)
{
        if (a.type != b.type)
                return false;
        switch (a.type) {
        case frame_rule_type::offset:
        case frame_rule_type::val_offset:
                return a.offset == b.offset;
        case frame_rule_type::reg:
                return a.reg == b.reg;
        default:
                return true;
        }
}

inline bool
operator!=(const frame_register_rule &a, const frame_register_rule &b)
{
        return !(a == b);
}

/**
 * A rule describing how to compute the canonical frame address
 * (CFA) at a row of a frame table.  The CFA is reg + offset, where
 * offset is a plain byte offset (not factored).
 */
struct frame_cfa_rule
{
        /**
         * False if the CFA is undefined at this row.  The CFA is
         * initially undefined; a CIE's initial instructions usually
         * define it.
         */
        bool defined;
        uint64_t reg;
        int64_t offset;

        frame_cfa_rule()
                : defined(false), reg(0), offset(0) { }

        frame_cfa_rule(uint64_t reg, int64_t offset)
                : defined(true), reg(reg), offset(offset) { }
};

inline bool
operator==(const frame_cfa_rule &a, const frame_cfa_rule &b)
{
        if (!a.defined || !b.defined)
                return a.defined == b.defined;
        return a.reg == b.reg && a.offset == b.offset;
}

inline bool
operator!=(const frame_cfa_rule &a, const frame_cfa_rule &b)
{
        return !(a == b);
}

/**
 * A row of a frame table.  The row's rules apply to the half-open
 * address range [begin, end).  Registers without an entry in
 * 'registers' have the undefined rule.
 */
struct frame_row
{
        taddr begin, end;
        frame_cfa_rule cfa;
        std::map<uint64_t, frame_register_rule> registers;

        frame_row()
                : begin(0), end(0) { }

        bool covers(taddr addr) const
        {
                return begin <= addr && addr < end;
        }
};

inline bool
operator==(const frame_row &a, const frame_row &b)
{
        return a.begin == b.begin && a.end == b.end && a.cfa == b.cfa &&
                a.registers == b.registers;
}

inline bool
operator!=(const frame_row &a, const frame_row &b)
{
        return !(a == b);
}

/**
 * One piece of code moved from old_start to new_start, keeping its
 * length.  Source ranges of a relocation must not overlap each
 * other, target ranges must not overlap each other, and each
 * segment must lie entirely within exactly one original FDE.
 */
struct frame_segment
{
        taddr old_start, new_start, length;

        frame_segment(taddr old_start, taddr new_start, taddr length)
                : old_start(old_start), new_start(new_start),
                  length(length) { }
};

class relocated_frame;

/**
 * A parsed .debug_frame section: a list of FDEs, each expanded
 * into a list of rows giving the rules in effect over half-open
 * address ranges.
 *
 * Only little-endian DWARF32 records, CIE version 3, and empty CIE
 * augmentations are supported; anything else (including expression
 * rules) throws format_error.
 */
class frame_table
{
public:
        class fde
        {
        public:
                taddr initial_location;
                taddr address_range;
                uint64_t code_alignment;
                int64_t data_alignment;
                uint64_t return_address_register;
                /**
                 * The state established by the CIE's initial
                 * instructions.  DW_CFA_restore* rules return a
                 * register to these rules.  begin and end are not
                 * meaningful here.
                 */
                frame_row initial_state;
                /**
                 * Rows of this FDE in increasing address order.
                 * Adjacent rows tile [initial_location,
                 * initial_location + address_range).
                 */
                std::vector<frame_row> rows;

                fde()
                        : initial_location(0), address_range(0),
                          code_alignment(0), data_alignment(0),
                          return_address_register(0) { }

                taddr end() const
                {
                        return initial_location + address_range;
                }

                bool covers(taddr addr) const
                {
                        return initial_location <= addr && addr < end();
                }

                /**
                 * Return the row containing addr, or nullptr if
                 * addr is outside this FDE.
                 */
                const frame_row *find_row(taddr addr) const;
        };

        /**
         * Construct a frame table from the .debug_frame section of
         * the given DWARF file.  address_size must be 4 or 8.
         */
        frame_table(const dwarf &file, unsigned address_size);

        /**
         * \internal Construct a frame table directly from a
         * section.  The section must be little-endian.
         */
        frame_table(const std::shared_ptr<section> &sec, unsigned address_size);

        /**
         * Construct an invalid, empty frame table.
         */
        frame_table()
                : addr_size(0) { }

        frame_table(const frame_table &o) = default;
        frame_table(frame_table &&o) = default;

        frame_table &operator=(const frame_table &o) = default;
        frame_table &operator=(frame_table &&o) = default;

        bool valid() const
        {
                return addr_size != 0;
        }

        unsigned address_size() const
        {
                return addr_size;
        }

        const std::vector<fde> &fdes() const
        {
                return fdes_;
        }

        /**
         * Return the FDE containing addr, or nullptr.
         */
        const fde *find_fde(taddr addr) const;

        /**
         * Return the row containing addr across all FDEs, or
         * nullptr.
         */
        const frame_row *find_row(taddr addr) const;

        /**
         * Produce a stand-alone .debug_frame section covering only
         * the moved code segments.  Each segment yields one FDE at
         * its new start address covering exactly the moved bytes;
         * the rules at each new address equal the rules at the
         * corresponding old address, including the state in effect
         * at a segment's first address.  Unmapped parts of the
         * original FDEs are not emitted.
         *
         * Throws format_error if the segments overlap, wrap around,
         * or do not each lie within exactly one original FDE, or if
         * the rules cannot be encoded (for example, a row whose CFA
         * becomes undefined after being defined).
         */
        relocated_frame relocate(const std::vector<frame_segment> &segments) const;

private:
        unsigned addr_size;
        std::vector<fde> fdes_;
};

/**
 * The result of frame_table::relocate: a self-contained
 * .debug_frame section.  Owns both the encoded bytes and the rules
 * parsed back from them; neither refers to the original DWARF file
 * or loader.
 */
class relocated_frame
{
public:
        relocated_frame() = default;

        relocated_frame(std::vector<char> data, frame_table table)
                : data_(std::move(data)), table_(std::move(table)) { }

        /**
         * The encoded .debug_frame bytes.
         */
        const std::vector<char> &bytes() const
        {
                return data_;
        }

        /**
         * The rules of the relocated frame, parsed back from
         * bytes().
         */
        const frame_table &table() const
        {
                return table_;
        }

        /**
         * Short-hand for table().find_row(addr).
         */
        const frame_row *find_row(taddr addr) const
        {
                return table_.find_row(addr);
        }

private:
        std::vector<char> data_;
        frame_table table_;
};

DWARFPP_END_NAMESPACE

#endif
