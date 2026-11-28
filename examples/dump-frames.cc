// Print the call frame information of an ELF file's .debug_frame
// section, and demonstrate relocating it for moved code segments.

#include "elf++.hh"
#include "dwarf++.hh"
#include "frame.hh"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <string.h>

using namespace std;

static const char *
rule_kind_name(dwarf::frame_reg_kind k)
{
        switch (k) {
        case dwarf::frame_reg_kind::undefined:  return "undefined";
        case dwarf::frame_reg_kind::same_value: return "same_value";
        case dwarf::frame_reg_kind::offset:     return "offset";
        case dwarf::frame_reg_kind::val_offset: return "val_offset";
        case dwarf::frame_reg_kind::reg:        return "reg";
        }
        return "?";
}

static void
dump_rows(const dwarf::frame_table &ft)
{
        for (auto &row : ft.rows()) {
                printf("  [0x%08" PRIx64 ", 0x%08" PRIx64 ")", row.begin, row.end);
                if (row.cfa.defined)
                        printf(" cfa=r%" PRIu64 "%+" PRId64, row.cfa.reg,
                               row.cfa.offset);
                else
                        printf(" cfa=undefined");
                for (auto &kv : row.regs) {
                        printf(" r%" PRIu64 "=", kv.first);
                        switch (kv.second.kind) {
                        case dwarf::frame_reg_kind::offset:
                                printf("cfa%+d", (int)kv.second.offset);
                                break;
                        case dwarf::frame_reg_kind::val_offset:
                                printf("val(cfa%+d)", (int)kv.second.offset);
                                break;
                        case dwarf::frame_reg_kind::reg:
                                printf("r%" PRIu64, kv.second.reg);
                                break;
                        default:
                                printf("%s", rule_kind_name(kv.second.kind));
                        }
                }
                printf("\n");
        }
}

int
main(int argc, char **argv)
{
        if (argc < 2 || argc > 3) {
                fprintf(stderr, "usage: %s elf-file [addr-size]\n", argv[0]);
                return 2;
        }
        unsigned addr_size = argc == 3 ? atoi(argv[2]) : 8;
        if (addr_size != 4 && addr_size != 8) {
                fprintf(stderr, "addr-size must be 4 or 8\n");
                return 2;
        }

        int fd = open(argv[1], O_RDONLY);
        if (fd < 0) {
                fprintf(stderr, "%s: %s\n", argv[1], strerror(errno));
                return 1;
        }

        elf::elf ef(elf::create_mmap_loader(fd));
        dwarf::dwarf dw(dwarf::elf::create_loader(ef));

        try {
                dwarf::frame_table ft(dw, addr_size);
                printf("%zu row(s) of frame information:\n", ft.rows().size());
                dump_rows(ft);
        } catch (const dwarf::format_error &e) {
                fprintf(stderr, "cannot read .debug_frame: %s\n", e.what());
                return 1;
        }
        return 0;
}
