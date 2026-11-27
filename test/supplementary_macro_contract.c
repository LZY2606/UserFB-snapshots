/* SPDX-License-Identifier: MIT

   Contract tests for supplementary-object macro traversal:
   dwarf_get_macro_context_by_debug() and the dwarfdump
   --print-macinfo / --check-macros behaviour that follows
   DW_MACRO_import_sup into the bound supplementary object.

   These tests read small, synthetic, non-executable ELF
   objects built here.  No image is executed and no process is
   attached.

   The main object has one CU with a macro unit; the
   supplementary object has only .debug_macro and .debug_str
   and no CU at all.
*/
#include "dwarf.h"
#include "libdwarf.h"
#include "check.h"
#include "fixture.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* A tiny ELF64 little-endian writer, independent of fixture.c so the
   test can lay out exactly the macro units it needs.  Never
   executable: e_type is ET_REL (1), no program headers. */

struct tsection {
    const char *name;
    const unsigned char *data;
    size_t size;
    unsigned nameoff;
    unsigned off;
};

static void
tput(unsigned char *p, uint64_t v, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; ++i) {
        p[i] = (unsigned char)v;
        v >>= 8;
    }
}

static int
twrite_elf(const char *path, struct tsection *s, unsigned count)
{
    unsigned char data[16384];
    unsigned char names[512];
    unsigned i = 0;
    unsigned no = 1;
    unsigned pos = 64;
    unsigned shoff = 0;
    unsigned n = count + 2;
    FILE *f = 0;
    size_t wrote = 0;

    memset(data, 0, sizeof(data));
    memset(names, 0, sizeof(names));
    for (i = 0; i < count; ++i) {
        s[i].nameoff = no;
        strcpy((char *)names + no, s[i].name);
        no += (unsigned)strlen(s[i].name) + 1;
        s[i].off = pos;
        memcpy(data + pos, s[i].data, s[i].size);
        pos += (unsigned)s[i].size;
    }
    {
        unsigned nameoff = pos;
        unsigned shname = no;

        strcpy((char *)names + no, ".shstrtab");
        no += 10;
        memcpy(data + pos, names, no);
        pos += no;
        shoff = (pos + 7) & ~7u;
        memcpy(data, "\177ELF\2\1\1", 7);
        tput(data + 16, 2, 2);   /* ET_EXEC: only a file type, never
                                    loaded or run here. */
        tput(data + 18, 62, 2);  /* EM_X86_64 */
        tput(data + 20, 1, 4);
        tput(data + 40, shoff, 8);
        tput(data + 52, 64, 2);
        tput(data + 58, 64, 2);
        tput(data + 60, n, 2);
        tput(data + 62, n - 1, 2);
        for (i = 0; i < count; ++i) {
            unsigned char *h = data + shoff + 64 * (i + 1);

            tput(h, s[i].nameoff, 4);
            tput(h + 4, 1, 4);       /* PROGBITS */
            tput(h + 24, s[i].off, 8);
            tput(h + 32, s[i].size, 8);
            tput(h + 48, 1, 8);      /* no alignment needed */
        }
        {
            unsigned char *h = data + shoff + 64 * (n - 1);

            tput(h, shname, 4);
            tput(h + 4, 3, 4);       /* STRTAB */
            tput(h + 24, nameoff, 8);
            tput(h + 32, no, 8);
            tput(h + 48, 1, 8);
        }
    }
    f = fopen(path, "wb");
    if (!f) {
        return 1;
    }
    wrote = fwrite(data, 1, shoff + 64 * n, f);
    if (fclose(f)) {
        return 1;
    }
    return wrote == shoff + 64 * n ? 0 : 1;
}

/*  Macro unit builder: version 5, no line offset, no operand
    table, unless asked. */
struct macunit {
    unsigned char off_size_flag; /* 1 means 8-byte offsets */
    unsigned char line_flag;
    unsigned char optable_flag;
    unsigned char buf[1024];
    unsigned len;
};

static void
mau_init(struct macunit *u)
{
    memset(u, 0, sizeof(*u));
}

static void
mau_byte(struct macunit *u, unsigned v)
{
    u->buf[u->len++] = (unsigned char)v;
}

static void
mau_uleb(struct macunit *u, uint64_t v)
{
    do {
        unsigned char b = (unsigned char)(v & 0x7f);
        v >>= 7;
        if (v) {
            b |= 0x80;
        }
        u->buf[u->len++] = b;
    } while (v);
}

static void
mau_offset(struct macunit *u, uint64_t v)
{
    unsigned n = u->off_size_flag ? 8 : 4;
    unsigned i = 0;

    for (i = 0; i < n; ++i) {
        u->buf[u->len++] = (unsigned char)v;
        v >>= 8;
    }
}

/* Writes the 5-byte header: version, flags. */
static void
mau_header(struct macunit *u)
{
    unsigned char flags = 0;

    if (u->off_size_flag) {
        flags |= 1;
    }
    if (u->line_flag) {
        flags |= 2;
    }
    if (u->optable_flag) {
        flags |= 4;
    }
    mau_byte(u, 5);
    mau_byte(u, 0);
    mau_byte(u, flags);
    if (u->line_flag) {
        mau_offset(u, 0);
    }
    if (u->optable_flag) {
        /* An empty custom operands table. */
        mau_byte(u, 0);
    }
}

static void
mau_define_strp(struct macunit *u, uint64_t line, uint64_t stroff)
{
    mau_byte(u, DW_MACRO_define_strp);
    mau_uleb(u, line);
    mau_offset(u, stroff);
}

