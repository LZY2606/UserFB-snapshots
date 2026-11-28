// Copyright (c) 2013 Austin T. Clements. All rights reserved.
// Use of this source code is governed by an MIT license
// that can be found in the LICENSE file.

#ifndef _DWARFPP_FRAME_HH_
#define _DWARFPP_FRAME_HH_

#ifndef DWARFPP_BEGIN_NAMESPACE
#define DWARFPP_BEGIN_NAMESPACE namespace dwarf {
#define DWARFPP_END_NAMESPACE   }
#endif

#include "dwarf++.hh"

#include <map>

DWARFPP_BEGIN_NAMESPACE

//////////////////////////////////////////////////////////////////
// Call frame information (.debug_frame)
//
// This is a reader and relocator for DWARF call frame information.
// It currently supports little-endian DWARF32 .debug_frame sections
// containing version 3 CIEs with empty augmentations.  .eh_frame is
// not supported.
//

/**
 * The kind of a register rule in a call frame information row.
 */
enum class frame_reg_kind
{
        /**
         * The register has no recoverable value.  This is also the
         * rule for registers that have no rule at all.
         */
        undefined,
        /**
         * The register has the same value it had in the caller.
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
         * The register's previous value is in register reg.
         */
        reg,
};

/**
 * The rule for recovering a single register in a frame row.
 */
struct frame_reg_rule
{
        frame_reg_kind kind;
        /**
         * The offset for kind::offset and kind::val_offset rules.
         * This is the decoded offset (the encoded operand scaled by
         * the CIE's data alignment factor).
         */
        int64_t offset;
        /**
         * The register number for kind::reg rules.
         */
        uint64_t reg;

        frame_reg_rule()
                : kind(frame_reg_kind::undefined), offset(0), reg(0) { }

        static frame_reg_rule make_undefined()
        {
                return frame_reg_rule();
        }

        static frame_reg_rule make_same_value()
        {
                frame_reg_rule r;
                r.kind = frame_reg_kind::same_value;
                return r;
        }

        static frame_reg_rule make_offset(int64_t offset)
        {
                frame_reg_rule r;
                r.kind = frame_reg_kind::offset;
                r.offset = offset;
                return r;
        }

        static frame_reg_rule make_val_offset(int64_t offset)
        {
                frame_reg_rule r;
                r.kind = frame_reg_kind::val_offset;
                r.offset = offset;
                return r;
        }

        static frame_reg_rule make_reg(uint64_t reg)
        {
                frame_reg_rule r;
                r.kind = frame_reg_kind::reg;
                r.reg = reg;
                return r;
        }

        bool operator==(const frame_reg_rule &o) const
        {
                return kind == o.kind &&
                        (kind == frame_reg_kind::undefined ||
                         kind == frame_reg_kind::same_value ||
                         (kind == frame_reg_kind::reg ? reg == o.reg
                                                      : offset == o.offset));
        }

        bool operator!=(const frame_reg_rule &o) const
        {
                return !(*this == o);
        }
};

/**
 * The rule for computing the canonical frame address (CFA) in a
 * frame row.  If defined is false, the CFA is undefined in this row.
 */
struct frame_cfa_rule
{
        bool defined;
        /**
         * The register from which the CFA is computed.
         */
        uint64_t reg;
        /**
         * The offset added to reg to obtain the CFA.
         */
        int64_t offset;

        frame_cfa_rule()
                : defined(false), reg(0), offset(0) { }

        frame_cfa_rule(uint64_t reg, int64_t offset)
                : defined(true), reg(reg), offset(offset) { }

        bool operator==(const frame_cfa_rule &o) const
        {
                return defined == o.defined &&
                        (!defined || (reg == o.reg && offset == o.offset));
        }

        bool operator!=(const frame_cfa_rule &o) const
        {
                return !(*this == o);
        }
};

/**
 * A row of call frame information.  The row describes the rules in
 * effect for the half-open address range [begin, end).
 *
 * regs holds the rules of all registers whose rule is not
 * "undefined"; querying a register absent from regs yields an
 * undefined rule.
 */
struct frame_row
{
        taddr begin, end;

        frame_cfa_rule cfa;

        std::map<uint64_t, frame_reg_rule> regs;

