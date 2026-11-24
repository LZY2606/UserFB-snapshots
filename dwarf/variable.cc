// Copyright (c) 2013 Austin T. Clements. All rights reserved.
// Use of this source code is governed by an MIT license
// that can be found in the LICENSE file.

#include "variable.hh"
#include "internal.hh"

#include <algorithm>
#include <limits>
#include <stdexcept>

using namespace std;

DWARFPP_BEGIN_NAMESPACE

namespace {

// Internal control-flow exceptions used to map snapshot shortages to
// the public variable_status taxonomy.  They derive from expr_error so
// they can propagate out of the expression engine.
struct snapshot_missing_register : expr_error
{
        unsigned regnum;
        snapshot_missing_register(unsigned regnum)
                : expr_error("register not present in snapshot"),
                  regnum(regnum) { }
};

struct snapshot_missing_cfa : expr_error
{
        snapshot_missing_cfa()
                : expr_error("CFA not present in snapshot") { }
};

struct snapshot_missing_memory : expr_error
{
        taddr address;
        unsigned width;
        snapshot_missing_memory(taddr address, unsigned width)
                : expr_error("target memory not present in snapshot"),
                  address(address), width(width) { }
};

/**
 * expr_context backed by an offline snapshot.  All accesses are
 * recorded so the caller can see exactly which context a value came
 * from.
 */
class snapshot_context : public expr_context
{
public:
        snapshot_context(const variable_snapshot &snap, variable_result &out)
                : snap_(snap), out_(out) { }

        taddr reg(unsigned regnum) override
        {
                if (!snap_.has_register(regnum))
                        throw snapshot_missing_register(regnum);
                taddr v = snap_.get_register(regnum);
                record_register(regnum, v);
                return v;
        }

        taddr deref_size(taddr address, unsigned size) override
        {
                // A dereference inside an expression is a target
                // memory read, exactly like an address location.
                vector<unsigned char> bytes = read(address, size);
                return assemble(bytes);
        }

        taddr cfa() override
        {
                if (!snap_.has_cfa())
                        throw snapshot_missing_cfa();
                out_.cfa_accessed = true;
                out_.cfa_value = snap_.get_cfa();
                return out_.cfa_value;
        }

        vector<unsigned char> read(taddr address, unsigned width)
        {
                vector<unsigned char> bytes;
                try {
                        bytes = snap_.read_memory(address, width);
                } catch (const snapshot_missing_memory &) {
                        throw;
                }
                out_.memory.push_back(accessed_memory{address, bytes});
                return bytes;
        }

        taddr assemble(const vector<unsigned char> &bytes)
        {
                uint64_t v = 0;
                if (snap_.endian() == snapshot_endian::little) {
                        for (size_t i = 0; i < bytes.size(); i++)
                                v |= (uint64_t)bytes[i] << (8 * i);
                } else {
                        for (size_t i = 0; i < bytes.size(); i++)
                                v = (v << 8) | bytes[i];
                }
                return v;
        }

private:
        void record_register(unsigned regnum, taddr value)
        {
                for (auto &a : out_.registers) {
                        if (a.regnum == regnum) {
                                a.value = value;
                                return;
                        }
                }
                out_.registers.push_back(accessed_register{regnum, value});
        }

        const variable_snapshot &snap_;
        variable_result &out_;
};

static uint64_t
low_mask(unsigned width)
{
        if (width >= sizeof(uint64_t))
                return ~(uint64_t)0;
        return ((uint64_t)1 << (8 * width)) - 1;
}

// Encode the low width bytes of value in the target byte order.
static vector<unsigned char>
encode_bytes(uint64_t value, unsigned width, snapshot_endian order)
{
        vector<unsigned char> bytes(width);
        for (unsigned i = 0; i < width; i++)
                bytes[i] = (unsigned char)((value >> (8 * i)) & 0xff);
        if (order == snapshot_endian::big)
                reverse(bytes.begin(), bytes.end());
        return bytes;
}

static uint64_t
assemble_bytes(const vector<unsigned char> &bytes, snapshot_endian order)
{
        uint64_t v = 0;
        if (order == snapshot_endian::little) {
                for (size_t i = 0; i < bytes.size(); i++)
                        v |= (uint64_t)bytes[i] << (8 * i);
        } else {
                for (size_t i = 0; i < bytes.size(); i++)
                        v = (v << 8) | bytes[i];
        }
        return v;
}

} // anonymous namespace

