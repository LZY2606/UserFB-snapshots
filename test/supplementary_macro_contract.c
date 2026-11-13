/* SPDX-License-Identifier: MIT */
/*
    Contract tests for DWARF5 .debug_macro data that lives in a
    supplementary (tied) object with no compilation units, and for
    the new libdwarf entry dwarf_get_macro_context_by_debug().

    No main() here: task_support/driver.c runs the cases registered
    in task_cases[] below.  Fixtures are tiny ELF64LE objects written
    in C (never executed).  The real dwarfdump binary is exercised
    through the TASK_DWARFDUMP environment variable.
*/
#include "check.h"
#include "fixture.h"
#include "libdwarf.h"
#include "dwarf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*  ---------- tiny ELF64LE writer (objects are data, never run) ---- */
struct smc_section {
    const char *name;
    const unsigned char *data;
    size_t size;
    unsigned nameoff;
    unsigned off;
};
static void smc_put(unsigned char *p, unsigned long long v, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; ++i) {
        p[i] = (unsigned char)v;
        v >>= 8;
    }
}
static int smc_write_elf(const char *path,
    struct smc_section *s, unsigned count)
{
    unsigned char data[8192];
    unsigned char names[256];
    unsigned i, no = 1, pos = 64, shoff, n = count + 2;
    FILE *f;

    memset(data, 0, sizeof(data));
    memset(names, 0, sizeof(names));
    for (i = 0; i < count; ++i) {
        s[i].nameoff = no;
        memcpy(names + no, s[i].name, strlen(s[i].name) + 1);
        no += (unsigned)strlen(s[i].name) + 1;
        s[i].off = pos;
        memcpy(data + pos, s[i].data, s[i].size);
        pos += (unsigned)s[i].size;
    }
    {
        unsigned nameoff = pos, shname = no;
        memcpy(names + no, ".shstrtab", 10);
        no += 10;
        memcpy(data + pos, names, no);
        pos += no;
        shoff = (pos + 7) & ~7u;
        memcpy(data, "\177ELF\2\1\1", 7);
        smc_put(data + 16, 2, 2);
        smc_put(data + 18, 62, 2);
        smc_put(data + 20, 1, 4);
        smc_put(data + 40, shoff, 8);
        smc_put(data + 52, 64, 2);
        smc_put(data + 58, 64, 2);
        smc_put(data + 60, n, 2);
        smc_put(data + 62, n - 1, 2);
        for (i = 0; i < count; ++i) {
            unsigned char *h = data + shoff + 64 * (i + 1);
            smc_put(h, s[i].nameoff, 4);
            smc_put(h + 4, 1, 4);
            smc_put(h + 24, s[i].off, 8);
            smc_put(h + 32, s[i].size, 8);
            smc_put(h + 48, 1, 8);
        }
        {
            unsigned char *h = data + shoff + 64 * (n - 1);
            smc_put(h, shname, 4);
            smc_put(h + 4, 3, 4);
            smc_put(h + 24, nameoff, 8);
            smc_put(h + 32, no, 8);
            smc_put(h + 48, 1, 8);
        }
    }
    f = fopen(path, "wb");
    if (!f) {
        return 1;
    }
    i = fwrite(data, 1, shoff + 64 * n, f) == shoff + 64 * n ? 0 : 1;
    return fclose(f) ? 1 : (int)i;
}

/*  ---------- shared fixture bytes ---------- */
/*  abbrev: code 1, DW_TAG_compile_unit, no children,
    DW_AT_name/DW_FORM_string, DW_AT_macros/DW_FORM_sec_offset. */
static const unsigned char smc_abbrev[] =
    {1, 0x11, 0, 0x03, 0x08, 0x79, 0x17, 0, 0, 0};
/*  abbrev: code 1, DW_TAG_compile_unit, no children,
    DW_AT_name/DW_FORM_string only. */
static const unsigned char smc_abbrev_nameonly[] =
    {1, 0x11, 0, 0x03, 0x08, 0, 0, 0};

/*  mainB: CU "mb.c", macros at 0.
    macro unit: import_sup 0; undef K; import_sup 0; define "K 2". */