        /**
         * Return the rule for reg, or an undefined rule if this row
         * has no rule for reg.
         */
        frame_reg_rule rule(uint64_t reg) const
        {
                auto it = regs.find(reg);
                if (it == regs.end())
                        return frame_reg_rule();
                return it->second;
        }

        /**
         * Return true if this row describes the same state as o
         * (ignoring the address range).
         */
        bool same_state(const frame_row &o) const
        {
                return cfa == o.cfa && regs == o.regs;
        }

        bool operator==(const frame_row &o) const
        {
                return begin == o.begin && end == o.end && same_state(o);
        }

        bool operator!=(const frame_row &o) const
        {
                return !(*this == o);
        }
};

/**
 * A code segment mapping for frame_table::relocate.  The bytes in
 * [old_start, old_start + length) are moved to [new_start, new_start
 * + length).
 */
struct frame_segment
{
        taddr old_start, new_start, length;

        frame_segment() : old_start(0), new_start(0), length(0) { }
        frame_segment(taddr old_start, taddr new_start, taddr length)
                : old_start(old_start), new_start(new_start), length(length) { }
};

/**
 * The result of frame_table::relocate: a stand-alone .debug_frame
 * section covering relocated code segments, plus the decoded rule
 * rows of that section.  This object owns its data; it does not
 * reference the frame table or loader it was produced from.
 */
class relocated_frame
{
public:
        relocated_frame() = default;
        relocated_frame(const relocated_frame &o) = default;
        relocated_frame(relocated_frame &&o) = default;

        relocated_frame &operator=(const relocated_frame &o) = default;
        relocated_frame &operator=(relocated_frame &&o) = default;

        /**
         * The bytes of the relocated .debug_frame section.
         */
        const std::vector<char> &bytes() const
        {
                return data;
        }

        /**
         * The rule rows of the relocated section, at the new
         * addresses.
         */
        const std::vector<frame_row> &rows() const
        {
                return table;
        }

private:
        friend class frame_table;

        relocated_frame(std::vector<char> &&data, std::vector<frame_row> &&table)
                : data(std::move(data)), table(std::move(table)) { }

        std::vector<char> data;
        std::vector<frame_row> table;
};

/**
 * A parsed .debug_frame section: a set of call frame information
 * entries (CIEs) and frame description entries (FDEs) decoded into
 * rule rows.
 *
 * Only little-endian DWARF32 entries, CIE version 3, and empty CIE
 * augmentations are supported; anything else (including expression
 * rules) causes construction to throw format_error.
 */
class frame_table
{
public:
        /**
         * Construct a frame table from the .debug_frame section of
         * the given DWARF file.  addr_size is the target address
         * size in bytes and must be 4 or 8.  Throws format_error if
         * the section is missing or malformed.
         */
        frame_table(const dwarf &file, unsigned addr_size);

        /**
         * Construct an invalid, empty frame table.
         */
        frame_table() = default;
        frame_table(const frame_table &o) = default;
        frame_table(frame_table &&o) = default;

        frame_table &operator=(const frame_table &o) = default;
        frame_table &operator=(frame_table &&o) = default;

        /**
         * Return true if this object represents a parsed frame
         * table.  Default constructed frame tables are not valid.
         */
        bool valid() const
        {
                return !!m;
        }

        /**
         * Return all rule rows of all FDEs, in section order.  Rows
         * of each FDE contiguously cover the FDE's address range.
         */
        const std::vector<frame_row> &rows() const;

        /**
         * Return the row covering addr, or nullptr if no FDE covers
         * addr.
         */
        const frame_row *find_row(taddr addr) const;

        /**
         * Relocate call frame information for moved code segments.
         *
         * Each segment moves [old_start, old_start+length) to
         * [new_start, new_start+length).  Segments must have
         * non-zero length, must not overlap each other in either old
         * or new addresses, and each segment must lie entirely
         * within a single FDE of this table; violations throw
         * invalid_argument.
         *
         * The result contains one FDE per segment covering exactly
         * the moved bytes.  The rule in effect at any new address
         * equals the rule at the corresponding old address,
         * including state that was already in effect at a segment's
         * start and the effects of remember_state/restore_state and
         * restore instructions within the segment.  Unmapped parts
         * of the original FDEs are not emitted.
         */
        relocated_frame relocate(const std::vector<frame_segment> &segments) const;

private:
        struct impl;
        std::shared_ptr<impl> m;
};

DWARFPP_END_NAMESPACE

#endif