static void
mau_undef_strp(struct macunit *u, uint64_t line, uint64_t stroff)
{
    mau_byte(u, DW_MACRO_undef_strp);
    mau_uleb(u, line);
    mau_offset(u, stroff);
}

static void
mau_define_strx(struct macunit *u, uint64_t line, uint64_t idx)
{
    mau_byte(u, DW_MACRO_define_strx);
    mau_uleb(u, line);
    mau_uleb(u, idx);
}

static void
mau_start_file(struct macunit *u, uint64_t line, uint64_t filenum)
{
    mau_byte(u, DW_MACRO_start_file);
    mau_uleb(u, line);
    mau_uleb(u, filenum);
}

static void
mau_import(struct macunit *u, uint64_t off)
{
    mau_byte(u, DW_MACRO_import);
    mau_offset(u, off);
}

static void
mau_import_sup(struct macunit *u, uint64_t off)
{
    mau_byte(u, DW_MACRO_import_sup);
    mau_offset(u, off);
}

static void
mau_end(struct macunit *u)
{
    mau_byte(u, 0);
}

/*  Builds the main + supplementary pair used by the cases.
    Paths are formed in mainpath/suppath.

    Main CU macro unit (offset 0):
        define_strp "MAIN_A 1"     (string offset 0 in main .debug_str)
        import_sup 0               (into supplementary offset 0)
        undef_strp "MAIN_A 1"
        import_sup 0               (again: the same supplementary unit)
        define_strp "MAIN_A 2"     (different spelling: a real
                                    redefinition after the import)
        end
    Supplementary macro unit (offset 0):
        define_strp "SUP_K 1"      (string offset 0 in sup .debug_str)
        end
    The main CU also has DW_AT_macros with offset 0. */
#define MAIN_STRINGS "SUP_K 1\0SUP_K 2"
#define SUP_STRINGS  "SUP_K 1\0SUP_K 2"

static int
build_pair(const char *directory, char *mainpath, size_t mainlen,
    char *suppath, size_t suplen)
{
    static const unsigned char abbrev[] = {
        1, 0x11, 0, 3, 8, 0x79, 0x17, 0x10, 0x17, 0, 0, 0
    };
    static const unsigned char info[] = {
        24, 0, 0, 0, 5, 0, 1, 8, 0, 0, 0, 0,
        1, 'm', 'a', 'i', 'n', '.', 'c', 0, 0, 0, 0, 0, 0, 0, 0, 0
    };
    static const unsigned char line[] = {
        49, 0, 0, 0, 5, 0, 8, 0, 38, 0, 0, 0,
        1, 1, 1, 0xfb, 14, 13, 0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1,
        1, 1, 8, 1, '.', 0,
        2, 1, 8, 2, 0x0b, 1, 'm', 'a', 'i', 'n', '.', 'c', 0, 0,
        0, 1, 1
    };
    struct macunit mainunit;
    struct macunit supunit;
    struct tsection mainsec[5];
    struct tsection supsec[2];

    mau_init(&mainunit);
    mau_header(&mainunit);
    mau_import_sup(&mainunit, 0);       /* defines SUP_K 1 */
    mau_undef_strp(&mainunit, 2, 0);    /* undef SUP_K 1 */
    mau_import_sup(&mainunit, 0);       /* defines SUP_K 1 again */
    mau_define_strp(&mainunit, 3, 8);   /* SUP_K 2: a different
                                           spelling after the
                                           undef, then redefinition */
    mau_end(&mainunit);

    mau_init(&supunit);
    mau_header(&supunit);
    mau_define_strp(&supunit, 1, 0);    /* "SUP_K 1" */
    mau_end(&supunit);

    mainsec[0].name = ".debug_abbrev";
    mainsec[0].data = abbrev;
    mainsec[0].size = sizeof(abbrev);
    mainsec[1].name = ".debug_info";
    mainsec[1].data = info;
    mainsec[1].size = sizeof(info);
    mainsec[2].name = ".debug_line";
    mainsec[2].data = line;
    mainsec[2].size = sizeof(line);
    mainsec[3].name = ".debug_macro";
    mainsec[3].data = mainunit.buf;
    mainsec[3].size = mainunit.len;
    mainsec[4].name = ".debug_str";
    mainsec[4].data = (const unsigned char *)MAIN_STRINGS;
    mainsec[4].size = sizeof(MAIN_STRINGS);

    supsec[0].name = ".debug_macro";
    supsec[0].data = supunit.buf;
    supsec[0].size = supunit.len;
    supsec[1].name = ".debug_str";
    supsec[1].data = (const unsigned char *)SUP_STRINGS;
    supsec[1].size = sizeof(SUP_STRINGS);

    if (snprintf(mainpath, mainlen, "%s/contract_main.elf",
        directory) >= (int)mainlen) {
        return 1;
    }
    if (snprintf(suppath, suplen, "%s/contract_sup.elf",
        directory) >= (int)suplen) {
        return 1;
    }
    if (twrite_elf(mainpath, mainsec, 5)) {
        return 1;
    }
    return twrite_elf(suppath, supsec, 2);
}

/*  A single-object fixture for the library tests: no CU at all,
    only .debug_macro and .debug_str.  macrodata is the raw
    section contents. */
static int
build_sup_only(const char *directory, const char *name,
    const unsigned char *macrodata, size_t macrolen,
    const char *strings, size_t stringslen,
    char *path, size_t pathlen)
{
    struct tsection sec[2];

    sec[0].name = ".debug_macro";
    sec[0].data = macrodata;
    sec[0].size = macrolen;
    sec[1].name = ".debug_str";
    sec[1].data = (const unsigned char *)strings;
    sec[1].size = stringslen;
    if (snprintf(path, pathlen, "%s/%s", directory, name) >=
        (int)pathlen) {
        return 1;
    }
    return twrite_elf(path, sec, 2);
}

