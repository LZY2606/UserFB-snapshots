// Copyright (c) 2013 Austin T. Clements. All rights reserved.
// Use of this source code is governed by an MIT license
// that can be found in the LICENSE file.

#ifndef _DWARFPP_VARIABLE_HH_
#define _DWARFPP_VARIABLE_HH_

#include "dwarf++.hh"

#include <map>
#include <string>
#include <vector>

DWARFPP_BEGIN_NAMESPACE

/**
 * Byte order of the target machine whose state a snapshot describes.
 * This must match the byte order of the ELF file being analyzed.
 */
enum class snapshot_endian
{
        little,
        big
};

/**
 * An offline, owning snapshot of a target machine's registers and
 * memory at one point in execution.  Nothing here ever refers to the
 * analyzing process's own memory or registers: addresses are target
 * addresses and are never converted to native pointers.
 */
class variable_snapshot
{
public:
        explicit variable_snapshot(snapshot_endian order) : order_(order) { }

        snapshot_endian endian() const
        {
                return order_;
        }

        /** Record register regnum as holding value. */
        void set_register(unsigned regnum, taddr value)
        {
                registers_[regnum] = value;
        }

        bool has_register(unsigned regnum) const
        {
                return registers_.find(regnum) != registers_.end();
        }

        /** Return the recorded value of register regnum. */
        taddr get_register(unsigned regnum) const
        {
                auto it = registers_.find(regnum);
                if (it == registers_.end())
                        throw std::out_of_range("register not present in snapshot");
                return it->second;
        }

        const std::map<unsigned, taddr> &registers() const
        {
                return registers_;
        }

        /** Record the canonical frame address. */
        void set_cfa(taddr cfa)
        {
                has_cfa_ = true;
                cfa_ = cfa;
        }

        bool has_cfa() const
        {
                return has_cfa_;
        }

        taddr get_cfa() const
        {
                return cfa_;
        }

        /**
         * Add a memory segment covering the half-open target address
         * range [address, address + bytes.size()).  Segments must not
         * overlap and must not wrap the target address space.
         * Throws std::invalid_argument otherwise.
         */
        void add_memory_segment(taddr address, std::vector<unsigned char> bytes);

        /** A contiguous run of target memory bytes. */
        struct segment
        {
                taddr address;
                std::vector<unsigned char> bytes;
        };

        const std::vector<segment> &segments() const
        {
                return segments_;
        }

        /**
         * Read exactly width target bytes starting at address,
         * concatenating across adjacent segments if necessary.
         * Throws std::invalid_argument for a wrapping or oversized
         * read and an expr_error-derived exception if any requested
         * byte is not covered; a partial value is never returned.
         */
        std::vector<unsigned char> read_memory(taddr address,
                                               unsigned width) const;

private:
        snapshot_endian order_;
        std::map<unsigned, taddr> registers_;
        bool has_cfa_ = false;
        taddr cfa_ = 0;
        std::vector<segment> segments_;
};

/**
 * The outcome of an offline variable analysis.  Every value here is
 * owned (plain scalars, strings and byte vectors), so a
 * variable_result remains valid after the ELF loader, snapshot and
 * DIE arguments have been released or modified.
 */
enum class variable_status
{
        /** A value was recovered.  See value and raw_bytes. */
        value,
        /** The variable has no location at this pc (gap or empty). */
        unavailable,
        /** A location needs a register the snapshot does not provide. */
        missing_register,
        /** A location needs the CFA but the snapshot has none. */
        missing_cfa,
        /** A memory byte needed for the value is not in the snapshot. */
        missing_memory,
        /** An expression uses an operation this engine does not implement. */
        unsupported,
        /** The DWARF data or expression is malformed. */
        malformed
};

std::string to_string(variable_status v);

/** Evidence that a register was read during evaluation. */
struct accessed_register
{
        unsigned regnum;
        taddr value;
};

/** Evidence that target memory was read during evaluation. */
struct accessed_memory
{
        taddr address;
        std::vector<unsigned char> bytes;
};

/**
 * The owning result of inspect_variable.  Evidence lists contain only
 * context that was actually accessed; registers, the CFA and memory
 * that were never touched do not appear.
 */
struct variable_result
{
        variable_status status = variable_status::unavailable;

        // Present when status == value.
        uint64_t value = 0;
        std::vector<unsigned char> raw_bytes;

        // Location resolution evidence.
        bool has_location_attr = false;
        section_offset location_die_offset = 0;
        bool location_is_list = false;
        bool frame_base_used = false;
        section_offset frame_base_die_offset = 0;
        bool selected = false;
        taddr range_low = 0, range_high = 0;
        section_type expr_section = section_type::info;
        section_offset expr_offset = 0;
        section_length expr_length = 0;
        bool expr_empty = false;

        // Accessed context.
        std::vector<accessed_register> registers;
        bool cfa_accessed = false;
        taddr cfa_value = 0;
        std::vector<accessed_memory> memory;

        // Set when the recovered value came from a memory address.
        bool has_value_address = false;
        taddr value_address = 0;

        // Detail for unsupported/malformed outcomes.
        std::string error;
};

/**
 * Analyze variable (a child of function) entirely offline at program
 * counter pc, recovering a value of width bytes (1-8) from snap.
 * The variable and function DIEs are supplied by the caller; no
 * global variable search or lexical-scope enumeration is performed.
 *
 * The snapshot's byte order must match that of the DIEs' ELF file.
 * This function never executes the ELF and never reads the analyzing
 * process's memory or registers.
 */
variable_result inspect_variable(const die &variable, const die &function,
                                 taddr pc, unsigned width,
                                 const variable_snapshot &snap);

DWARFPP_END_NAMESPACE

#endif
