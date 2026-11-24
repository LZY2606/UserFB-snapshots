// Offline variable inspection example.
//
// This never executes the analyzed ELF and never reads this process's
// own memory or registers.  All target state comes from the snapshot
// arguments: target addresses are treated as opaque numbers.
//
// Usage:
//   inspect-variable <elf> <cu-offset> <function-die-offset> \
//       <variable-die-offset> <pc> <width> <endian=little|big> \
//       [cfa=<hex>|cfa=-] [reg=<num>:<hex>]... [mem=<hexaddr>:<hexbytes>]...
//
// Example (test/golden-gcc-4.9.2/example, fib's parameter x):
//   inspect-variable example 0 0x2b 0x4c 0x4004bd 4 endian=little \
//       cfa=0x7000 mem=0x6fdc:2a000000

#include "elf++.hh"
#include "dwarf/dwarf++.hh"
#include "dwarf/variable.hh"
#include "elf/to_hex.hh"

#include <errno.h>
#include <fcntl.h>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace std;

// Public expr_context fed directly by the snapshot.  This is used to
// demonstrate the dwarf::at_location entry point; all values are
// target values supplied by the snapshot.
class example_context : public dwarf::expr_context
{
public:
        explicit example_context(const dwarf::variable_snapshot &snap)
                : snap_(snap) { }

        dwarf::taddr reg(unsigned regnum) override
        {
                return snap_.get_register(regnum);
        }

        dwarf::taddr cfa() override
        {
                return snap_.get_cfa();
        }

private:
        const dwarf::variable_snapshot &snap_;
};

static uint64_t
parse_num(const string &s)
{
        return strtoull(s.c_str(), nullptr, 0);
}

static vector<unsigned char>
parse_bytes(const string &hex)
{
        vector<unsigned char> out;
        if (hex.size() % 2 != 0)
                throw invalid_argument("hex byte string must have an even number of digits");
        for (size_t i = 0; i < hex.size(); i += 2) {
                char *end = nullptr;
                string pair = hex.substr(i, 2);
                unsigned long b = strtoul(pair.c_str(), &end, 16);
                if (*end != '\0')
                        throw invalid_argument("invalid hex byte '" + pair + "'");
                out.push_back((unsigned char)b);
        }
        return out;
}

static string
hex_bytes(const vector<unsigned char> &bytes)
{
        string s;
        static const char d[] = "0123456789abcdef";
        for (unsigned char b : bytes) {
                s.push_back(d[b >> 4]);
                s.push_back(d[b & 0xf]);
        }
        return s;
}

static void
usage(const char *cmd)
{
        cerr << "usage: " << cmd << " <elf> <cu-offset> <function-die-offset> "
                "<variable-die-offset> <pc> <width> endian=<little|big>\n"
                "       [cfa=<hex>|cfa=-] [reg=<num>:<hex>]... "
                "[mem=<hexaddr>:<hexbytes>]...\n";
        exit(2);
}