void
variable_snapshot::add_memory_segment(taddr address,
                                      vector<unsigned char> bytes)
{
        if (bytes.empty())
                throw invalid_argument("memory segment must not be empty");

        taddr last = address + bytes.size() - 1;
        if (last < address)
                throw invalid_argument("memory segment wraps the target "
                                       "address space");

        for (auto &s : segments_) {
                taddr s_last = s.address + s.bytes.size() - 1;
                bool overlap = !(last < s.address || s_last < address);
                if (overlap)
                        throw invalid_argument("memory segments overlap");
            }

        segments_.push_back(segment{address, std::move(bytes)});
}

vector<unsigned char>
variable_snapshot::read_memory(taddr address, unsigned width) const
{
        if (width == 0)
                throw invalid_argument("read width must be non-zero");
        if (width > 8)
                throw invalid_argument("read width must be at most 8");
        if (address > numeric_limits<taddr>::max() - (width - 1))
                throw invalid_argument("memory read wraps the target "
                                       "address space");

        vector<unsigned char> out;
        out.reserve(width);
        taddr want = address;
        unsigned remaining = width;

        while (remaining > 0) {
                const segment *hit = nullptr;
                size_t skip = 0;
                for (auto &s : segments_) {
                        taddr s_last = s.address + s.bytes.size() - 1;
                        if (s.address <= want && want <= s_last) {
                                hit = &s;
                                skip = (size_t)(want - s.address);
                                break;
                        }
                }
                if (!hit)
                        throw snapshot_missing_memory(address, width);

                size_t avail = hit->bytes.size() - skip;
                size_t take = min<size_t>(avail, remaining);
                out.insert(out.end(),
                           hit->bytes.begin() + skip,
                           hit->bytes.begin() + skip + take);
                want += take;
                remaining -= (unsigned)take;
        }
        return out;
}

string
to_string(variable_status v)
{
        switch (v) {
        case variable_status::value:
                return "value";
        case variable_status::unavailable:
                return "unavailable";
        case variable_status::missing_register:
                return "missing_register";
        case variable_status::missing_cfa:
                return "missing_cfa";
        case variable_status::missing_memory:
                return "missing_memory";
        case variable_status::unsupported:
                return "unsupported";
        case variable_status::malformed:
                return "malformed";
        }
        return "unknown";
}

