// Copyright (c) 2013 Austin T. Clements. All rights reserved.
// Use of this source code is governed by an MIT license
// that can be found in the LICENSE file.

#include "internal.hh"
#include "inline.hh"

#include <algorithm>
#include <set>
#include <utility>

using namespace std;

DWARFPP_BEGIN_NAMESPACE

namespace {

// Maximum number of distinct DIEs visited while inheriting a single
// attribute through abstract_origin/specification references.
const unsigned metadata_budget = 256;

struct die_id
{
        section_offset unit;
        section_offset die;
};

die_id
id_of(const die &d)
{
        die_id id;
        id.unit = d.get_unit().get_section_offset();
        id.die = d.get_section_offset();
        return id;
}

bool
die_has_own_range(const die &d)
{
        return d.has(DW_AT::ranges) || d.has(DW_AT::low_pc);
}

// Caller must have established die_has_own_range.  Range lists keep
// their holes and base-address selection, and all intervals are
// half-open.
bool
die_range_covers(const die &d, taddr pc)
{
        try {
                return die_pc_range(d).contains(pc);
        } catch (out_of_range &e) {
                return false;
        } catch (value_type_mismatch &e) {
                return false;
        }
}

bool
is_frame_tag(DW_TAG tag)
{
        return tag == DW_TAG::subprogram ||
               tag == DW_TAG::inlined_subroutine;
}

// Depth-first collection of the deepest matching lexical paths.
// cur holds the enclosing matched frame DIEs; every complete path is
// appended to paths.  Returns true if at least one complete path was
// emitted from this subtree.
bool
walk_concrete(const die &d, taddr pc, vector<die> &cur,
              vector<vector<die> > &paths)
{
        if (is_frame_tag(d.tag)) {
                // A frame DIE without its own range never matches and
                // acts as a barrier: its descendants cannot be
                // stitched onto an outer frame.
                if (!die_has_own_range(d) || !die_range_covers(d, pc))
                        return false;

                cur.push_back(d);
                bool any_child = false;
                for (auto &child : d)
                        any_child = walk_concrete(child, pc, cur, paths)
                                    || any_child;
                if (!any_child) {
                        // No matching inlined descendant: this frame
                        // is the deepest point of this path.
                        paths.push_back(cur);
                }
                cur.pop_back();
                return true;
        }

        if (d.tag == DW_TAG::lexical_block) {
                // A ranged lexical block must cover the PC itself; a
                // lexical block without a range is transparent.
                if (die_has_own_range(d) && !die_range_covers(d, pc))
                        return false;
        }

        // Other scoping DIEs and transparent lexical blocks: descend
        // without becoming frames.
        bool any_child = false;
        for (auto &child : d)
                any_child = walk_concrete(child, pc, cur, paths)
                            || any_child;
        return any_child;
}

// The result of one inherited attribute lookup: the value and the
// unit that actually provided it (its line table owns decl_file
// indexes).
struct attr_hit
{
        value v;
        const unit *provider;
        bool found;
};

// Depth-first metadata inheritance.  The concrete DIE is checked
// first, then the whole abstract_origin branch, then the
// specification branch, at every hop.  Back edges are skipped via the
// visited set and a reference graph larger than metadata_budget
// distinct DIEs raises inline_metadata_budget_exceeded.
attr_hit
find_inherited_attr(const die &start, DW_AT attr)
{
        vector<die> stack;
        set<pair<section_offset, section_offset> > seen;
        stack.push_back(start);

        unsigned visited = 0;
        while (!stack.empty()) {
                die d = stack.back();
                stack.pop_back();

                die_id id = id_of(d);
                if (!seen.insert(make_pair(id.unit, id.die)).second)
                        continue;
                if (++visited > metadata_budget)
                        throw inline_metadata_budget_exceeded(
                                "metadata inheritance budget of "
                                + std::to_string(metadata_budget)
                                + " distinct DIEs exceeded for "
                                + to_string(attr));

                if (d.has(attr)) {
                        attr_hit hit;
                        hit.v = d[attr];
                        hit.provider = &d.get_unit();
                        hit.found = true;
                        return hit;
                }

                // Push specification first so abstract_origin is
                // explored first by the LIFO stack.
                if (d.has(DW_AT::specification))
                        stack.push_back(
                                d[DW_AT::specification].as_reference());
                if (d.has(DW_AT::abstract_origin))
                        stack.push_back(
                                d[DW_AT::abstract_origin].as_reference());
        }

        attr_hit miss;
        miss.provider = nullptr;
        miss.found = false;
        return miss;
}

const line_table::file *
resolve_file(const unit &provider, uint64_t index)
{
        // Index 0 means unknown regardless of the line table.
        if (index == 0)
                return nullptr;
        const compilation_unit *cu =
                dynamic_cast<const compilation_unit *>(&provider);
        if (!cu)
                return nullptr;
        const line_table &lt = cu->get_line_table();
        if (!lt.valid())
                return nullptr;
        try {
                return lt.get_file((unsigned)index);
        } catch (out_of_range &e) {
                return nullptr;
        }
}

inline_source_position
empty_position()
{
        inline_source_position pos;
        pos.file_present = false;
        pos.line_present = false;
        pos.column_present = false;
        pos.line = 0;
        pos.column = 0;
        return pos;
}

// Declaration coordinates: each field is inherited independently and
// the file index is resolved in the providing unit's line table.
void
fill_declaration(inline_source_position &pos, const die &d)
{
        attr_hit file = find_inherited_attr(d, DW_AT::decl_file);
        if (file.found) {
                const line_table::file *lf =
                        resolve_file(*file.provider,
                                     file.v.as_uconstant());
                if (lf) {
                        pos.file_present = true;
                        pos.file = lf->path;
                }
        }

        attr_hit line = find_inherited_attr(d, DW_AT::decl_line);
        if (line.found) {
                pos.line_present = true;
                pos.line = line.v.as_uconstant();
        }

        attr_hit column = find_inherited_attr(d, DW_AT::decl_column);
        if (column.found) {
                pos.column_present = true;
                pos.column = column.v.as_uconstant();
        }
}

// Call coordinates: read from the concrete instance only, never
// inherited; file indexes belong to the concrete instance's own CU.
void
fill_call(inline_source_position &pos, const die &d)
{
        if (d.has(DW_AT::call_file)) {
                const line_table::file *lf =
                        resolve_file(d.get_unit(),
                                     d[DW_AT::call_file].as_uconstant());
                if (lf) {
                        pos.file_present = true;
                        pos.file = lf->path;
                }
        }
        if (d.has(DW_AT::call_line)) {
                pos.line_present = true;
                pos.line = d[DW_AT::call_line].as_uconstant();
        }
        if (d.has(DW_AT::call_column)) {
                pos.column_present = true;
                pos.column = d[DW_AT::call_column].as_uconstant();
        }
}

inline_frame
build_frame(const die &d)
{
        inline_frame f;
        f.unit_offset = d.get_unit().get_section_offset();
        f.die_offset = d.get_section_offset();
        f.tag = d.tag;
        f.name_present = false;
        f.linkage_name_present = false;
        f.declaration = empty_position();
        f.call = empty_position();

        attr_hit name = find_inherited_attr(d, DW_AT::name);
        if (name.found) {
                f.name_present = true;
                f.name = name.v.as_string();
        }

        attr_hit linkage = find_inherited_attr(d, DW_AT::linkage_name);
        if (linkage.found) {
                f.linkage_name_present = true;
                f.linkage_name = linkage.v.as_string();
        }

        fill_declaration(f.declaration, d);
        if (d.tag == DW_TAG::inlined_subroutine)
                fill_call(f.call, d);
        return f;
}

bool
path_less(const vector<die> &a, const vector<die> &b)
{
        size_t n = min(a.size(), b.size());
        for (size_t i = 0; i < n; ++i) {
                section_offset xa = a[i].get_section_offset();
                section_offset xb = b[i].get_section_offset();
                if (xa != xb)
                        return xa < xb;
        }
        return a.size() < b.size();
}

bool
path_equal(const vector<die> &a, const vector<die> &b)
{
        if (a.size() != b.size())
                return false;
        for (size_t i = 0; i < a.size(); ++i)
                if (a[i].get_section_offset() !=
                    b[i].get_section_offset())
                        return false;
        return true;
}

} // anonymous namespace

vector<inline_stack>
inline_stacks(const dwarf &file, taddr pc)
{
        vector<vector<die> > paths;

        for (auto &cu : file.compilation_units()) {
                const die &root = cu.root();
                // Range-less CUs are always scanned; ranged CUs are
                // filtered by their own root range.
                if (die_has_own_range(root) && !die_range_covers(root, pc))
                        continue;

                vector<die> cur;
                walk_concrete(root, pc, cur, paths);
        }

        sort(paths.begin(), paths.end(), path_less);
        paths.erase(unique(paths.begin(), paths.end(), path_equal),
                    paths.end());

        vector<inline_stack> result;
        result.reserve(paths.size());
        for (auto &path : paths) {
                inline_stack stack;
                stack.frames.reserve(path.size());
                for (auto &d : path)
                        stack.frames.push_back(build_frame(d));
                result.push_back(std::move(stack));
        }
        return result;
}

DWARFPP_END_NAMESPACE
