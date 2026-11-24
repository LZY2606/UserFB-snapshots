// Independently authored DWARF4 contract test for dwarf::inline_stacks.
//
// The fixture below hand-builds valid little-endian DWARF32, 8-byte
// address DWARF4 sections (.debug_abbrev, .debug_info, .debug_line,
// .debug_ranges) and feeds them through the real memory loader and
// the production parser.  No hand-constructed frame objects are
// tested: every expectation is checked against query results.

#include "dwarf++.hh"
#include "inline.hh"

#include <cstdint>
#include <initializer_list>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using namespace std;

namespace {

unsigned checks = 0;

void
require(bool ok, const string &message)
{
        ++checks;
        if (!ok)
                throw runtime_error("assertion failed: " + message);
}

struct bytes
{
        vector<unsigned char> v;

        size_t size() const { return v.size(); }

        void n(uint64_t x, unsigned width)
        {
                for (unsigned i = 0; i < width; ++i)
                        v.push_back((unsigned char)((x >> (8 * i)) & 255));
        }

        void u(uint64_t x)
        {
                do {
                        unsigned char b = (unsigned char)(x & 127);
                        x >>= 7;
                        v.push_back((unsigned char)(b | (x ? 128 : 0)));
                } while (x);
        }

        void str(const string &s)
        {
                v.insert(v.end(), s.begin(), s.end());
                v.push_back(0);
        }

        void patch(size_t at, uint64_t x, unsigned width = 4)
        {
                if (at + width > size())
                        throw logic_error("patch out of bounds");
                for (unsigned i = 0; i < width; ++i)
                        v[at + i] = (unsigned char)((x >> (8 * i)) & 255);
        }
};

// Numeric DWARF4 encodings used by the fixture.
enum : unsigned {
        TAG_CU = 0x11, TAG_LEX = 0x0b, TAG_SUB = 0x2e, TAG_INLINE = 0x1d,
        AT_NAME = 0x03, AT_STMT = 0x10, AT_LOW = 0x11, AT_HIGH = 0x12,
        AT_COMP_DIR = 0x1b, AT_INLINE = 0x20, AT_AO = 0x31,
        AT_DECL_COL = 0x39, AT_DECL_FILE = 0x3a, AT_DECL_LINE = 0x3b,
        AT_SPEC = 0x47, AT_RANGES = 0x55, AT_CALL_COL = 0x57,
        AT_CALL_FILE = 0x58, AT_CALL_LINE = 0x59, AT_LINKAGE = 0x6e,
        FORM_ADDR = 0x01, FORM_DATA4 = 0x06, FORM_STRING = 0x08,
        FORM_DATA1 = 0x0b, FORM_REF_ADDR = 0x10, FORM_REF4 = 0x13,
        FORM_SEC_OFF = 0x17, FORM_DATA2 = 0x05
};

struct attr_spec
{
        unsigned attr;
        unsigned form;
};

struct abbrev_spec
{
        unsigned tag;
        bool children;
        vector<attr_spec> attrs;
};

struct ref_patch
{
        size_t pos;
        string target;
        bool local;             // true: ref4, CU-relative
        string cu;
};

struct fixture : dwarf::loader
{
        vector<abbrev_spec> abbrevs;
        bytes ab, info, line, ranges;
        map<dwarf::section_type, bytes> sections;
        map<string, uint64_t> off;           // label -> .debug_info offset
        map<string, string> die_cu;          // DIE label -> CU label
        vector<ref_patch> patches;
        string current_cu;

        fixture()
        {
                build_abbrevs();
                emit_abbrev();
                build_ranges();
                build_line_tables();
                build_cu_a();
                build_cu_b();
                resolve_patches();
                sections[dwarf::section_type::abbrev] = ab;
                sections[dwarf::section_type::info] = info;
                sections[dwarf::section_type::line] = line;
                sections[dwarf::section_type::ranges] = ranges;
        }

        const void *
        load(dwarf::section_type type, size_t *size_out) override
        {
                auto it = sections.find(type);
                if (it == sections.end()) {
                        *size_out = 0;
                        return nullptr;
                }
                *size_out = it->second.v.size();
                return it->second.v.data();
        }

        unsigned
        reg(unsigned tag, bool children, initializer_list<attr_spec> attrs)
        {
                abbrev_spec s;
                s.tag = tag;
                s.children = children;
                s.attrs.assign(attrs.begin(), attrs.end());
                abbrevs.push_back(s);
                return (unsigned)abbrevs.size();
        }