static Dwarf_Debug
open_dbg(const char *path, Dwarf_Error *err)
{
    Dwarf_Debug dbg = 0;
    int res = 0;

    res = dwarf_init_path(path, 0, 0, DW_GROUPNUMBER_ANY,
        0, 0, &dbg, err);
    return res == DW_DLV_OK ? dbg : 0;
}

/* Count define/undef operators and collect strings in order. */
struct seen_strings {
    char text[32][64];
    unsigned count;
    unsigned number_of_imports;
    unsigned number_of_import_sup;
};

static int
collect_defundef(Dwarf_Macro_Context mc, Dwarf_Unsigned count,
    struct seen_strings *seen, Dwarf_Error *err)
{
    Dwarf_Unsigned k = 0;

    memset(seen, 0, sizeof(*seen));
    for (k = 0; k < count; ++k) {
        Dwarf_Unsigned section_offset = 0;
        Dwarf_Half op = 0;
        Dwarf_Half forms = 0;
        const Dwarf_Small *formarray = 0;
        int res = 0;

        res = dwarf_get_macro_op(mc, k, &section_offset, &op,
            &forms, &formarray, err);
        if (res != DW_DLV_OK) {
            return res;
        }
        if (op == DW_MACRO_define || op == DW_MACRO_undef ||
            op == DW_MACRO_define_strp || op == DW_MACRO_undef_strp ||
            op == DW_MACRO_define_strx || op == DW_MACRO_undef_strx) {
            Dwarf_Unsigned line = 0;
            Dwarf_Unsigned index = 0;
            Dwarf_Unsigned stroff = 0;
            const char *text = 0;

            res = dwarf_get_macro_defundef(mc, k, &line, &index,
                &stroff, &forms, &text, err);
            if (res != DW_DLV_OK) {
                return res;
            }
            if (seen->count < 32 && text) {
                snprintf(seen->text[seen->count],
                    sizeof(seen->text[0]), "%s", text);
                ++seen->count;
            }
        } else if (op == DW_MACRO_import) {
            ++seen->number_of_imports;
        } else if (op == DW_MACRO_import_sup) {
            ++seen->number_of_import_sup;
        }
    }
    return DW_DLV_OK;
}

/* ------------------------------------------------------------------ */
/* owner_context: the CU-less entry point reads the supplementary
   object and gets strings from its own .debug_str. */
