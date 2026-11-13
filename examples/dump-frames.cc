#include "elf++.hh"
#include "dwarf++.hh"
#include "frame.hh"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

using namespace std;

static const char *
rule_type_name(dwarf::frame_rule_type t)
{
        switch (t) {
        case dwarf::frame_rule_type::undefined: return "undefined";
        case dwarf::frame_rule_type::same_value: return "same_value";
        case dwarf::frame_rule_type::offset: return "offset";
        case dwarf::frame_rule_type::val_offset: return "val_offset";
        case dwarf::frame_rule_type::reg: return "register";
        }
        return "?";
}

int
main(int argc, char **argv)
{
        if (argc != 2) {
                fprintf(stderr, "usage: %s elf-file\n", argv[0]);
                return 2;
        }

        int fd = open(argv[1], O_RDONLY);
        if (fd < 0) {
                fprintf(stderr, "%s: %s\n", argv[1], strerror(errno));
                return 1;
        }

        elf::elf ef(elf::create_mmap_loader(fd));
        dwarf::dwarf dw(dwarf::elf::create_loader(ef));

        unsigned addr_size =
                ef.get_hdr().ei_class == elf::elfclass::_64 ? 8 : 4;
        dwarf::frame_table ft;
        try {
                ft = dwarf::frame_table(dw, addr_size);
        } catch (dwarf::format_error &e) {
                fprintf(stderr, "%s: %s\n", argv[1], e.what());
                return 1;
        }
        for (auto &fde : ft.fdes()) {
                printf("FDE [%#" PRIx64 ", %#" PRIx64 ")\n",
                       (uint64_t)fde.initial_location, (uint64_t)fde.end());
                for (auto &row : fde.rows) {
                        printf("  [%#" PRIx64 ", %#" PRIx64 ")",
                               (uint64_t)row.begin, (uint64_t)row.end);
                        if (row.cfa.defined)
                                printf(" CFA=r%" PRIu64 "%+" PRId64,
                                       row.cfa.reg, row.cfa.offset);
                        else
                                printf(" CFA=undefined");
                        for (auto &kv : row.registers) {
                                printf(" r%" PRIu64 "=%s", kv.first,
                                       rule_type_name(kv.second.type));
                                if (kv.second.type == dwarf::frame_rule_type::offset ||
                                    kv.second.type == dwarf::frame_rule_type::val_offset)
                                        printf("(%+" PRId64 ")", kv.second.offset);
                                if (kv.second.type == dwarf::frame_rule_type::reg)
                                        printf("(r%" PRIu64 ")", kv.second.reg);
                        }
                        printf("\n");
                }
        }

        return 0;
}