static const unsigned char smc_mainb_info[] =
    {18, 0, 0, 0, 5, 0, 1, 8, 0, 0, 0, 0,
    1, 'm', 'b', '.', 'c', 0, 0, 0, 0, 0};
static const unsigned char smc_mainb_mac[] =
    {5, 0, 0,
    0x0a, 0, 0, 0, 0,              /* import_sup 0 */
    0x02, 1, 'K', 0,               /* undef K */
    0x0a, 0, 0, 0, 0,              /* import_sup 0 again */
    0x01, 1, 'K', ' ', '2', 0,     /* define "K 2" */
    0};
/*  supB: unit at 0: define "K 1"; import 15.
    unit at 15: define "J 2".  A local (ordinary) import
    wholly inside the supplementary object. */
static const unsigned char smc_supb_mac[] =
    {5, 0, 0,
    0x01, 1, 'K', ' ', '1', 0,     /* define "K 1" */
    0x07, 15, 0, 0, 0,             /* import 15 (local) */
    0,
    5, 0, 0,                       /* unit at offset 15 */
    0x01, 1, 'J', ' ', '2', 0,     /* define "J 2" */
    0};

/*  mainC: CU "mc.c", macros at 0.
    macro unit: import_sup 0 (target unsupported); define "AFTER 1". */
static const unsigned char smc_mainc_info[] =
    {18, 0, 0, 0, 5, 0, 1, 8, 0, 0, 0, 0,
    1, 'm', 'c', '.', 'c', 0, 0, 0, 0, 0};
static const unsigned char smc_mainc_mac[] =
    {5, 0, 0,
    0x0a, 0, 0, 0, 0,              /* import_sup 0 */
    0x01, 1, 'A', 'F', 'T', 'E', 'R', ' ', '1', 0,
    0};
/*  supC: unit at 0 has DW_MACRO_start_file (unsupported
    without a CU), unit at 7 is version 4 (unsupported),
    unit at 11 has DW_MACRO_define_strx (unsupported). */
static const unsigned char smc_supc_mac[] =
    {5, 0, 0,
    0x03, 1, 0,                    /* start_file: unsupported */
    0,
    4, 0, 0,                       /* version 4: unsupported */
    0,
    5, 0, 0,
    0x0b, 1, 0,                    /* define_strx: unsupported */
    0};

/*  mainE: two CUs.  CU1 "c1" macros at 0 (import_sup 0),
    CU2 "c2" macros at 9 (define "CU2MAC 1"). */
static const unsigned char smc_maine_info[] =
    {16, 0, 0, 0, 5, 0, 1, 8, 0, 0, 0, 0,
    1, 'c', '1', 0, 0, 0, 0, 0,
    16, 0, 0, 0, 5, 0, 1, 8, 0, 0, 0, 0,
    1, 'c', '2', 0, 9, 0, 0, 0};
static const unsigned char smc_maine_mac[] =
    {5, 0, 0,
    0x0a, 0, 0, 0, 0,              /* CU1: import_sup 0 */
    0,
    5, 0, 0,                       /* CU2 unit at offset 9 */
    0x01, 1, 'C', 'U', '2', 'M', 'A', 'C', ' ', '1', 0,
    0};
/*  supE: unit at 0 imports offset 0: a genuine cycle
    within the supplementary object. */
static const unsigned char smc_supe_mac[] =
    {5, 0, 0,
    0x07, 0, 0, 0, 0,              /* import 0 (self) */
    0};

/*  nomac: a CU but no .debug_macro section at all. */
static const unsigned char smc_nomac_info[] =
    {12, 0, 0, 0, 5, 0, 1, 8, 0, 0, 0, 0,
    1, 'n', 'm', 0};

static const char *smc_build(void)
{
    const char *b = getenv("TASK_BUILD");
    return b ? b : ".task-build";
}
static const char *smc_dwarfdump(void)
{
    return getenv("TASK_DWARFDUMP");
}