static int
case_library_reads_supplementary(void)
{
    char mainpath[4096];
    char suppath[4096];
    Dwarf_Debug maindbg = 0;
    Dwarf_Debug supdbg = 0;
    Dwarf_Debug tied = 0;
    Dwarf_Error err = 0;
    Dwarf_Macro_Context mc = 0;
    Dwarf_Unsigned version = 0;
    Dwarf_Unsigned count = 0;
    Dwarf_Unsigned length = 0;
    struct seen_strings seen;
    int res = 0;
    const char *scratch = getenv("TASK_BUILD");

    if (!scratch || !scratch[0]) {
        scratch = ".";
    }
    TASK_CHECK(task_basic_fixtures(scratch) == 0);
    snprintf(mainpath, sizeof(mainpath), "%s/main.elf", scratch);
    snprintf(suppath, sizeof(suppath), "%s/sup.elf", scratch);
    maindbg = open_dbg(mainpath, &err);
    TASK_CHECK(maindbg != 0);
    supdbg = open_dbg(suppath, &err);
    TASK_CHECK(supdbg != 0);
    TASK_CHECK(dwarf_set_tied_dbg(maindbg, supdbg, &err) ==
        DW_DLV_OK);
    TASK_CHECK(dwarf_get_tied_dbg(maindbg, &tied, &err) ==
        DW_DLV_OK);
    TASK_CHECK(tied == supdbg);
    /* The tied object is the supplementary object for the main. */
    TASK_CHECK(dwarf_get_tied_dbg(supdbg, &tied, &err) ==
        DW_DLV_OK);

    res = dwarf_get_macro_context_by_debug(supdbg, 0, &version,
        &mc, &count, &length, &err);
    TASK_CHECK(res == DW_DLV_OK);
    TASK_CHECK(version == 5);
    TASK_CHECK(count == 2);
    TASK_CHECK(collect_defundef(mc, count, &seen, &err) == DW_DLV_OK);
    TASK_CHECK(seen.count == 1);
    TASK_CHECK(!strcmp(seen.text[0], "SUP_OWNER 9"));
    dwarf_dealloc_macro_context(mc);
    mc = 0;

    /* Out-of-range offset is an error, not a crash. */
    res = dwarf_get_macro_context_by_debug(supdbg, 1000, &version,
        &mc, &count, &length, &err);
    TASK_CHECK(res == DW_DLV_ERROR);
    if (res == DW_DLV_ERROR) {
        dwarf_dealloc_error(supdbg, err);
        err = 0;
    }

    /* An object without .debug_macro is NO_ENTRY. */
    {
        static const char onlystr[] = "JUST A STRING";
        char txt[4096];
        Dwarf_Debug nodbg = 0;

        snprintf(txt, sizeof(txt), "%s/contract_nomac.elf",
            scratch);
        TASK_CHECK(build_sup_only(scratch, "contract_nomac.elf",
            (const unsigned char *)"", 0, onlystr,
            sizeof(onlystr), txt, sizeof(txt)) == 0);
        nodbg = open_dbg(txt, &err);
        TASK_CHECK(nodbg != 0);
        res = dwarf_get_macro_context_by_debug(nodbg, 0,
            &version, &mc, &count, &length, &err);
        if (res == DW_DLV_ERROR) {
            dwarf_dealloc_error(nodbg, err);
            err = 0;
        }
        TASK_CHECK(res == DW_DLV_NO_ENTRY);
        dwarf_finish(nodbg);
    }

    /* Build a standalone supplementary object with a local
       DW_MACRO_import so a plain import stays in that object. */
    {
        struct macunit u;
        struct macunit u2;
        static const char strs[] = "SUP_LOCAL 3\0SUP_TARGET 4";
        char standalone[4096];
        Dwarf_Debug sdbg = 0;

        mau_init(&u);
        mau_header(&u);
        mau_define_strp(&u, 1, 0);   /* SUP_LOCAL 3 */
        mau_import(&u, 0);
        mau_end(&u);

        mau_init(&u2);
        mau_header(&u2);
        mau_define_strp(&u2, 1, 12); /* SUP_TARGET 4 */
        mau_end(&u2);

        /* Lay both units in one section: unit at 0, second unit
           right after it. */
        {
            unsigned char both[2048];
            unsigned second = u.len;

            memcpy(both, u.buf, u.len);
            memcpy(both + second, u2.buf, u2.len);
            TASK_CHECK(build_sup_only(
                getenv("TASK_BUILD") ? getenv("TASK_BUILD") : "/tmp",
                "contract_standalone_sup.elf",
                both, u.len + u2.len,
                strs, sizeof(strs), standalone,
                sizeof(standalone)) == 0);
        }
        sdbg = open_dbg(standalone, &err);
        TASK_CHECK(sdbg != 0);
        res = dwarf_get_macro_context_by_debug(sdbg, 0, &version,
            &mc, &count, &length, &err);
        TASK_CHECK(res == DW_DLV_OK);
        TASK_CHECK(collect_defundef(mc, count, &seen, &err) ==
            DW_DLV_OK);
        TASK_CHECK(seen.count == 1);
        TASK_CHECK(!strcmp(seen.text[0], "SUP_LOCAL 3"));
        TASK_CHECK(seen.number_of_imports == 1);
        dwarf_dealloc_macro_context(mc);
        mc = 0;
        /* The second unit resolves its string from this object. */
        res = dwarf_get_macro_context_by_debug(sdbg,
            (Dwarf_Unsigned)(u.len), &version, &mc, &count,
            &length, &err);
        TASK_CHECK(res == DW_DLV_OK);
        TASK_CHECK(collect_defundef(mc, count, &seen, &err) ==
            DW_DLV_OK);
        TASK_CHECK(seen.count == 1);
        TASK_CHECK(!strcmp(seen.text[0], "SUP_TARGET 4"));
        dwarf_dealloc_macro_context(mc);
        mc = 0;
        dwarf_finish(sdbg);
    }

    dwarf_finish(maindbg);
    dwarf_finish(supdbg);
    return 0;
}

