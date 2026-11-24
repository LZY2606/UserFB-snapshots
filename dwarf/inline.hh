// Copyright (c) 2013 Austin T. Clements. All rights reserved.
// Use of this this source code is governed by an MIT license
// that can be found in the LICENSE file.

// DWARF4 inline stack queries.
//
// This header provides offline symbolization of a single, already
// linked program counter in terms of the concrete DIE tree.  It only
// reads debugging information through the dwarf/loader interfaces; it
// never inspects a live process and never executes the program being
// analyzed.

#ifndef _DWARFPP_INLINE_HH_
#define _DWARFPP_INLINE_HH_

#include "dwarf++.hh"

#include <cstdint>
#include <string>
#include <vector>

DWARFPP_BEGIN_NAMESPACE

/**
 * An exception indicating that metadata inheritance for a single
 * attribute traversed more distinct DIEs than the query budget
 * allows.  This is recognizable by type so callers can distinguish
 * budget exhaustion from malformed section errors.
 *
 * Back edges in the abstract_origin/specification reference graph are
 * handled silently: already-visited DIEs are skipped.  This exception
 * is only thrown when the finite reference graph itself exceeds the
 * per-attribute budget of 256 distinct DIEs.
 */
class inline_metadata_budget_exceeded : public std::runtime_error
{
public:
        explicit inline_metadata_budget_exceeded(const std::string &what_arg)
                : std::runtime_error(what_arg) { }
        explicit inline_metadata_budget_exceeded(const char *what_arg)
                : std::runtime_error(what_arg) { }
};

/**
 * A source position attached to an inline frame.
 *
 * Each of the three fields is independent: an unknown field is marked
 * with its *_present flag set to false, and its value member is
 * meaningless.  Unknown fields are never backfilled from another
 * frame, another position, or the line number program.
 *
 * File index 0 is treated as unknown even when the corresponding
 * attribute is present.  Non-zero file indexes are resolved in the
 * line table of the compilation unit that actually provides the
 * attribute, so the resolved path may come from a different CU than
 * the queried DIE.
 */
struct inline_source_position
{
        /** True if a file path could be resolved. */
        bool file_present;
        /** The resolved source file path. */
        std::string file;

        /** True if the line attribute is present.  A line value of 0
         *  is still considered present. */
        bool line_present;
        /** The source line number. */
        std::uint64_t line;

        /** True if the column attribute is present.  A column value
         *  of 0 is still considered present. */
        bool column_present;
        /** The source column number. */
        std::uint64_t column;
};

/**
 * One frame of an inline stack: either a concrete physical
 * DW_TAG::subprogram or a concrete DW_TAG::inlined_subroutine.
 *
 * Every frame stores the identity of its concrete DIE as plain
 * numbers: the section offset of the header of the unit containing
 * the DIE, and the DIE's own offset within .debug_info.  Distinct
 * concrete DIEs remain distinct frames even when their names and
 * coordinates are identical.
 *
 * All strings and coordinates are owned by the frame itself; the
 * object does not retain any reference to the dwarf file or its
 * sections, so it remains valid after the originating dwarf object
 * is destroyed.
 */
struct inline_frame
{
        /** Section offset of the compilation unit header that
         *  contains the concrete DIE. */
        section_offset unit_offset;

        /** Section offset of the concrete DIE within .debug_info. */
        section_offset die_offset;

        /** DW_TAG::subprogram or DW_TAG::inlined_subroutine. */
        DW_TAG tag;

        /** True if a DW_AT::name was found. */
        bool name_present;
        /** The inherited or concrete name. */
        std::string name;

        /** True if a DW_AT::linkage_name was found. */
        bool linkage_name_present;
        /** The inherited or concrete linkage name. */
        std::string linkage_name;

        /** Declaration position (DW_AT::decl_file/decl_line/
         *  decl_column), inherited through abstract_origin and
         *  specification when the concrete DIE lacks the attribute. */
        inline_source_position declaration;

        /** Call position of a concrete inlined instance
         *  (DW_AT::call_file/call_line/call_column).  These
         *  attributes are read from the concrete DIE only and are
         *  never inherited.  All fields are absent for a physical
         *  function. */
        inline_source_position call;
};

/**
 * A complete inline stack, ordered from the outermost concrete
 * physical function to the innermost matching concrete inlined
 * instance.
 */
struct inline_stack
{
        std::vector<inline_frame> frames;
};

/**
 * Return every candidate inline stack covering the linked program
 * counter pc in the given DWARF file.
 *
 * Stack membership is determined solely by lexical ancestry in the
 * concrete DIE tree.  DW_TAG::lexical_block DIEs are transparent to
 * the stack, but a lexical block with its own range must cover pc or
 * its whole subtree is pruned; lexical blocks without a range are
 * always transparent.  subprogram and inlined_subroutine DIEs match
 * only when they have their own ranges (DW_AT::ranges or
 * DW_AT::low_pc, optionally with DW_AT::high_pc) and that range
 * covers pc.  A frame DIE without its own range is a barrier: it does
 * not match and its descendants cannot be stitched onto an outer
 * stack.
 *
 * All ranges are treated as half-open [low, high).  Holes and
 * base-address selection entries in range lists are honored, and
 * DW_AT::high_pc is accepted in both its address form and its
 * constant (offset from low_pc) form.
 *
 * Compilation units without a root range are always scanned; units
 * with a root range are filtered by it.
 *
 * Only the deepest matching lexical paths under each concrete
 * physical function are returned; short prefixes of a returned path
 * are not returned separately.  Parallel inlines, overlapping
 * physical functions, and overlaps between compilation units all
 * produce independent candidates.  Candidates are sorted
 * lexicographically by the sequence of concrete DIE section offsets
 * from the outer frame inward, and identical sequences are
 * de-duplicated.
 *
 * Metadata (name, linkage_name, declaration position) first looks at
 * the concrete DIE itself, then performs a depth-first walk through
 * abstract_origin and, only after the whole origin branch is
 * exhausted, specification references.  The declaration file index is
 * resolved in the line table of the unit that provides the
 * attribute.  Each attribute query visits at most 256 distinct DIEs
 * and throws inline_metadata_budget_exceeded if the reference graph
 * is larger; reference back edges are skipped, not followed.
 *
 * Returns an empty vector when no concrete frame covers pc.
 *
 * The result owns all of its storage.  The dwarf object must remain
 * valid only for the duration of this call.
 */
std::vector<inline_stack>
inline_stacks(const dwarf &file, taddr pc);

DWARFPP_END_NAMESPACE

#endif