/*  Write all the custom fixtures.  Returns nonzero on failure. */
static int smc_make_fixtures(void)
{
    char path[4096];
    const char *dir = smc_build();

    if (task_basic_fixtures(dir)) {
        return 1;
    }
    {
        struct smc_section secs[] = {
            {".debug_abbrev", smc_abbrev, sizeof(smc_abbrev), 0, 0},
            {".debug_info", smc_mainb_info, sizeof(smc_mainb_info),
                0, 0},
            {".debug_macro", smc_mainb_mac, sizeof(smc_mainb_mac),
                0, 0}};
        if (snprintf(path, sizeof(path), "%s/mainb.elf", dir) >=
            (int)sizeof(path)) {
            return 1;
        }
        if (smc_write_elf(path, secs, 3)) {
            return 1;
        }
    }
    {
        struct smc_section secs[] = {
            {".debug_macro", smc_supb_mac, sizeof(smc_supb_mac),
                0, 0}};
        if (snprintf(path, sizeof(path), "%s/supb.elf", dir) >=
            (int)sizeof(path)) {
            return 1;
        }
        if (smc_write_elf(path, secs, 1)) {
            return 1;
        }
    }
    {
        struct smc_section secs[] = {
            {".debug_abbrev", smc_abbrev, sizeof(smc_abbrev), 0, 0},
            {".debug_info", smc_mainc_info, sizeof(smc_mainc_info),
                0, 0},
            {".debug_macro", smc_mainc_mac, sizeof(smc_mainc_mac),
                0, 0}};
        if (snprintf(path, sizeof(path), "%s/mainc.elf", dir) >=
            (int)sizeof(path)) {
            return 1;
        }
        if (smc_write_elf(path, secs, 3)) {
            return 1;
        }
    }
    {
        struct smc_section secs[] = {
            {".debug_macro", smc_supc_mac, sizeof(smc_supc_mac),
                0, 0}};
        if (snprintf(path, sizeof(path), "%s/supc.elf", dir) >=
            (int)sizeof(path)) {
            return 1;
        }
        if (smc_write_elf(path, secs, 1)) {
            return 1;
        }
    }
    {
        struct smc_section secs[] = {
            {".debug_abbrev", smc_abbrev, sizeof(smc_abbrev), 0, 0},
            {".debug_info", smc_maine_info, sizeof(smc_maine_info),
                0, 0},
            {".debug_macro", smc_maine_mac, sizeof(smc_maine_mac),
                0, 0}};
        if (snprintf(path, sizeof(path), "%s/maine.elf", dir) >=
            (int)sizeof(path)) {
            return 1;
        }
        if (smc_write_elf(path, secs, 3)) {
            return 1;
        }
    }
    {
        struct smc_section secs[] = {
            {".debug_macro", smc_supe_mac, sizeof(smc_supe_mac),
                0, 0}};
        if (snprintf(path, sizeof(path), "%s/supe.elf", dir) >=
            (int)sizeof(path)) {
            return 1;
        }
        if (smc_write_elf(path, secs, 1)) {
            return 1;
        }
    }
    {
        struct smc_section secs[] = {
            {".debug_abbrev", smc_abbrev_nameonly,
                sizeof(smc_abbrev_nameonly), 0, 0},
            {".debug_info", smc_nomac_info, sizeof(smc_nomac_info),
                0, 0}};
        if (snprintf(path, sizeof(path), "%s/nomac.elf", dir) >=
            (int)sizeof(path)) {
            return 1;
        }
        if (smc_write_elf(path, secs, 2)) {
            return 1;
        }
    }
    return 0;
}

/*  ---------- dwarfdump runner and output scanner ---------- */
static int smc_run_dwarfdump(const char *args, const char *outpath)
{
    char cmd[8192];
    const char *dd = smc_dwarfdump();

    if (!dd) {
        return -1;
    }
    if (snprintf(cmd, sizeof(cmd),
        "\"%s\" --file-name=src/bin/dwarfdump/dwarfdump.conf"
        " %s > \"%s\" 2>&1",
        dd, args, outpath) >= (int)sizeof(cmd)) {
        return -1;
    }
    return system(cmd);
}

/*  Reads the whole file, NUL terminates it.  Caller frees. */
static char *smc_read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    long len;
    char *buf;
    if (!f) {
        return 0;
    }
    if (fseek(f, 0, SEEK_END)) {
        fclose(f);
        return 0;
    }
    len = ftell(f);
    if (len < 0) {
        fclose(f);
        return 0;
    }
    if (fseek(f, 0, SEEK_SET)) {
        fclose(f);
        return 0;
    }
    buf = (char *)malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return 0;
    }
    if (len && fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf);
        fclose(f);
        return 0;
    }
    buf[len] = 0;
    fclose(f);
    return buf;
}

