/*
 * frame_example.c - minimal caller-provided CFG and frame report example.
 *
 * Build from the repository root using task_support/build.sh, or directly
 * with cc by compiling every C file in libudis86 together with
 * task_support/generated/libudis86/itab.c and this example.
 */
#include "libudis86/frame.h"
#include "udis86.h"

#include <stdint.h>
#include <stdio.h>

static void
print_value(const char *name, ud_frame_value_t value)
{
    if (value.kind == UD_FRAME_RELATIVE)
        printf("  %s = S%+lld\n", name, (long long)value.offset);
    else if (value.kind == UD_FRAME_UNKNOWN)
        printf("  %s = unknown\n", name);
    else
        printf("  %s = unreached\n", name);
}

int
main(void)
{
    /*
     * push rbp
     * mov  rbp, rsp
     * sub  rsp, 0x20
     * mov  rax, [rbp-8]
     * leave
     * ret
     */
    static const uint8_t code[] = {
        0x55,
        0x48, 0x89, 0xe5,
        0x48, 0x83, 0xec, 0x20,
        0x48, 0x8b, 0x45, 0xf8,
        0xc9,
        0xc3
    };

    ud_frame_block_t block;
    ud_frame_graph_t graph;
    ud_frame_error_t error;
    ud_frame_report_t *report;
    size_t i;

    block.begin = 0;
    block.end = sizeof code;
    block.successors = NULL;
    block.successor_count = 0;
    graph.blocks = &block;
    graph.block_count = 1;
    graph.entry = 0;

    report = ud_frame_analyze(code, sizeof code, &graph, &error);
    if (report == NULL) {
        fprintf(stderr, "frame analysis failed in block %zu offset %zu: %s\n",
                error.block, error.offset, error.message);
        return 1;
    }

    for (i = 0; i < report->insn_count; ++i) {
        const ud_frame_insn_t *insn = &report->insns[i];
        const char *name =
            ud_lookup_mnemonic((enum ud_mnemonic_code)insn->mnemonic);
        size_t j;

        printf("%04zx: %-8s before:\n", insn->offset,
               name != NULL ? name : "?");
        print_value("rsp", insn->before.rsp);
        print_value("rbp", insn->before.rbp);

        for (j = 0; j < insn->access_count; ++j) {
            const ud_frame_access_t *a = &insn->accesses[j];
            if (a->kind == UD_FRAME_ACCESS_LEA)
                printf("    address calculation: ");
            else
                printf("    %s %u-byte slot: ",
                       (a->direction & UD_FRAME_ACCESS_WRITE) ? "write"
                                                              : "read",
                       a->width);
            if (a->address_known)
                printf("S%+lld\n", (long long)a->address);
            else
                printf("unknown address\n");
        }
    }

    ud_frame_report_free(report);
    return 0;
}