        void
        build_abbrevs()
        {
                // 1: ranged CU root
                reg(TAG_CU, true, {
                        {AT_NAME, FORM_STRING}, {AT_COMP_DIR, FORM_STRING},
                        {AT_LOW, FORM_ADDR}, {AT_HIGH, FORM_DATA4},
                        {AT_STMT, FORM_SEC_OFF}});
                // 2: range-less CU root
                reg(TAG_CU, true, {
                        {AT_NAME, FORM_STRING}, {AT_COMP_DIR, FORM_STRING},
                        {AT_STMT, FORM_SEC_OFF}});
                // 3: physical subprogram with children
                reg(TAG_SUB, true, {
                        {AT_NAME, FORM_STRING}, {AT_LOW, FORM_ADDR},
                        {AT_HIGH, FORM_DATA4}});
                // 4: leaf subprogram, constant high_pc
                reg(TAG_SUB, false, {
                        {AT_NAME, FORM_STRING}, {AT_LOW, FORM_ADDR},
                        {AT_HIGH, FORM_DATA4}});
                // 5: leaf subprogram, address high_pc
                reg(TAG_SUB, false, {
                        {AT_NAME, FORM_STRING}, {AT_LOW, FORM_ADDR},
                        {AT_HIGH, FORM_ADDR}});
                // 6: abstract subprogram, full declaration metadata
                reg(TAG_SUB, false, {
                        {AT_NAME, FORM_STRING}, {AT_LINKAGE, FORM_STRING},
                        {AT_INLINE, FORM_DATA1}, {AT_DECL_FILE, FORM_DATA1},
                        {AT_DECL_LINE, FORM_DATA2},
                        {AT_DECL_COL, FORM_DATA1}});
                // 7: abstract subprogram, abstract origin only
                reg(TAG_SUB, false, {{AT_AO, FORM_REF4}});
                // 8: abstract subprogram, origin plus declaration
                reg(TAG_SUB, false, {
                        {AT_AO, FORM_REF4}, {AT_DECL_FILE, FORM_DATA1},
                        {AT_DECL_LINE, FORM_DATA2}});
                // 9: abstract subprogram, name plus linkage
                reg(TAG_SUB, false, {
                        {AT_NAME, FORM_STRING},
                        {AT_LINKAGE, FORM_STRING}});
                // 10: abstract subprogram, name plus inline
                reg(TAG_SUB, false, {
                        {AT_NAME, FORM_STRING},
                        {AT_INLINE, FORM_DATA1}});
                // 11: abstract subprogram, origin plus specification
                reg(TAG_SUB, false, {
                        {AT_AO, FORM_REF4}, {AT_SPEC, FORM_REF4}});
                // 12: abstract subprogram, name only
                reg(TAG_SUB, false, {{AT_NAME, FORM_STRING}});
                // 13: abstract subprogram, specification only
                reg(TAG_SUB, false, {{AT_SPEC, FORM_REF4}});
                // 14: abstract subprogram, name plus specification
                reg(TAG_SUB, false, {
                        {AT_NAME, FORM_STRING}, {AT_SPEC, FORM_REF4}});
                // 15: ranged lexical block with children
                reg(TAG_LEX, true, {{AT_RANGES, FORM_SEC_OFF}});
                // 16: transparent lexical block with children
                reg(TAG_LEX, true, {});
                // 17: IA1, concrete name, address high_pc, call site
                reg(TAG_INLINE, true, {
                        {AT_NAME, FORM_STRING}, {AT_AO, FORM_REF_ADDR},
                        {AT_LOW, FORM_ADDR}, {AT_HIGH, FORM_ADDR},
                        {AT_CALL_FILE, FORM_DATA1},
                        {AT_CALL_LINE, FORM_DATA2},
                        {AT_CALL_COL, FORM_DATA1}});
                // 18: inline, constant high_pc, full call position
                reg(TAG_INLINE, false, {
                        {AT_AO, FORM_REF_ADDR}, {AT_LOW, FORM_ADDR},
                        {AT_HIGH, FORM_DATA4}, {AT_CALL_FILE, FORM_DATA1},
                        {AT_CALL_LINE, FORM_DATA2},
                        {AT_CALL_COL, FORM_DATA1}});
                // 19: inline, concrete decl_line 0, no call column
                reg(TAG_INLINE, false, {
                        {AT_AO, FORM_REF_ADDR},
                        {AT_DECL_LINE, FORM_DATA2},
                        {AT_LOW, FORM_ADDR}, {AT_HIGH, FORM_ADDR},
                        {AT_CALL_FILE, FORM_DATA1},
                        {AT_CALL_LINE, FORM_DATA2}});
                // 20: inline, range list
                reg(TAG_INLINE, false, {
                        {AT_AO, FORM_REF_ADDR},
                        {AT_RANGES, FORM_SEC_OFF}});
                // 21: inline, constant high_pc, no call position
                reg(TAG_INLINE, false, {
                        {AT_AO, FORM_REF_ADDR}, {AT_LOW, FORM_ADDR},
                        {AT_HIGH, FORM_DATA4}});
                // 22: range-less inline barrier with children
                reg(TAG_INLINE, true, {{AT_AO, FORM_REF_ADDR}});
        }