static int smc_text_has(const char *haystack, const char *needle)
{
    return haystack && strstr(haystack, needle) != 0;
}

/*  ---------- group: owner_context ---------- */

/*  The CU-less library entry reads the supplementary object's
    own macro section and its own strings; the same offset in
    a different object is a different macro unit. */
static int smc_case_cu_less_context(void)
{
    Dwarf_Debug dbg = 0, sup = 0, tied = 0;
    Dwarf_Die die = 0;
    Dwarf_Error error = 0;
    Dwarf_Macro_Context mc = 0;
    Dwarf_Unsigned version = 0, off = 0, count = 0, len = 0;
    Dwarf_Unsigned k = 0, lineno = 0, index = 0, stroff = 0;
    Dwarf_Half op = 0, forms = 0;
    const Dwarf_Small *form = 0;
    const char *text = 0;
    char mainpath[4096], suppath[4096];
    unsigned supdefs = 0, maindefs = 0;
    int res = 0;

    TASK_CHECK(smc_make_fixtures() == 0);
    TASK_CHECK(snprintf(mainpath, sizeof(mainpath),
        "%s/main.elf", smc_build()) < (int)sizeof(mainpath));
    TASK_CHECK(snprintf(suppath, sizeof(suppath),
        "%s/sup.elf", smc_build()) < (int)sizeof(suppath));
    TASK_CHECK(dwarf_init_path(mainpath, 0, 0,
        DW_GROUPNUMBER_ANY, 0, 0, &dbg, &error) == DW_DLV_OK);
    TASK_CHECK(dwarf_init_path(suppath, 0, 0,
        DW_GROUPNUMBER_ANY, 0, 0, &sup, &error) == DW_DLV_OK);
    TASK_CHECK(dwarf_set_tied_dbg(dbg, sup, &error) == DW_DLV_OK);
    TASK_CHECK(dwarf_get_tied_dbg(dbg, &tied, &error) == DW_DLV_OK);
    TASK_CHECK(tied == sup);
    TASK_CHECK(dwarf_offdie_b(dbg, 12, 1, &die, &error) == DW_DLV_OK);

    /*  The main object's macro unit at offset 0 via its CU. */
    TASK_CHECK(dwarf_get_macro_context(die, &version, &mc,
        &off, &count, &len, &error) == DW_DLV_OK);
    TASK_CHECK(version == 5 && off == 0);
    for (k = 0; k < count; ++k) {
        TASK_CHECK(dwarf_get_macro_op(mc, k, &off, &op,
            &forms, &form, &error) == DW_DLV_OK);
        if (op == DW_MACRO_define_strp) {
            TASK_CHECK(dwarf_get_macro_defundef(mc, k,
                &lineno, &index, &stroff, &forms, &text,
                &error) == DW_DLV_OK);
            TASK_CHECK(!strcmp(text, "MAIN_OWNER 1"));
            ++maindefs;
        }
    }
    TASK_CHECK(maindefs == 1);
    dwarf_dealloc_macro_context(mc);
    mc = 0;

    /*  The supplementary object's macro unit, also at offset 0,
        needs no CU at all.  Its strp string comes from the
        supplementary object's own .debug_str, even though the
        main object has a different string at that same
        .debug_str offset. */
    TASK_CHECK(dwarf_get_macro_context_by_debug(sup, 0,
        &version, &mc, &count, &len, &error) == DW_DLV_OK);
    TASK_CHECK(version == 5);
    TASK_CHECK(count == 2); /* one define plus terminator */
    for (k = 0; k < count; ++k) {
        TASK_CHECK(dwarf_get_macro_op(mc, k, &off, &op,
            &forms, &form, &error) == DW_DLV_OK);
        if (op == DW_MACRO_define_strp) {
            TASK_CHECK(dwarf_get_macro_defundef(mc, k,
                &lineno, &index, &stroff, &forms, &text,
                &error) == DW_DLV_OK);
            TASK_CHECK(!strcmp(text, "SUP_OWNER 9"));
            ++supdefs;
        }
    }
    TASK_CHECK(supdefs == 1);
    dwarf_dealloc_macro_context(mc);
    mc = 0;

    /*  Same offset 0 in the main object through the CU-less
        entry: the main unit uses start_file/import_sup, which
        the CU-less entry must refuse, not reinterpret. */
    res = dwarf_get_macro_context_by_debug(dbg, 0,
        &version, &mc, &count, &len, &error);
    TASK_CHECK(res == DW_DLV_ERROR);
    if (res == DW_DLV_ERROR && error) {
        dwarf_dealloc_error(dbg, error);
        error = 0;
    }

    dwarf_dealloc_die(die);
    TASK_CHECK(dwarf_finish(dbg) == DW_DLV_OK);
    TASK_CHECK(dwarf_finish(sup) == DW_DLV_OK);
    return 0;
}