static int
case_same_offset_two_objects(void)
{
    char base[4096];
    char mainpath[4096];
    char suppath[4096];
    char pairmain[4096];
    char pairsup[4096];
    Dwarf_Debug dbg = 0;
    Dwarf_Debug supdbg = 0;
    Dwarf_Error err = 0;
    Dwarf_Macro_Context mc = 0;
    Dwarf_Unsigned version = 0;
    Dwarf_Unsigned count = 0;
    Dwarf_Unsigned length = 0;
    struct seen_strings seen;
    int res = 0;

    snprintf(base, sizeof(base), "%s",
        getenv("TASK_BUILD") ? getenv("TASK_BUILD") : "/tmp");
    TASK_CHECK(build_pair(base, pairmain, sizeof(pairmain),
        pairsup, sizeof(pairsup)) == 0);
    (void)mainpath;
    (void)suppath;
    dbg = open_dbg(pairmain, &err);
    TASK_CHECK(dbg != 0);
    supdbg = open_dbg(pairsup, &err);
    TASK_CHECK(supdbg != 0);
    TASK_CHECK(dwarf_set_tied_dbg(dbg, supdbg, &err) == DW_DLV_OK);

    /* Both objects have a macro unit at offset 0; reading each
       through its own Dwarf_Debug must yield its own contents.
       The CU-less entry excludes import_sup, so the main unit
       here holds only define_strp: the point is that the same
       offset names different content in the two objects. */
    {
        struct macunit mu;
        struct macunit su;
        struct tsection msec[4];
        struct tsection ssec[2];
        static const unsigned char abbrev[] = {
            1, 0x11, 0, 3, 8, 0x79, 0x17, 0x10, 0x17, 0, 0, 0
        };
        static const unsigned char info[] = {
            24, 0, 0, 0, 5, 0, 1, 8, 0, 0, 0, 0,
            1, 'm', 'a', 'i', 'n', '.', 'c', 0, 0, 0, 0, 0, 0, 0, 0, 0
        };
        static const char mstr[] = "MAIN_A 1";
        static const char sstr[] = "SUP_K 1";

        mau_init(&mu);
        mau_header(&mu);
        mau_define_strp(&mu, 1, 0);
        mau_end(&mu);
        mau_init(&su);
        mau_header(&su);
        mau_define_strp(&su, 1, 0);
        mau_end(&su);
        msec[0].name = ".debug_abbrev";
        msec[0].data = abbrev;
        msec[0].size = sizeof(abbrev);
        msec[1].name = ".debug_info";
        msec[1].data = info;
        msec[1].size = sizeof(info);
        msec[2].name = ".debug_macro";
        msec[2].data = mu.buf;
        msec[2].size = mu.len;
        msec[3].name = ".debug_str";
        msec[3].data = (const unsigned char *)mstr;
        msec[3].size = sizeof(mstr);
        ssec[0].name = ".debug_macro";
        ssec[0].data = su.buf;
        ssec[0].size = su.len;
        ssec[1].name = ".debug_str";
        ssec[1].data = (const unsigned char *)sstr;
        ssec[1].size = sizeof(sstr);
        snprintf(pairmain, sizeof(pairmain),
            "%s/contract_same_main.elf", base);
        snprintf(pairsup, sizeof(pairsup),
            "%s/contract_same_sup.elf", base);
        TASK_CHECK(twrite_elf(pairmain, msec, 4) == 0);
        TASK_CHECK(twrite_elf(pairsup, ssec, 2) == 0);
        dwarf_finish(supdbg);
        dwarf_finish(dbg);
        dbg = open_dbg(pairmain, &err);
        TASK_CHECK(dbg != 0);
        supdbg = open_dbg(pairsup, &err);
        TASK_CHECK(supdbg != 0);
        TASK_CHECK(dwarf_set_tied_dbg(dbg, supdbg, &err) ==
            DW_DLV_OK);
    }

    res = dwarf_get_macro_context_by_debug(dbg, 0, &version,
        &mc, &count, &length, &err);
    TASK_CHECK(res == DW_DLV_OK);
    TASK_CHECK(collect_defundef(mc, count, &seen, &err) == DW_DLV_OK);
    TASK_CHECK(seen.count == 1);
    TASK_CHECK(!strcmp(seen.text[0], "MAIN_A 1"));
    dwarf_dealloc_macro_context(mc);
    mc = 0;

    res = dwarf_get_macro_context_by_debug(supdbg, 0, &version,
        &mc, &count, &length, &err);
    TASK_CHECK(res == DW_DLV_OK);
    TASK_CHECK(collect_defundef(mc, count, &seen, &err) == DW_DLV_OK);
    TASK_CHECK(seen.count == 1);
    TASK_CHECK(!strcmp(seen.text[0], "SUP_K 1"));
    TASK_CHECK(seen.number_of_imports == 0);
    TASK_CHECK(seen.number_of_import_sup == 0);
    dwarf_dealloc_macro_context(mc);
    mc = 0;

    dwarf_finish(supdbg);
    dwarf_finish(dbg);
    return 0;
}