        void
        emit_abbrev()
        {
                for (size_t i = 0; i < abbrevs.size(); ++i) {
                        auto &s = abbrevs[i];
                        ab.u(i + 1);
                        ab.u(s.tag);
                        ab.n(s.children ? 1 : 0, 1);
                        for (auto &a : s.attrs) {
                                ab.u(a.attr);
                                ab.u(a.form);
                        }
                        ab.u(0);
                        ab.u(0);
                }
                ab.u(0);
        }

        void
        range_pair(uint64_t low, uint64_t high)
        {
                ranges.n(low, 8);
                ranges.n(high, 8);
        }

        void
        end_list()
        {
                ranges.n(0, 8);
                ranges.n(0, 8);
        }

        void
        build_ranges()
        {
                // Initial base address is CU A's low_pc, 0x1000.
                off["ranges_lb1"] = ranges.size();
                range_pair(0x10, 0x20);          // [0x1010,0x1020)
                range_pair(0x30, 0x50);          // [0x1030,0x1050)
                end_list();

                off["ranges_lbx"] = ranges.size();
                range_pair(0x80, 0x84);          // [0x1080,0x1084)
                end_list();

                off["ranges_ic"] = ranges.size();
                range_pair(0x10, 0x20);          // [0x1010,0x1020)
                ranges.n(~(uint64_t)0, 8);       // base address selection
                ranges.n(0x1040, 8);
                range_pair(0, 0x10);             // [0x1040,0x1050)
                end_list();
        }

        // One DWARF4 line program with two named files.
        size_t
        line_table(const string &one, const string &two)
        {
                size_t start = line.size();
                line.n(0, 4);
                line.n(4, 2);
                size_t hp = line.size();
                line.n(0, 4);
                line.n(1, 1);
                line.n(1, 1);
                line.n(1, 1);
                line.n(251, 1);
                line.n(14, 1);
                line.n(13, 1);
                const unsigned counts[] = {
                        0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1
                };
                for (auto c : counts)
                        line.n(c, 1);
                line.str("");                   // directory 0
                for (auto &f : {one, two}) {
                        line.str(f);
                        line.u(0);
                        line.u(0);
                        line.u(0);
                }
                line.str("");
                line.patch(hp, line.size() - (hp + 4));
                // Minimal valid program.
                line.n(0, 1);
                line.u(9);
                line.n(2, 1);
                line.n(0x1000, 8);
                line.n(1, 1);
                line.n(2, 1);
                line.u(0x100);
                line.n(0, 1);
                line.u(1);
                line.n(1, 1);
                line.patch(start, line.size() - (start + 4));
                return start;
        }

        void
        build_line_tables()
        {
                // Same numeric file indexes resolve to different
                // paths in the two CUs.
                off["line_a"] = line_table("a1.cc", "a2.cc");
                off["line_b"] = line_table("b1.cc", "shared.hpp");
        }

        void
        begin_cu(const string &label, const string &name, const string &dir,
                 unsigned abbr)
        {
                current_cu = label;
                off[label] = info.size();
                info.n(0, 4);                   // length, patched later
                info.n(4, 2);                   // DWARF version 4
                info.n(0, 4);                   // abbrev offset
                info.n(8, 1);                   // address size
                info.u(abbr);                   // root DIE abbrev code
                info.str(name);
                info.str(dir);
        }

        void
        end_cu(const string &label)
        {
                info.u(0);                      // root children terminator
                info.patch(off[label],
                           info.size() - (off[label] + 4));
        }

        void
        begin_die(const string &label, unsigned abbr)
        {
                off[label] = info.size();
                die_cu[label] = current_cu;
                info.u(abbr);
        }

        void
        ref(const string &target, bool local)
        {
                patches.push_back({info.size(), target, local,
                                   current_cu});
                info.n(0, 4);
        }