/*  dwarfdump --print-macinfo follows DW_MACRO_import_sup into
    the tied object and labels which object each unit is from. */
static int smc_case_print_labels_owner(void)
{
    char args[4096], outpath[4096];
    char *out = 0;
    int rc;

    TASK_CHECK(smc_make_fixtures() == 0);
    TASK_CHECK(smc_dwarfdump() != 0);
    TASK_CHECK(snprintf(args, sizeof(args),
        "--print-macinfo --file-tied=\"%s/sup.elf\" \"%s/main.elf\"",
        smc_build(), smc_build()) < (int)sizeof(args));
    TASK_CHECK(snprintf(outpath, sizeof(outpath),
        "%s/smc_out_print.txt", smc_build()) < (int)sizeof(outpath));
    rc = smc_run_dwarfdump(args, outpath);
    TASK_CHECK(rc == 0);
    out = smc_read_file(outpath);
    TASK_CHECK(out != 0);
    /*  Both objects have a macro unit at offset 0 and the
        output distinguishes them. */
    TASK_CHECK(smc_text_has(out,
        "Macro info for a single cu at macro Offset 0x00000000"));
    TASK_CHECK(smc_text_has(out,
        "Macro info for imported macro unit at macro Offset"
        " 0x00000000 in supplementary object"));
    /*  Each object's strp string resolved from its own object. */
    TASK_CHECK(smc_text_has(out, "MAIN_OWNER 1"));
    TASK_CHECK(smc_text_has(out, "SUP_OWNER 9"));
    /*  Region statistics are kept apart per object. */
    TASK_CHECK(smc_text_has(out,
        "Macro unit count DWARF5 .debug_macro: 1"));
    TASK_CHECK(smc_text_has(out,
        "Macro unit count DWARF5 .debug_macro"
        " (supplementary object): 1"));
    free(out);
    return 0;
}

/*  ---------- group: import_execution ---------- */

/*  An ordinary import inside a supplementary macro unit stays
    in the supplementary object; dwarfdump executes the shared
    block every time it is imported. */