int
main(int argc, char **argv)
{
        if (argc < 8)
                usage(argv[0]);

        const char *path = argv[1];
        dwarf::section_offset cu_off = parse_num(argv[2]);
        dwarf::section_offset func_off = parse_num(argv[3]);
        dwarf::section_offset var_off = parse_num(argv[4]);
        dwarf::taddr pc = parse_num(argv[5]);
        unsigned width = (unsigned)parse_num(argv[6]);

        dwarf::snapshot_endian order = dwarf::snapshot_endian::little;
        bool have_cfa = false;
        dwarf::taddr cfa = 0;
        struct reg_spec { unsigned num; dwarf::taddr value; };
        vector<reg_spec> regs;
        struct mem_spec { dwarf::taddr addr; vector<unsigned char> bytes; };
        vector<mem_spec> mems;

        for (int i = 7; i < argc; i++) {
                string arg = argv[i];
                if (arg.compare(0, 7, "endian=") == 0) {
                        string v = arg.substr(7);
                        if (v == "little")
                                order = dwarf::snapshot_endian::little;
                        else if (v == "big")
                                order = dwarf::snapshot_endian::big;
                        else
                                usage(argv[0]);
                } else if (arg.compare(0, 4, "cfa=") == 0) {
                        string v = arg.substr(4);
                        if (v != "-") {
                                have_cfa = true;
                                cfa = parse_num(v);
                        }
                } else if (arg.compare(0, 4, "reg=") == 0) {
                        string v = arg.substr(4);
                        size_t colon = v.find(':');
                        if (colon == string::npos)
                                usage(argv[0]);
                        reg_spec r;
                        r.num = (unsigned)parse_num(v.substr(0, colon));
                        r.value = parse_num(v.substr(colon + 1));
                        regs.push_back(r);
                } else if (arg.compare(0, 4, "mem=") == 0) {
                        string v = arg.substr(4);
                        size_t colon = v.find(':');
                        if (colon == string::npos)
                                usage(argv[0]);
                        mem_spec m;
                        m.addr = parse_num(v.substr(0, colon));
                        m.bytes = parse_bytes(v.substr(colon + 1));
                        mems.push_back(move(m));
                } else {
                        usage(argv[0]);
                }
        }

        int fd = open(path, O_RDONLY);
        if (fd < 0) {
                cerr << path << ": " << strerror(errno) << "\n";
                return 1;
        }

        elf::elf ef(elf::create_mmap_loader(fd));
        dwarf::dwarf dw(dwarf::elf::create_loader(ef));

        const dwarf::compilation_unit &cu =
                dwarf::compilation_unit_at(dw, cu_off);
        dwarf::die function = dwarf::die_at(cu, func_off);
        dwarf::die variable = dwarf::die_at(cu, var_off);

        dwarf::variable_snapshot snap(order);
        if (have_cfa)
                snap.set_cfa(cfa);
        for (auto &r : regs)
                snap.set_register(r.num, r.value);
        for (auto &m : mems)
                snap.add_memory_segment(m.addr, m.bytes);

        dwarf::variable_result r =
                dwarf::inspect_variable(variable, function, pc, width, snap);

        // Also exercise the public location-expression entry point
        // directly.  This shows the raw DWARF location description
        // before the snapshot layer turns it into a concrete value.
        {
                example_context ectx(snap);
                try {
                        dwarf::expr_result er =
                                dwarf::at_location(variable, function, pc, &ectx);
                        cout << "at_location="
                             << dwarf::to_string(er.location_type)
                             << " value=0x" << to_hex(er.value) << "\n";
                } catch (const dwarf::expr_error &e) {
                        cout << "at_location_error=" << e.what() << "\n";
                }
        }

        cout << "status=" << dwarf::to_string(r.status) << "\n";
        if (r.status == dwarf::variable_status::value) {
                cout << "value=" << r.value
                     << " (0x" << to_hex(r.value) << ")\n";
                cout << "raw=" << hex_bytes(r.raw_bytes) << "\n";
        }
        if (!r.error.empty())
                cout << "error=" << r.error << "\n";

        cout << "location_die=0x" << to_hex(r.location_die_offset)
             << " list=" << (r.location_is_list ? "yes" : "no")
             << " frame_base_used=" << (r.frame_base_used ? "yes" : "no");
        if (r.frame_base_used)
                cout << " frame_base_die=0x" << to_hex(r.frame_base_die_offset);
        cout << "\n";

        if (r.selected) {
                cout << "range=[0x" << to_hex(r.range_low)
                     << ",0x" << to_hex(r.range_high) << ")\n";
                cout << "expr=" << dwarf::to_string(r.expr_section)
                     << "+0x" << to_hex(r.expr_offset)
                     << " len=" << r.expr_length
                     << " empty=" << (r.expr_empty ? "yes" : "no") << "\n";
        }

        if (r.cfa_accessed)
                cout << "cfa=0x" << to_hex(r.cfa_value) << "\n";
        for (auto &a : r.registers)
                cout << "reg=" << a.regnum
                     << " value=0x" << to_hex(a.value) << "\n";
        if (r.has_value_address)
                cout << "address=0x" << to_hex(r.value_address) << "\n";
        for (auto &m : r.memory)
                cout << "memory=0x" << to_hex(m.address)
                     << ":" << hex_bytes(m.bytes) << "\n";

        return r.status == dwarf::variable_status::value ? 0 : 1;
}