        void
        build_cu_a()
        {
                begin_cu("cuA", "caller.cc", "/cuA", 1);
                info.n(0x1000, 8);              // root low_pc
                info.n(0x100, 4);               // root high_pc, constant
                info.n(off["line_a"], 4);

                // Physical function physA: [0x1000,0x1100).
                begin_die("physA", 3);
                info.str("physA");
                info.n(0x1000, 8);
                info.n(0x100, 4);

                // Ranged lexical block with a hole.
                begin_die("lb1", 15);
                info.n(off["ranges_lb1"], 4);

                // IA1: concrete name override, address high_pc.
                begin_die("ia1", 17);
                info.str("concrete_name");
                ref("o1", false);
                info.n(0x1010, 8);
                info.n(0x1018, 8);              // high_pc, address form
                info.n(1, 1);                   // call file 1 -> /cuA/a1.cc
                info.n(11, 2);                  // call line
                info.n(4, 1);                   // call column

                // IA2: constant high_pc, nested inline.
                begin_die("ia2", 18);
                ref("o2", false);
                info.n(0x1010, 8);
                info.n(4, 4);                   // [0x1010,0x1014)
                info.n(2, 1);                   // call file 2 -> a2.cc
                info.n(22, 2);
                info.n(5, 1);

                // Transparent lexical block between frame DIEs; it
                // wraps the parallel inline rather than sitting empty.
                begin_die("lb_transparent", 16);

                // IA3: parallel inline overlapping IA2, concrete
                // decl_line 0, no call column.
                begin_die("ia3", 19);
                ref("o5", false);
                info.n(0, 2);                   // decl_line present, 0
                info.n(0x1012, 8);
                info.n(0x1016, 8);              // high_pc, address form
                info.n(1, 1);
                info.n(33, 2);
                info.u(0);                      // end transparent block
                info.u(0);                      // end ia1 children

                // IC: range list with hole and base selection.
                begin_die("ic", 20);
                ref("d0", false);
                info.n(off["ranges_ic"], 4);
                info.u(0);                      // end lb1 children

                // IB: second concrete instance of origin o1, no call.
                begin_die("ib", 21);
                ref("o1", false);
                info.n(0x1050, 8);
                info.n(0x10, 4);                // [0x1050,0x1060)

                // Range-less inline barrier with a ranged child: the
                // child must not be stitched onto physA.
                begin_die("ibar", 22);
                ref("o1", false);
                begin_die("ichild", 21);
                ref("o1", false);
                info.n(0x1070, 8);
                info.n(8, 4);                   // [0x1070,0x1078)
                info.u(0);                      // end ibar children

                // Ranged lexical block that does not cover the whole
                // child inline range.
                begin_die("lbx", 15);
                info.n(off["ranges_lbx"], 4);
                begin_die("ix", 21);
                ref("o5", false);
                info.n(0x1080, 8);
                info.n(0x10, 4);                // [0x1080,0x1090)
                info.u(0);                      // end lbx children

                // Concrete instance whose origin chain contains a
                // finite cycle.  Metadata lookup must skip the back
                // edge and still inherit c3's name.
                begin_die("icyc", 21);
                ref("c1", false);
                info.n(0x10a0, 8);
                info.n(4, 4);                   // [0x10a0,0x10a4)

                // Concrete instance whose origin chain is longer than
                // the metadata budget; querying it must raise the
                // recognizable budget exception.
                begin_die("ibudget", 21);
                ref("k0", false);
                info.n(0x10a4, 8);
                info.n(4, 4);                   // [0x10a4,0x10a8)
                info.u(0);                      // end physA children

                // Non-ancestral interval containment: a sibling
                // physical function whose range sits inside physA.
                begin_die("contained", 4);
                info.str("contained_fn");
                info.n(0x1012, 8);
                info.n(2, 4);                   // [0x1012,0x1014)

                // Half-open boundary probe.
                begin_die("edge_fn", 5);
                info.str("edge_fn");
                info.n(0x1020, 8);
                info.n(0x1024, 8);              // high_pc, address form

                end_cu("cuA");
        }

        void
        build_cu_b()
        {
                begin_cu("cuB", "meta.cc", "/cuB", 2);
                info.n(off["line_b"], 4);

                // Physical function in a range-less CU: the CU is
                // still scanned for every PC.
                begin_die("physB", 4);
                info.str("physB");
                info.n(0x2000, 8);
                info.n(0x10, 4);                // [0x2000,0x2010)

                // o1: full abstract description.  File index 1 in
                // CU B's line table is b1.cc.
                begin_die("o1", 6);
                info.str("callee");
                info.str("_Z6calleev");
                info.n(1, 1);                   // DW_INL_inlined
                info.n(1, 1);                   // decl file
                info.n(7, 2);                   // decl line
                info.n(2, 1);                   // decl column

                // o2: multi-hop origin to o4.
                begin_die("o2", 7);
                ref("o4", true);

                // o3: origin plus its own declaration.
                begin_die("o3", 8);
                ref("o1", true);
                info.n(2, 1);                   // decl file -> shared.hpp
                info.n(9, 2);                   // decl line override

                // o4: diamond convergence point.
                begin_die("o4", 9);
                info.str("diamond");
                info.str("_Z7diamondv");

                // o5: name but no declaration coordinates.
                begin_die("o5", 10);
                info.str("origin5");
                info.n(1, 1);

                // d0: diamond; abstract origin and specification both
                // reach o1 through different paths.
                begin_die("d0", 11);
                ref("o1", true);
                ref("o3", true);

                // Finite cycle: c1 -> c2 -> c3, and c3 points back at
                // c2.  The name is reachable; the back edge must be
                // skipped, not followed forever.
                begin_die("c1", 7);
                ref("c2", true);
                begin_die("c2", 13);
                ref("c3", true);
                begin_die("c3", 14);
                info.str("cycle_name");
                ref("c2", true);

                // Long finite chain to exercise the 256-DIE budget.
                for (unsigned i = 0; i < 259; ++i) {
                        begin_die("k" + to_string(i), 7);
                        ref("k" + to_string(i + 1), true);
                }
                begin_die("k259", 12);
                info.str("deep_name");

                end_cu("cuB");
        }