static int smc_case_sup_local_import(void)
{
    Dwarf_Debug sup = 0;
    Dwarf_Error error = 0;
    Dwarf_Macro_Context mc = 0;
    Dwarf_Unsigned version = 0, off = 0, count = 0, len = 0;
    Dwarf_Unsigned k = 0, lineno = 0, index = 0, stroff = 0;
    Dwarf_Unsigned target = 0;
    Dwarf_Half op = 0, forms = 0;
    const Dwarf_Small *form = 0;
    const char *text = 0;
    char suppath[4096], args[4096], outpath[4096];
    unsigned imports = 0, defines = 0;
    int rc;

    TASK_CHECK(smc_make_fixtures() == 0);
    TASK_CHECK(snprintf(suppath, sizeof(suppath),
        "%s/supb.elf", smc_build()) < (int)sizeof(suppath));
    TASK_CHECK(dwarf_init_path(suppath, 0, 0,
        DW_GROUPNUMBER_ANY, 0, 0, &sup, &error) == DW_DLV_OK);

    /*  Unit at 0: define "K 1", then an ordinary import whose
        target (15) is another unit in this same object. */
    TASK_CHECK(dwarf_get_macro_context_by_debug(sup, 0,
        &version, &mc, &count, &len, &error) == DW_DLV_OK);
    TASK_CHECK(count == 3);
    for (k = 0; k < count; ++k) {
        TASK_CHECK(dwarf_get_macro_op(mc, k, &off, &op,
            &forms, &form, &error) == DW_DLV_OK);
        if (op == DW_MACRO_import) {
            TASK_CHECK(dwarf_get_macro_import(mc, k, &target,
                &error) == DW_DLV_OK);
            TASK_CHECK(target == 15);
            ++imports;
        }
        if (op == DW_MACRO_define) {
            TASK_CHECK(dwarf_get_macro_defundef(mc, k,
                &lineno, &index, &stroff, &forms, &text,
                &error) == DW_DLV_OK);
            TASK_CHECK(!strcmp(text, "K 1"));
            ++defines;
        }
    }
    TASK_CHECK(imports == 1 && defines == 1);
    dwarf_dealloc_macro_context(mc);
    mc = 0;

    /*  The local import target is read in the same object. */
    TASK_CHECK(dwarf_get_macro_context_by_debug(sup, 15,
        &version, &mc, &count, &len, &error) == DW_DLV_OK);
    TASK_CHECK(count == 2);
    TASK_CHECK(dwarf_get_macro_op(mc, 0, &off, &op,
        &forms, &form, &error) == DW_DLV_OK);
    TASK_CHECK(op == DW_MACRO_define);
    TASK_CHECK(dwarf_get_macro_defundef(mc, 0,
        &lineno, &index, &stroff, &forms, &text,
        &error) == DW_DLV_OK);
    TASK_CHECK(!strcmp(text, "J 2"));
    dwarf_dealloc_macro_context(mc);
    mc = 0;
    TASK_CHECK(dwarf_finish(sup) == DW_DLV_OK);

    /*  dwarfdump prints the shared block and its local
        import target, both owned by the supplementary object. */
    TASK_CHECK(snprintf(args, sizeof(args),
        "--print-macinfo --file-tied=\"%s/supb.elf\""
        " \"%s/mainb.elf\"",
        smc_build(), smc_build()) < (int)sizeof(args));
    TASK_CHECK(snprintf(outpath, sizeof(outpath),
        "%s/smc_out_supb.txt", smc_build()) < (int)sizeof(outpath));
    rc = smc_run_dwarfdump(args, outpath);
    TASK_CHECK(rc == 0);
    {
        char *outp = smc_read_file(outpath);
        TASK_CHECK(outp != 0);
        TASK_CHECK(smc_text_has(outp, "K 1"));
        TASK_CHECK(smc_text_has(outp, "J 2"));
        TASK_CHECK(smc_text_has(outp, "K 2"));
        TASK_CHECK(smc_text_has(outp, "in supplementary object"));
        TASK_CHECK(smc_text_has(outp,
            "Macro unit count DWARF5 .debug_macro"
            " (supplementary object): 2"));
        free(outp);
    }
    return 0;
}

/*  Importing the same shared block twice at the same file
    position runs its define/undef ops through the checks
    every time, in order: define K, undef K, define K again,
    then a different define of K must still be flagged. */
static int smc_case_repeated_import_checked(void)
{
    char args[4096], outpath[4096];
    char *out = 0;
    int rc;

    TASK_CHECK(smc_make_fixtures() == 0);
    TASK_CHECK(snprintf(args, sizeof(args),
        "--print-macinfo --file-tied=\"%s/supb.elf\""
        " \"%s/mainb.elf\"",
        smc_build(), smc_build()) < (int)sizeof(args));
    TASK_CHECK(snprintf(outpath, sizeof(outpath),
        "%s/smc_out_repeat.txt", smc_build()) < (int)sizeof(outpath));
    rc = smc_run_dwarfdump(args, outpath);
    TASK_CHECK(rc == 0);
    out = smc_read_file(outpath);
    TASK_CHECK(out != 0);
    /*  This note only appears if the second import of the
        shared block really re-executed "define K 1" after
        the main block's "undef K". */
    TASK_CHECK(smc_text_has(out,
        "MACRONOTE: Duplicating the macro name \"K\""
        " but with a different spelling"));
    free(out);
    out = 0;

    /*  The same execution happens in check mode too. */
    TASK_CHECK(snprintf(args, sizeof(args),
        "--check-macros --file-tied=\"%s/supb.elf\""
        " \"%s/mainb.elf\"",
        smc_build(), smc_build()) < (int)sizeof(args));
    TASK_CHECK(snprintf(outpath, sizeof(outpath),
        "%s/smc_out_repeat_chk.txt", smc_build()) <
        (int)sizeof(outpath));
    rc = smc_run_dwarfdump(args, outpath);
    TASK_CHECK(rc == 0);
    out = smc_read_file(outpath);
    TASK_CHECK(out != 0);
    TASK_CHECK(smc_text_has(out,
        "MACRONOTE: Duplicating the macro name \"K\""
        " but with a different spelling"));
    TASK_CHECK(smc_text_has(out,
        "Macro unit count DWARF5 .debug_macro"
        " (supplementary object): 2"));
    free(out);
    return 0;
}