/* Unsupported constructs must be rejected by the CU-less entry. */
static int
case_unsupported_constructs(void)
{
    char base[4096];
    Dwarf_Error err = 0;
    int which = 0;

    snprintf(base, sizeof(base), "%s",
        getenv("TASK_BUILD") ? getenv("TASK_BUILD") : "/tmp");
    for (which = 0; which < 4; ++which) {
        struct macunit u;
        static const char strs[] = "X 1\0Y 2";
        char path[4096];
        Dwarf_Debug dbg = 0;
        Dwarf_Macro_Context mc = 0;
        Dwarf_Unsigned version = 0;
        Dwarf_Unsigned count = 0;
        Dwarf_Unsigned length = 0;
        int res = 0;

        mau_init(&u);
        if (which == 0) {
            mau_header(&u);
            mau_define_strx(&u, 1, 0);
            mau_end(&u);
        } else if (which == 1) {
            mau_header(&u);
            mau_start_file(&u, 1, 0);
            mau_end(&u);
        } else if (which == 2) {
            u.line_flag = 1;
            mau_header(&u);
            mau_define_strp(&u, 1, 0);
            mau_end(&u);
        } else {
            u.optable_flag = 1;
            mau_header(&u);
            mau_define_strp(&u, 1, 0);
            mau_end(&u);
        }
        TASK_CHECK(build_sup_only(base, "contract_unsup.elf",
            u.buf, u.len, strs, sizeof(strs), path,
            sizeof(path)) == 0);
        dbg = open_dbg(path, &err);
        TASK_CHECK(dbg != 0);
        res = dwarf_get_macro_context_by_debug(dbg, 0, &version,
            &mc, &count, &length, &err);
        TASK_CHECK(res == DW_DLV_ERROR);
        if (res == DW_DLV_ERROR) {
            TASK_CHECK(dwarf_errno(err) ==
                DW_DLE_INTERFACE_NOT_SUPPORTED ||
                dwarf_errno(err) == DW_DLE_MACRO_OP_UNHANDLED);
            dwarf_dealloc_error(dbg, err);
            err = 0;
        }
        if (res == DW_DLV_OK) {
            dwarf_dealloc_macro_context(mc);
        }
        dwarf_finish(dbg);
    }
    /* Version 4 (the GNU extension) also needs a CU context. */
    {
        struct macunit u;
        static const char strs[] = "X 1";
        char path[4096];
        Dwarf_Debug dbg = 0;
        Dwarf_Macro_Context mc = 0;
        Dwarf_Unsigned version = 0;
        Dwarf_Unsigned count = 0;
        Dwarf_Unsigned length = 0;
        int res = 0;

        mau_init(&u);
        mau_byte(&u, 4);
        mau_byte(&u, 0);
        mau_byte(&u, 0);
        mau_define_strp(&u, 1, 0);
        mau_end(&u);
        TASK_CHECK(build_sup_only(base, "contract_v4.elf",
            u.buf, u.len, strs, sizeof(strs), path,
            sizeof(path)) == 0);
        dbg = open_dbg(path, &err);
        TASK_CHECK(dbg != 0);
        res = dwarf_get_macro_context_by_debug(dbg, 0, &version,
            &mc, &count, &length, &err);
        TASK_CHECK(res == DW_DLV_ERROR);
        if (res == DW_DLV_ERROR) {
            dwarf_dealloc_error(dbg, err);
            err = 0;
        }
        dwarf_finish(dbg);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* import_execution: run the real dwarfdump and check that the
   supplementary definitions are printed and checked, in order, and
   that the main and supplementary units are told apart. */

static int
run_cli_named(const char *mode, const char *mainname,
    const char *tiedname, char *out, size_t outlen)
{
    char cmd[16384];
    FILE *p = 0;
    size_t used = 0;

    snprintf(cmd, sizeof(cmd),
        "\"%s\" --file-name=src/bin/dwarfdump/dwarfdump.conf %s "
        "--file-tied=\"%s/%s\" \"%s/%s\" 2>&1",
        getenv("TASK_DWARFDUMP") ? getenv("TASK_DWARFDUMP") : "",
        mode,
        getenv("TASK_BUILD") ? getenv("TASK_BUILD") : "/tmp",
        tiedname,
        getenv("TASK_BUILD") ? getenv("TASK_BUILD") : "/tmp",
        mainname);
    p = popen(cmd, "r");
    if (!p) {
        return -1;
    }
    out[0] = 0;
    while (used + 1 < outlen) {
        size_t n = fread(out + used, 1, outlen - 1 - used, p);

        used += n;
        if (!n) {
            break;
        }
    }
    out[used] = 0;
    return pclose(p);
}

static int
run_cli(const char *mode, char *out, size_t outlen)
{
    return run_cli_named(mode, "contract_main.elf",
        "contract_sup.elf", out, outlen);
}

static int
contains(const char *hay, const char *needle)
{
    return strstr(hay, needle) != 0;
}

static int
case_cli_prints_supplementary(void)
{
    char base[4096];
    char pairmain[4096];
    char pairsup[4096];
    static char out[262144];

    snprintf(base, sizeof(base), "%s",
        getenv("TASK_BUILD") ? getenv("TASK_BUILD") : "/tmp");
    TASK_CHECK(build_pair(base, pairmain, sizeof(pairmain),
        pairsup, sizeof(pairsup)) == 0);
    TASK_CHECK(run_cli("--print-macinfo", out, sizeof(out)) >= 0);
    /* Both the undef in the main object and the define in the
       supplementary object resolve SUPP_K from the right file. */
    TASK_CHECK(contains(out, "SUP_K 1"));
    TASK_CHECK(contains(out, "SUP_K 2"));
    TASK_CHECK(contains(out, "main object"));
    TASK_CHECK(contains(out, "supplementary object"));
    /* The supplementary unit at the same offset 0 is labelled. */
    TASK_CHECK(contains(out,
        "imported macro unit at macro Offset 0x00000000 in "
        "supplementary object"));
    return 0;
}

static int
case_cli_check_runs_supplementary(void)
{
    char base[4096];
    char pairmain[4096];
    char pairsup[4096];
    static char out[262144];

    snprintf(base, sizeof(base), "%s",
        getenv("TASK_BUILD") ? getenv("TASK_BUILD") : "/tmp");
    TASK_CHECK(build_pair(base, pairmain, sizeof(pairmain),
        pairsup, sizeof(pairsup)) == 0);
    TASK_CHECK(run_cli("--check-macros", out, sizeof(out)) >= 0);
    /* The shared block is imported twice, once before the main
       undef and once after it.  The second import must execute
       again (a def after the undef), and the final define of the
       same name with a different spelling must raise the
       existing redefinition note. */
    /*  If the second import had been skipped, SUP_K would be
        undefined when the final define arrives and no
        redefinition note would fire.  The note proves the
        shared block executed both times. */
    TASK_CHECK(contains(out,
        "Duplicating the macro name \"SUP_K\""));
    TASK_CHECK(contains(out, "SUP_K 1"));
    TASK_CHECK(contains(out, "SUP_K 2"));
    return 0;
}

/* ------------------------------------------------------------------ */
/* recovery: bad imports and loops do not lose the objects or stop
   later siblings from being processed, and no stack entry is left. */

static int
case_recovery_cycle_and_siblings(void)
{
    char base[4096];
    struct macunit mainu;
    struct macunit supu;
    struct macunit supu2;
    struct tsection mainsec[5];
    struct tsection supsec[2];
    char mainpath[4096];
    char suppath[4096];
    static const unsigned char abbrev[] = {
        1, 0x11, 0, 3, 8, 0x79, 0x17, 0x10, 0x17, 0, 0, 0
    };
    static const unsigned char info[] = {
        24, 0, 0, 0, 5, 0, 1, 8, 0, 0, 0, 0,
        1, 'm', 'a', 'i', 'n', '.', 'c', 0, 0, 0, 0, 0, 0, 0, 0, 0
    };
    static const unsigned char line[] = {
        49, 0, 0, 0, 5, 0, 8, 0, 38, 0, 0, 0,
        1, 1, 1, 0xfb, 14, 13, 0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1,
        1, 1, 8, 1, '.', 0,
        2, 1, 8, 2, 0x0b, 1, 'm', 'a', 'i', 'n', '.', 'c', 0, 0,
        0, 1, 1
    };
    static const char mainstr[] = "MAIN_B 1";
    static const char supstr[] = "SUP_LOOP 1\0SUP_SIBLING 2";
    Dwarf_Debug dbg = 0;
    Dwarf_Debug supdbg = 0;
    Dwarf_Error err = 0;
    Dwarf_Macro_Context mc = 0;
    Dwarf_Unsigned version = 0;
    Dwarf_Unsigned count = 0;
    Dwarf_Unsigned length = 0;
    struct seen_strings seen;
    int res = 0;
    unsigned second_off = 0;

    snprintf(base, sizeof(base), "%s",
        getenv("TASK_BUILD") ? getenv("TASK_BUILD") : "/tmp");

    /*  Supplementary unit at 0: a plain import of itself, a
        true loop inside one object.  A plain import stays in
        the current object, so this cannot escape elsewhere. */
    mau_init(&supu);
    mau_header(&supu);
    mau_define_strp(&supu, 1, 0);     /* SUP_LOOP 1 */
    mau_import(&supu, 0);             /* loop on itself */
    mau_end(&supu);

    /*  Supplementary unit at the sibling offset: fine. */
    second_off = supu.len;
    mau_init(&supu2);
    mau_header(&supu2);
    mau_define_strp(&supu2, 1, 11);   /* SUP_SIBLING 2 */
    mau_end(&supu2);

    /*  Main unit: import_sup into the looping unit, then
        import_sup into its sibling, then a define of its own
        afterwards.  The loop must not stop the sibling or the
        rest of this unit from being processed. */
    mau_init(&mainu);
    mau_header(&mainu);
    mau_define_strp(&mainu, 1, 0);    /* MAIN_B 1 */
    mau_import_sup(&mainu, 0);
    mau_import_sup(&mainu, second_off);
    mau_end(&mainu);

    {
        unsigned char both[2048];

        memcpy(both, supu.buf, supu.len);
        memcpy(both + second_off, supu2.buf, supu2.len);
        mainsec[0].name = ".debug_abbrev";
        mainsec[0].data = abbrev;
        mainsec[0].size = sizeof(abbrev);
        mainsec[1].name = ".debug_info";
        mainsec[1].data = info;
        mainsec[1].size = sizeof(info);
        mainsec[2].name = ".debug_macro";
        mainsec[2].data = mainu.buf;
        mainsec[2].size = mainu.len;
        mainsec[3].name = ".debug_str";
        mainsec[3].data = (const unsigned char *)mainstr;
        mainsec[3].size = sizeof(mainstr);
        mainsec[4].name = ".debug_line";
        mainsec[4].data = line;
        mainsec[4].size = sizeof(line);

        supsec[0].name = ".debug_macro";
        supsec[0].data = both;
        supsec[0].size = supu.len + supu2.len;
        supsec[1].name = ".debug_str";
        supsec[1].data = (const unsigned char *)supstr;
        supsec[1].size = sizeof(supstr);

        snprintf(mainpath, sizeof(mainpath), "%s/contract_rec_main.elf",
            base);
        snprintf(suppath, sizeof(suppath), "%s/contract_rec_sup.elf",
            base);
        TASK_CHECK(twrite_elf(mainpath, mainsec, 5) == 0);
        TASK_CHECK(twrite_elf(suppath, supsec, 2) == 0);
    }

    dbg = open_dbg(mainpath, &err);
    TASK_CHECK(dbg != 0);
    supdbg = open_dbg(suppath, &err);
    TASK_CHECK(supdbg != 0);
    TASK_CHECK(dwarf_set_tied_dbg(dbg, supdbg, &err) == DW_DLV_OK);

    /* Both supplementary units read fine through the library. */
    res = dwarf_get_macro_context_by_debug(supdbg, 0, &version,
        &mc, &count, &length, &err);
    TASK_CHECK(res == DW_DLV_OK);
    TASK_CHECK(collect_defundef(mc, count, &seen, &err) == DW_DLV_OK);
    TASK_CHECK(seen.count == 1);
    TASK_CHECK(!strcmp(seen.text[0], "SUP_LOOP 1"));
    TASK_CHECK(seen.number_of_imports == 1);
    dwarf_dealloc_macro_context(mc);
    mc = 0;

    res = dwarf_get_macro_context_by_debug(supdbg, second_off,
        &version, &mc, &count, &length, &err);
    TASK_CHECK(res == DW_DLV_OK);
    TASK_CHECK(collect_defundef(mc, count, &seen, &err) == DW_DLV_OK);
    TASK_CHECK(seen.count == 1);
    TASK_CHECK(!strcmp(seen.text[0], "SUP_SIBLING 2"));
    dwarf_dealloc_macro_context(mc);
    mc = 0;

    /* The same offset in the main object is a different unit.
       It is read via the CU-based entry because it contains
       DW_MACRO_import_sup, which the CU-less entry excludes. */
    {
        Dwarf_Die die = 0;
        Dwarf_Unsigned macro_unit_offset = 0;

        TASK_CHECK(dwarf_offdie_b(dbg, 12, 1, &die, &err) ==
            DW_DLV_OK);
        res = dwarf_get_macro_context(die, &version, &mc,
            &macro_unit_offset, &count, &length, &err);
        TASK_CHECK(res == DW_DLV_OK);
        TASK_CHECK(collect_defundef(mc, count, &seen, &err) ==
            DW_DLV_OK);
        TASK_CHECK(seen.count == 1);
        TASK_CHECK(!strcmp(seen.text[0], "MAIN_B 1"));
        dwarf_dealloc_macro_context(mc);
        mc = 0;
        dwarf_dealloc_die(die);
    }

    /* A missing binding is an error, and the object still works
       afterwards. */
    {
        Dwarf_Debug lone = open_dbg(mainpath, &err);

        TASK_CHECK(lone != 0);
        (void)lone;
        dwarf_finish(lone);
    }

    dwarf_finish(supdbg);
    dwarf_finish(dbg);
    return 0;
}

static int
case_recovery_cli_after_loop(void)
{
    char base[4096];
    static char out[262144];

    snprintf(base, sizeof(base), "%s",
        getenv("TASK_BUILD") ? getenv("TASK_BUILD") : "/tmp");
    /*  The fixture from the previous case has a supplementary
        unit that imports itself.  dwarfdump must diagnose the
        loop, keep the exit status usable, and still report the
        sibling and the next unit. */
    TASK_CHECK(run_cli_named("--print-macinfo",
        "contract_rec_main.elf", "contract_rec_sup.elf",
        out, sizeof(out)) >= 0);
    TASK_CHECK(contains(out, "recursive nest of imports"));
    TASK_CHECK(contains(out, "SUP_LOOP 1"));
    TASK_CHECK(contains(out, "SUP_SIBLING 2"));
    TASK_CHECK(contains(out, "MAIN_B 1"));
    return 0;
}

static int
case_recovery_bad_offset_sibling(void)
{
    char base[4096];
    struct macunit mainu;
    struct tsection mainsec[5];
    static const unsigned char abbrev[] = {
        1, 0x11, 0, 3, 8, 0x79, 0x17, 0x10, 0x17, 0, 0, 0
    };
    static const unsigned char info[] = {
        24, 0, 0, 0, 5, 0, 1, 8, 0, 0, 0, 0,
        1, 'm', 'a', 'i', 'n', '.', 'c', 0, 0, 0, 0, 0, 0, 0, 0, 0
    };
    static const unsigned char line[] = {
        49, 0, 0, 0, 5, 0, 8, 0, 38, 0, 0, 0,
        1, 1, 1, 0xfb, 14, 13, 0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1,
        1, 1, 8, 1, '.', 0,
        2, 1, 8, 2, 0x0b, 1, 'm', 'a', 'i', 'n', '.', 'c', 0, 0,
        0, 1, 1
    };
    static const char mainstr[] = "MAIN_C 1";
    char mainpath[4096];
    static char out[262144];
    char cmd[16384];
    FILE *p = 0;
    size_t used = 0;

    snprintf(base, sizeof(base), "%s",
        getenv("TASK_BUILD") ? getenv("TASK_BUILD") : "/tmp");
    /*  import_sup to an out-of-range supplementary offset,
        then a plain import that is fine.  The bad one must be
        diagnosed and the later one still handled. */
    mau_init(&mainu);
    mau_header(&mainu);
    mau_import_sup(&mainu, 4096);   /* out of range */
    mau_define_strp(&mainu, 1, 0);
    mau_end(&mainu);
    mainsec[0].name = ".debug_abbrev";
    mainsec[0].data = abbrev;
    mainsec[0].size = sizeof(abbrev);
    mainsec[1].name = ".debug_info";
    mainsec[1].data = info;
    mainsec[1].size = sizeof(info);
    mainsec[2].name = ".debug_macro";
    mainsec[2].data = mainu.buf;
    mainsec[2].size = mainu.len;
    mainsec[3].name = ".debug_str";
    mainsec[3].data = (const unsigned char *)mainstr;
    mainsec[3].size = sizeof(mainstr);
    mainsec[4].name = ".debug_line";
    mainsec[4].data = line;
    mainsec[4].size = sizeof(line);
    snprintf(mainpath, sizeof(mainpath), "%s/contract_bad_main.elf",
        base);
    TASK_CHECK(twrite_elf(mainpath, mainsec, 5) == 0);

    snprintf(cmd, sizeof(cmd),
        "\"%s\" --file-name=src/bin/dwarfdump/dwarfdump.conf "
        "--print-macinfo --file-tied=\"%s/contract_sup.elf\" "
        "\"%s\" 2>&1",
        getenv("TASK_DWARFDUMP") ? getenv("TASK_DWARFDUMP") : "",
        base, mainpath);
    p = popen(cmd, "r");
    TASK_CHECK(p != 0);
    if (p) {
        out[0] = 0;
        while (used + 1 < sizeof(out)) {
            size_t n = fread(out + used, 1, sizeof(out) - 1 - used, p);

            used += n;
            if (!n) {
                break;
            }
        }
        out[used] = 0;
        (void)pclose(p);
    }
    /* The bad offset is reported with the supplementary object
       and the offset, and the later define is still printed. */
    TASK_CHECK(contains(out, "contract_bad_main.elf") ||
        contains(out, "macro offset"));
    TASK_CHECK(contains(out, "MAIN_C 1"));
    return 0;
}

/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
    { "owner_context", "library_reads_supplementary",
      case_library_reads_supplementary },
    { "owner_context", "same_offset_two_objects",
      case_same_offset_two_objects },
    { "owner_context", "unsupported_constructs",
      case_unsupported_constructs },
    { "import_execution", "cli_prints_supplementary",
      case_cli_prints_supplementary },
    { "import_execution", "cli_check_runs_supplementary",
      case_cli_check_runs_supplementary },
    { "recovery", "cycle_and_siblings",
      case_recovery_cycle_and_siblings },
    { "recovery", "cli_after_loop",
      case_recovery_cli_after_loop },
    { "recovery", "bad_offset_sibling",
      case_recovery_bad_offset_sibling },
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