        void
        resolve_patches()
        {
                for (auto &p : patches) {
                        uint64_t target = off.at(p.target);
                        if (p.local)
                                target -= off.at(p.cu);
                        info.patch(p.pos, target);
                }
        }
};

// ---- Value snapshots, independently owned from the query result ----

struct pos_snapshot
{
        bool file_present;
        string file;
        bool line_present;
        uint64_t line;
        bool column_present;
        uint64_t column;
};

struct frame_snapshot
{
        uint64_t unit_offset;
        uint64_t die_offset;
        unsigned tag;
        bool name_present;
        string name;
        bool linkage_present;
        string linkage;
        pos_snapshot declaration;
        pos_snapshot call;
};

typedef vector<vector<frame_snapshot> > query_snapshot;

pos_snapshot
snapshot_pos(const dwarf::inline_source_position &p)
{
        pos_snapshot s;
        s.file_present = p.file_present;
        s.file = p.file;
        s.line_present = p.line_present;
        s.line = p.line;
        s.column_present = p.column_present;
        s.column = p.column;
        return s;
}

frame_snapshot
snapshot_frame(const dwarf::inline_frame &f)
{
        frame_snapshot s;
        s.unit_offset = f.unit_offset;
        s.die_offset = f.die_offset;
        s.tag = (unsigned)f.tag;
        s.name_present = f.name_present;
        s.name = f.name;
        s.linkage_present = f.linkage_name_present;
        s.linkage = f.linkage_name;
        s.declaration = snapshot_pos(f.declaration);
        s.call = snapshot_pos(f.call);
        return s;
}

query_snapshot
snapshot_query(const vector<dwarf::inline_stack> &stacks)
{
        query_snapshot out;
        for (auto &st : stacks) {
                vector<frame_snapshot> frames;
                for (auto &f : st.frames)
                        frames.push_back(snapshot_frame(f));
                out.push_back(frames);
        }
        return out;
}

bool
same_snapshot(const query_snapshot &a, const query_snapshot &b)
{
        if (a.size() != b.size())
                return false;
        for (size_t i = 0; i < a.size(); ++i) {
                if (a[i].size() != b[i].size())
                        return false;
                for (size_t j = 0; j < a[i].size(); ++j) {
                        const frame_snapshot &x = a[i][j];
                        const frame_snapshot &y = b[i][j];
                        if (x.unit_offset != y.unit_offset ||
                            x.die_offset != y.die_offset || x.tag != y.tag ||
                            x.name_present != y.name_present ||
                            x.name != y.name ||
                            x.linkage_present != y.linkage_present ||
                            x.linkage != y.linkage ||
                            x.declaration.file_present !=
                                    y.declaration.file_present ||
                            x.declaration.file != y.declaration.file ||
                            x.declaration.line_present !=
                                    y.declaration.line_present ||
                            x.declaration.line != y.declaration.line ||
                            x.declaration.column_present !=
                                    y.declaration.column_present ||
                            x.declaration.column != y.declaration.column ||
                            x.call.file_present != y.call.file_present ||
                            x.call.file != y.call.file ||
                            x.call.line_present != y.call.line_present ||
                            x.call.line != y.call.line ||
                            x.call.column_present != y.call.column_present ||
                            x.call.column != y.call.column)
                                return false;
                }
        }
        return true;
}

const frame_snapshot *
find_frame(const query_snapshot &q, uint64_t die_offset)
{
        for (auto &st : q)
                for (auto &f : st)
                        if (f.die_offset == die_offset)
                                return &f;
        return nullptr;
}

void
expect_absent(const query_snapshot &q, uint64_t die_offset,
              const string &what)
{
        require(find_frame(q, die_offset) == nullptr,
                what + ": DIE 0x" + to_string(die_offset) + " must be absent");
}

vector<uint64_t>
stack_offsets(const vector<frame_snapshot> &st)
{
        vector<uint64_t> r;
        for (auto &f : st)
                r.push_back(f.die_offset);
        return r;
}

void
expect_stack(const query_snapshot &q, size_t index,
             const vector<uint64_t> &expected, const string &what)
{
        require(index < q.size(), what + ": candidate index in range");
        require(stack_offsets(q[index]) == expected,
                what + ": candidate " + to_string(index)
                + " frame offset sequence");
}

} // namespace