/*  ---------- group: recovery ---------- */

/*  With no supplementary object bound the import_sup gets a
    diagnostic naming source and offset, and the remaining
    operators of the unit are still processed. */
static int smc_case_missing_binding(void)
{
    char args[4096], outpath[4096];
    char *out = 0;
    int rc;

    TASK_CHECK(smc_make_fixtures() == 0);
    TASK_CHECK(snprintf(args, sizeof(args),
        "--print-macinfo \"%s/main.elf\"",
        smc_build()) < (int)sizeof(args));
    TASK_CHECK(snprintf(outpath, sizeof(outpath),
        "%s/smc_out_notied.txt", smc_build()) < (int)sizeof(outpath));
    rc = smc_run_dwarfdump(args, outpath);
    TASK_CHECK(rc == 0);
    out = smc_read_file(outpath);
    TASK_CHECK(out != 0);
    TASK_CHECK(smc_text_has(out,
        "DW_MACRO_import_sup"));
    TASK_CHECK(smc_text_has(out,
        "has no supplementary (tied) object bound"));
    /*  Sibling operators after the failed import still run. */
    TASK_CHECK(smc_text_has(out, "MAIN_OWNER 1"));
    TASK_CHECK(smc_text_has(out, "DW_MACRO_end_file"));
    TASK_CHECK(smc_text_has(out, "end-of-macros"));
    free(out);
    return 0;
}

/*  The CU-less entry reports out-of-range offsets and
    unsupported constructs as errors, and dwarfdump recovers
    and keeps going after such a failure. */