variable_result
inspect_variable(const die &variable, const die &function,
                 taddr pc, unsigned width,
                 const variable_snapshot &snap)
{
        variable_result out;

        if (width < 1 || width > 8) {
                out.status = variable_status::malformed;
                out.error = "value width must be between 1 and 8 bytes";
                return out;
        }

        // The snapshot byte order must match the ELF/DWARF byte order.
        const auto ord = variable.get_unit().data()->ord;
        bool elf_little = (ord == byte_order::lsb);
        bool snap_little = (snap.endian() == snapshot_endian::little);
        if (elf_little != snap_little) {
                out.status = variable_status::malformed;
                out.error = "snapshot byte order does not match the ELF file";
                return out;
        }

        location_evidence ev;
        snapshot_context sctx(snap, out);
        expr_result loc;
        try {
                loc = resolve_variable_location(variable, function, pc,
                                                &sctx, &ev);
        } catch (const expr_unsupported &e) {
                out.status = variable_status::unsupported;
                out.error = e.what();
                return out;
        } catch (const frame_base_missing &e) {
                out.status = variable_status::unavailable;
                out.error = e.what();
                return out;
        } catch (const snapshot_missing_register &e) {
                out.status = variable_status::missing_register;
                out.error = string("register ") + std::to_string(e.regnum) +
                            " is not present in the snapshot";
                return out;
        } catch (const snapshot_missing_cfa &e) {
                out.status = variable_status::missing_cfa;
                out.error = e.what();
                return out;
        } catch (const snapshot_missing_memory &e) {
                out.status = variable_status::missing_memory;
                out.error = "target memory at 0x" + to_hex(e.address) +
                            " is not present in the snapshot";
                return out;
        } catch (const expr_error &e) {
                out.status = variable_status::malformed;
                out.error = e.what();
                return out;
        } catch (const format_error &e) {
                out.status = variable_status::malformed;
                out.error = e.what();
                return out;
        } catch (const underflow_error &e) {
                out.status = variable_status::malformed;
                out.error = e.what();
                return out;
        } catch (const runtime_error &e) {
                out.status = variable_status::malformed;
                out.error = e.what();
                return out;
        }

        // Copy location evidence.
        out.has_location_attr = ev.has_location;
        out.location_die_offset = ev.location_die_offset;
        out.location_is_list = ev.is_list;
        out.frame_base_used = ev.frame_base_used;
        out.frame_base_die_offset = ev.frame_base_die_offset;
        out.selected = ev.selected;
        out.range_low = ev.range_low;
        out.range_high = ev.range_high;
        out.expr_section = ev.expr_section;
        out.expr_offset = ev.expr_offset;
        out.expr_length = ev.expr_length;
        out.expr_empty = ev.expr_empty;

        if (!ev.has_location) {
                out.status = variable_status::unavailable;
                out.error = "variable has no DW_AT_location";
                return out;
        }

        if (loc.location_type == expr_result::type::empty) {
                out.status = variable_status::unavailable;
                out.error = "selected location is an empty description";
                return out;
        }

        switch (loc.location_type) {
        case expr_result::type::reg: {
                unsigned regnum = (unsigned)loc.value;
                if (!snap.has_register(regnum)) {
                        out.status = variable_status::missing_register;
                        out.error = string("register ") + std::to_string(regnum) +
                                    " is not present in the snapshot";
                        return out;
                }
                taddr raw = snap.get_register(regnum);
                out.registers.push_back(accessed_register{regnum, raw});
                uint64_t v = (uint64_t)raw & low_mask(width);
                out.value = v;
                out.raw_bytes = encode_bytes(v, width, snap.endian());
                out.status = variable_status::value;
                return out;
        }

        case expr_result::type::literal: {
                uint64_t v = (uint64_t)loc.value & low_mask(width);
                out.value = v;
                out.raw_bytes = encode_bytes(v, width, snap.endian());
                out.status = variable_status::value;
                return out;
        }

        case expr_result::type::address: {
                taddr address = loc.value;
                vector<unsigned char> bytes;
                try {
                        bytes = snap.read_memory(address, width);
                } catch (const invalid_argument &e) {
                        out.status = variable_status::malformed;
                        out.error = e.what();
                        return out;
                } catch (const snapshot_missing_memory &e) {
                        out.status = variable_status::missing_memory;
                        out.error = "target memory at 0x" +
                                    to_hex(e.address) + " is not present "
                                    "in the snapshot";
                        return out;
                }
                out.has_value_address = true;
                out.value_address = address;
                out.memory.push_back(accessed_memory{address, bytes});
                out.raw_bytes = bytes;
                out.value = assemble_bytes(bytes, snap.endian());
                out.status = variable_status::value;
                return out;
        }

        case expr_result::type::implicit: {
                if (loc.implicit_len != width || loc.implicit_len > 8) {
                        out.status = variable_status::malformed;
                        out.error = "implicit location width does not match "
                                    "the requested value width";
                        return out;
                }
                out.raw_bytes.assign(
                        reinterpret_cast<const unsigned char *>(loc.implicit),
                        reinterpret_cast<const unsigned char *>(loc.implicit)
                                + loc.implicit_len);
                out.value = assemble_bytes(out.raw_bytes, snap.endian());
                out.status = variable_status::value;
                return out;
        }

        case expr_result::type::empty:
                break;
        }

        out.status = variable_status::malformed;
        out.error = "unrecognized location result type";
        return out;
}

DWARFPP_END_NAMESPACE