int
main()
{
        try {
                auto loader = make_shared<fixture>();
                dwarf::dwarf dw(loader);
                auto &cus = dw.compilation_units();
                require(cus.size() == 2, "fixture parses into two CUs");

                // Independently collect concrete DIE identities from
                // the parsed tree.
                map<uint64_t, dwarf::die> nodes;
                vector<dwarf::die> stack;
                for (auto &cu : cus)
                        stack.push_back(cu.root());
                while (!stack.empty()) {
                        dwarf::die d = stack.back();
                        stack.pop_back();
                        nodes.emplace(d.get_section_offset(), d);
                        for (auto &child : d)
                                stack.push_back(child);
                }
                auto die_at = [&](const string &label) {
                        return nodes.at(loader->off.at(label));
                };
                auto off_of = [&](const string &label) {
                        return loader->off.at(label);
                };

                // The concrete tree and abstract ancestors are
                // different DIEs.
                require(die_at("ia1") != die_at("o1"),
                        "concrete instance differs from abstract origin");
                require(die_at("ia1").get_unit() !=
                                die_at("o1").get_unit(),
                        "concrete and abstract DIEs live in different CUs");

                // Reference forms: ref_addr across CUs, ref4 inside a
                // CU, multi-hop and diamond sharing.
                require(die_at("ia1")[dwarf::DW_AT::abstract_origin]
                                .as_reference() == die_at("o1"),
                        "ref_addr cross-CU abstract origin");
                require(die_at("o2")[dwarf::DW_AT::abstract_origin]
                                .as_reference() == die_at("o4"),
                        "ref4 CU-local abstract origin");
                require(die_at("d0")[dwarf::DW_AT::abstract_origin]
                                .as_reference() == die_at("o1"),
                        "diamond origin edge");
                require(die_at("d0")[dwarf::DW_AT::specification]
                                .as_reference() == die_at("o3"),
                        "diamond specification edge");
                require(die_at("o3")[dwarf::DW_AT::abstract_origin]
                                .as_reference() == die_at("o1"),
                        "multi-hop shared reference");

                const vector<uint64_t> pcs = {
                        0x1012, 0x1010, 0x1020, 0x1035, 0x1040, 0x1050,
                        0x1070, 0x1082, 0x1088, 0x10a2, 0x2005, 0x5000
                };
                map<uint64_t, query_snapshot> forward;
                for (auto pc : pcs)
                        forward[pc] = snapshot_query(
                                dwarf::inline_stacks(dw, pc));

                // ---- pc=0x1012: nesting, parallels, containment ----
                const query_snapshot &q1012 = forward.at(0x1012);
                require(q1012.size() == 4,
                        "0x1012 yields four independent candidates");
                expect_stack(q1012, 0,
                             {off_of("physA"), off_of("ia1"), off_of("ia2")},
                             "0x1012 deepest nested path");
                expect_stack(q1012, 1,
                             {off_of("physA"), off_of("ia1"), off_of("ia3")},
                             "0x1012 parallel inline path");
                expect_stack(q1012, 2,
                             {off_of("physA"), off_of("ic")},
                             "0x1012 range-list sibling path");
                expect_stack(q1012, 3,
                             {off_of("contained")},
                             "0x1012 non-ancestral contained function");
                // The lexical blocks are transparent and never frames.
                expect_absent(q1012, off_of("lb1"),
                              "0x1012 ranged lexical block");
                expect_absent(q1012, off_of("lb_transparent"),
                              "0x1012 transparent lexical block");
                // Short prefixes are not returned alongside deep paths.
                for (auto &st : q1012)
                        require(stack_offsets(st) !=
                                        vector<uint64_t>{off_of("physA")},
                                "0x1012 no short physA prefix");

                // Concrete override versus inherited metadata.
                const frame_snapshot *ia1 = find_frame(q1012, off_of("ia1"));
                require(ia1->name_present && ia1->name == "concrete_name",
                        "ia1 concrete name overrides origin");
                require(ia1->declaration.file_present &&
                                ia1->declaration.file == "/cuB/b1.cc",
                        "ia1 declaration file resolved in origin CU B");
                require(ia1->declaration.line_present &&
                                ia1->declaration.line == 7,
                        "ia1 inherited declaration line");
                require(ia1->declaration.column_present &&
                                ia1->declaration.column == 2,
                        "ia1 inherited declaration column");
                require(ia1->call.file_present &&
                                ia1->call.file == "/cuA/a1.cc",
                        "ia1 call file resolved in concrete CU A");
                require(ia1->call.line_present && ia1->call.line == 11 &&
                                ia1->call.column_present &&
                                ia1->call.column == 4,
                        "ia1 concrete call coordinates");

                const frame_snapshot *ia2 = find_frame(q1012, off_of("ia2"));
                require(ia2->name_present && ia2->name == "diamond",
                        "ia2 name inherited through multi-hop origin");
                require(ia2->linkage_present &&
                                ia2->linkage == "_Z7diamondv",
                        "ia2 linkage inherited from convergence DIE");
                require(!ia2->declaration.file_present &&
                                !ia2->declaration.line_present &&
                                !ia2->declaration.column_present,
                        "ia2 has no inheritable declaration coordinates");
                require(ia2->call.file == "/cuA/a2.cc" &&
                                ia2->call.line == 22 && ia2->call.column == 5,
                        "ia2 call file index 2 resolves in CU A");

                const frame_snapshot *ia3 = find_frame(q1012, off_of("ia3"));
                require(ia3->name_present && ia3->name == "origin5",
                        "ia3 name inherited");
                require(ia3->declaration.line_present &&
                                ia3->declaration.line == 0,
                        "ia3 concrete decl_line value 0 still present");
                require(!ia3->declaration.file_present &&
                                !ia3->declaration.column_present,
                        "ia3 missing declaration file and column");
                require(ia3->call.line_present && ia3->call.line == 33 &&
                                !ia3->call.column_present,
                        "ia3 call column missing stays unknown");

                const frame_snapshot *ic = find_frame(q1012, off_of("ic"));
                require(ic->name_present && ic->name == "callee",
                        "ic name inherited through diamond");
                require(ic->declaration.file == "/cuB/b1.cc",
                        "ic declaration file follows full origin branch");
                require(!ic->call.file_present && !ic->call.line_present &&
                                !ic->call.column_present,
                        "ic without call attributes has no call position");

                const frame_snapshot *phys =
                        find_frame(q1012, off_of("physA"));
                require(phys->name == "physA" &&
                                !phys->call.file_present &&
                                !phys->call.line_present &&
                                !phys->call.column_present,
                        "physical function has no call position");
                require(phys->tag == (unsigned)dwarf::DW_TAG::subprogram &&
                                ia1->tag ==
                                        (unsigned)dwarf::DW_TAG
                                                ::inlined_subroutine,
                        "frame tags distinguish function and inline");

                // ---- pc=0x1010: half-open lower boundary ----
                const query_snapshot &q1010 = forward.at(0x1010);
                require(q1010.size() == 2, "0x1010 candidate count");
                expect_stack(q1010, 0,
                             {off_of("physA"), off_of("ia1"), off_of("ia2")},
                             "0x1010 nested path included at lower bound");
                expect_stack(q1010, 1, {off_of("physA"), off_of("ic")},
                             "0x1010 range-list path");
                expect_absent(q1010, off_of("ia3"),
                              "0x1010 ia3 excludes its own lower bound");

                // ---- pc=0x1020: half-open upper boundary ----
                const query_snapshot &q1020 = forward.at(0x1020);
                require(q1020.size() == 2, "0x1020 candidate count");
                expect_stack(q1020, 0, {off_of("physA")},
                             "0x1020 physical function alone past upper bound");
                expect_stack(q1020, 1, {off_of("edge_fn")},
                             "0x1020 edge function starts at boundary");
                expect_absent(q1020, off_of("lb1"),
                              "0x1020 ranged block excludes upper bound");
                expect_absent(q1020, off_of("ic"),
                              "0x1020 range list excludes upper bound");

                // ---- pc=0x1035: range hole ----
                const query_snapshot &q1035 = forward.at(0x1035);
                require(q1035.size() == 1, "0x1035 candidate count");
                expect_stack(q1035, 0, {off_of("physA")},
                             "0x1035 hole yields physical function only");
                expect_absent(q1035, off_of("ic"),
                              "0x1035 range-list hole excludes ic");

                // ---- pc=0x1040: base address selection ----
                const query_snapshot &q1040 = forward.at(0x1040);
                require(q1040.size() == 1, "0x1040 candidate count");
                expect_stack(q1040, 0, {off_of("physA"), off_of("ic")},
                             "0x1040 base-selected range segment matches");

                // ---- pc=0x1050: second instance of the same origin ----
                const query_snapshot &q1050 = forward.at(0x1050);
                require(q1050.size() == 1, "0x1050 candidate count");
                expect_stack(q1050, 0, {off_of("physA"), off_of("ib")},
                             "0x1050 second concrete instance");
                const frame_snapshot *ib = find_frame(q1050, off_of("ib"));
                require(ib->name == "callee" &&
                                ib->declaration.file == "/cuB/b1.cc" &&
                                ib->declaration.line == 7,
                        "ib shares origin metadata with ia1");
                require(!ib->call.file_present && !ib->call.line_present &&
                                !ib->call.column_present,
                        "ib missing call position stays fully unknown");
                require(ib->die_offset != ia1->die_offset,
                        "same origin yields distinct concrete identities");

                // ---- pc=0x1070: range-less frame is a barrier ----
                const query_snapshot &q1070 = forward.at(0x1070);
                require(q1070.size() == 1, "0x1070 candidate count");
                expect_stack(q1070, 0, {off_of("physA")},
                             "0x1070 barrier prunes ranged child");
                expect_absent(q1070, off_of("ibar"),
                              "0x1070 range-less inline not a frame");
                expect_absent(q1070, off_of("ichild"),
                              "0x1070 child not stitched across barrier");

                // ---- lexical block range containment ----
                const query_snapshot &q1082 = forward.at(0x1082);
                require(q1082.size() == 1, "0x1082 candidate count");
                expect_stack(q1082, 0, {off_of("physA"), off_of("ix")},
                             "0x1082 child inline matches through block");
                const query_snapshot &q1088 = forward.at(0x1088);
                require(q1088.size() == 1, "0x1088 candidate count");
                expect_stack(q1088, 0, {off_of("physA")},
                             "0x1088 parent block range prunes child inline");
                expect_absent(q1088, off_of("ix"),
                              "0x1088 child range cannot extend past parent");

                // ---- pc=0x10a2: finite cycle in the origin graph ----
                const query_snapshot &q10a2 = forward.at(0x10a2);
                require(q10a2.size() == 1, "0x10a2 candidate count");
                expect_stack(q10a2, 0,
                             {off_of("physA"), off_of("icyc")},
                             "0x10a2 cycle-chain concrete frame");
                const frame_snapshot *icyc =
                        find_frame(q10a2, off_of("icyc"));
                require(icyc->name_present && icyc->name == "cycle_name",
                        "cycle back edge skipped, name still inherited");

                // ---- range-less CU scanning and no-match ----
                const query_snapshot &q2005 = forward.at(0x2005);
                require(q2005.size() == 1, "0x2005 candidate count");
                expect_stack(q2005, 0, {off_of("physB")},
                             "range-less CU still scanned for any PC");
                require(q2005[0][0].unit_offset == off_of("cuB"),
                        "physB frame carries CU B identity");
                const query_snapshot &q5000 = forward.at(0x5000);
                require(q5000.empty(),
                        "unmatched PC returns an empty candidate set");

                // ---- Metadata traversal unit: budget exhaustion ----
                // The finite origin chain behind ibudget visits more
                // than 256 distinct DIEs for every inherited
                // attribute, so the recognizable budget exception
                // must escape the query.
                bool budget_thrown = false;
                try {
                        dwarf::inline_stacks(dw, 0x10a6);
                } catch (const dwarf::inline_metadata_budget_exceeded &) {
                        budget_thrown = true;
                }
                require(budget_thrown,
                        "256-DIE metadata budget raises recognizable exception");

                // ---- Re-query in reverse order: no shared state ----
                for (auto it = pcs.rbegin(); it != pcs.rend(); ++it) {
                        query_snapshot again =
                                snapshot_query(dwarf::inline_stacks(dw, *it));
                        require(same_snapshot(again, forward.at(*it)),
                                "reverse re-query at 0x" +
                                to_string(*it) + " matches forward result");
                }

                unsigned total = 0;
                for (auto &kv : forward)
                        total += (unsigned)kv.second.size();
                printf("INLINE_STACKS_CONTRACT_OK checks=%u candidates=%u\n",
                       checks, total);
                return 0;
        } catch (const dwarf::inline_metadata_budget_exceeded &e) {
                fprintf(stderr, "UNEXPECTED budget exception: %s\n",
                        e.what());
                return 1;
        } catch (const exception &e) {
                fprintf(stderr, "INLINE_STACKS_CONTRACT_FAIL %s\n",
                        e.what());
                return 1;
        }
}
