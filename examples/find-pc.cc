#include "elf++.hh"
#include "dwarf++.hh"
#include "inline.hh"

#include <errno.h>
#include <fcntl.h>
#include <string>
#include <inttypes.h>

using namespace std;

void
usage(const char *cmd)
{
        fprintf(stderr, "usage: %s elf-file pc\n", cmd);
        exit(2);
}

// Original single-branch concrete-tree traversal.  Kept as a
// reference path for the offline baseline comparison; the command
// itself reports every candidate through dwarf::inline_stacks.
bool
find_pc(const dwarf::die &d, dwarf::taddr pc, vector<dwarf::die> *stack)
{
        using namespace dwarf;

        // Scan children first to find most specific DIE
        bool found = false;
        for (auto &child : d) {
                if ((found = find_pc(child, pc, stack)))
                        break;
        }
        switch (d.tag) {
        case DW_TAG::subprogram:
        case DW_TAG::inlined_subroutine:
                try {
                        if (found || die_pc_range(d).contains(pc)) {
                                found = true;
                                stack->push_back(d);
                        }
                } catch (out_of_range &e) {
                } catch (value_type_mismatch &e) {
                }
                break;
        default:
                break;
        }
        return found;
}

static string
field_or_unknown(bool present, const string &value)
{
        if (!present)
                return "unknown";
        return value;
}

static string
number_or_unknown(bool present, uint64_t value)
{
        if (!present)
                return "unknown";
        return to_string(value);
}

static string
format_position(const dwarf::inline_source_position &pos)
{
        // Every field is independent; an unknown file, line, or
        // column is printed as unknown rather than backfilled.
        return field_or_unknown(pos.file_present, pos.file) + ":"
               + number_or_unknown(pos.line_present, pos.line) + ":"
               + number_or_unknown(pos.column_present, pos.column);
}

static void
print_frame(const dwarf::inline_frame &frame, unsigned depth)
{
        printf("  frame %u: cu=0x%" PRIx64 " die=0x%" PRIx64
               " tag=%s\n",
               depth, frame.unit_offset, frame.die_offset,
               to_string(frame.tag).c_str());
        printf("    name: %s\n",
               field_or_unknown(frame.name_present, frame.name).c_str());
        printf("    linkage_name: %s\n",
               field_or_unknown(frame.linkage_name_present,
                                frame.linkage_name).c_str());
        printf("    declaration: %s\n",
               format_position(frame.declaration).c_str());
        printf("    call: %s\n",
               format_position(frame.call).c_str());
}

int
main(int argc, char **argv)
{
        if (argc != 3)
                usage(argv[0]);

        dwarf::taddr pc;
        try {
                pc = stoll(argv[2], nullptr, 0);
        } catch (invalid_argument &e) {
                usage(argv[0]);
        } catch (out_of_range &e) {
                usage(argv[0]);
        }

        int fd = open(argv[1], O_RDONLY);
        if (fd < 0) {
                fprintf(stderr, "%s: %s\n", argv[1], strerror(errno));
                return 1;
        }

        elf::elf ef(elf::create_mmap_loader(fd));
        dwarf::dwarf dw(dwarf::elf::create_loader(ef));

        vector<dwarf::inline_stack> stacks = dwarf::inline_stacks(dw, pc);
        printf("pc=0x%" PRIx64 " candidates=%zu\n", pc, stacks.size());
        if (stacks.empty())
                printf("no inline stack covers this pc\n");

        for (size_t i = 0; i < stacks.size(); ++i) {
                printf("candidate %zu:\n", i + 1);
                for (size_t j = 0; j < stacks[i].frames.size(); ++j)
                        print_frame(stacks[i].frames[j], (unsigned)j);
                if (i + 1 < stacks.size())
                        printf("---\n");
        }

        return 0;
}