static int smc_case_bad_offset_and_unsupported(void)
{
    Dwarf_Debug sup = 0, nomac = 0;
    Dwarf_Error error = 0;
    Dwarf_Macro_Context mc = 0;
    Dwarf_Unsigned version = 0, count = 0, len = 0;
    char suppath[4096], nomacpath[4096], args[4096], outpath[4096];
    char *out = 0;
    int res, rc;

    TASK_CHECK(smc_make_fixtures() == 0);
    TASK_CHECK(snprintf(suppath, sizeof(suppath),
        "%s/supc.elf", smc_build()) < (int)sizeof(suppath));
    TASK_CHECK(snprintf(nomacpath, sizeof(nomacpath),
        "%s/nomac.elf", smc_build()) < (int)sizeof(nomacpath));
    TASK_CHECK(dwarf_init_path(suppath, 0, 0,
        DW_GROUPNUMBER_ANY, 0, 0, &sup, &error) == DW_DLV_OK);

    /*  start_file in a CU-less unit: unsupported. */
    res = dwarf_get_macro_context_by_debug(sup, 0,
        &version, &mc, &count, &len, &error);
    TASK_CHECK(res == DW_DLV_ERROR);
    if (error) {
        dwarf_dealloc_error(sup, error);
        error = 0;
    }
    /*  version 4 unit: unsupported. */
    res = dwarf_get_macro_context_by_debug(sup, 7,
        &version, &mc, &count, &len, &error);
    TASK_CHECK(res == DW_DLV_ERROR);
    if (error) {
        dwarf_dealloc_error(sup, error);
        error = 0;
    }
    /*  define_strx in a CU-less unit: unsupported. */
    res = dwarf_get_macro_context_by_debug(sup, 11,
        &version, &mc, &count, &len, &error);
    TASK_CHECK(res == DW_DLV_ERROR);
    if (error) {
        dwarf_dealloc_error(sup, error);
        error = 0;
    }
    /*  Offset beyond the end of the section. */
    res = dwarf_get_macro_context_by_debug(sup, 0x100,
        &version, &mc, &count, &len, &error);
    TASK_CHECK(res == DW_DLV_ERROR);
    if (error) {
        dwarf_dealloc_error(sup, error);
        error = 0;
    }
    TASK_CHECK(dwarf_finish(sup) == DW_DLV_OK);

    /*  An object with no .debug_macro section: NO_ENTRY. */
    TASK_CHECK(dwarf_init_path(nomacpath, 0, 0,
        DW_GROUPNUMBER_ANY, 0, 0, &nomac, &error) == DW_DLV_OK);
    res = dwarf_get_macro_context_by_debug(nomac, 0,
        &version, &mc, &count, &len, &error);
    TASK_CHECK(res == DW_DLV_NO_ENTRY);
    if (error) {
        dwarf_dealloc_error(nomac, error);
        error = 0;
    }
    TASK_CHECK(dwarf_finish(nomac) == DW_DLV_OK);

    /*  dwarfdump reports the failure with source and offset
        and still processes the sibling define after it. */
    TASK_CHECK(snprintf(args, sizeof(args),
        "--print-macinfo --file-tied=\"%s/supc.elf\""
        " \"%s/mainc.elf\"",
        smc_build(), smc_build()) < (int)sizeof(args));
    TASK_CHECK(snprintf(outpath, sizeof(outpath),
        "%s/smc_out_badunit.txt", smc_build()) <
        (int)sizeof(outpath));
    rc = smc_run_dwarfdump(args, outpath);
    TASK_CHECK(rc == 0);
    out = smc_read_file(outpath);
    TASK_CHECK(out != 0);
    TASK_CHECK(smc_text_has(out,
        "Following DW_MACRO_import_sup to sup_offset 0x00000000"));
    TASK_CHECK(smc_text_has(out, "AFTER 1"));
    free(out);
    return 0;
}

/*  A real import cycle inside the supplementary object is
    diagnosed and that branch stops; the next CU's macros
    are still processed.  The same offset in a different
    object is not a cycle. */
static int smc_case_cycle_and_next_cu(void)
{
    char args[4096], outpath[4096];
    char *out = 0;
    int rc;

    TASK_CHECK(smc_make_fixtures() == 0);
    TASK_CHECK(snprintf(args, sizeof(args),
        "--print-macinfo --file-tied=\"%s/supe.elf\""
        " \"%s/maine.elf\"",
        smc_build(), smc_build()) < (int)sizeof(args));
    TASK_CHECK(snprintf(outpath, sizeof(outpath),
        "%s/smc_out_cycle.txt", smc_build()) < (int)sizeof(outpath));
    rc = smc_run_dwarfdump(args, outpath);
    TASK_CHECK(rc == 0);
    out = smc_read_file(outpath);
    TASK_CHECK(out != 0);
    /*  main at offset 0 importing sup at offset 0 is fine;
        sup at 0 importing sup at 0 is the real cycle. */
    TASK_CHECK(smc_text_has(out, "recursive nest of imports"));
    TASK_CHECK(smc_text_has(out, "supplementary-object"));
    /*  The CU after the one with the cyclic import still
        gets its macros printed. */
    TASK_CHECK(smc_text_has(out, "CU2MAC 1"));
    free(out);
    return 0;
}

const struct task_case task_cases[] = {
    {"owner_context", "cu_less_context_and_strings",
        smc_case_cu_less_context},
    {"owner_context", "print_macinfo_labels_owner",
        smc_case_print_labels_owner},
    {"import_execution", "sup_local_import",
        smc_case_sup_local_import},
    {"import_execution", "repeated_import_checked",
        smc_case_repeated_import_checked},
    {"recovery", "missing_binding",
        smc_case_missing_binding},
    {"recovery", "bad_offset_and_unsupported",
        smc_case_bad_offset_and_unsupported},
    {"recovery", "cycle_stops_branch_next_cu_ok",
        smc_case_cycle_and_next_cu},
};
const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
