/*
    Regression cases for safe parsing of PE export metadata.

    Each case builds its own PE32/PE32+ bytes with the independent fixture
    from task_support/fixture.h (no PE writer, no image execution, no DLL
    loading) and registers itself through task_cases for task_support/driver.c.
*/
#include "task_support/check.h"
#include "task_support/fixture.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

/* File offsets inside the fixture's IMAGE_EXPORT_DIRECTORY (file ofs 0x200). */
#define EXP_NAME 0x20c
#define EXP_BASE 0x210
#define EXP_NUM_FUNCTIONS 0x214
#define EXP_NUM_NAMES 0x218
#define EXP_ADDR_FUNCTIONS 0x21c
#define EXP_ADDR_NAMES 0x220
#define EXP_ADDR_ORDINALS 0x224
/* Directory entry 0 (export) inside the optional header. */
#define DIR0_PE32 0xf8 /* opt(0x98) + prefix(96) */
#define DIR0_PE64 0x108 /* opt(0x98) + prefix(112) */

static int check_empty_error(pe_exports_t *e) {
    return e != NULL && e->err != LIBPE_E_OK && e->name == NULL
        && e->functions == NULL && e->functions_count == 0;
}

/* ---------------------------------------------------------------- tables */

static int case_normal_pe32_pe64(void) {
    int bits;
    for (bits = 32; bits <= 64; bits += 32) {
        unsigned char b[FIXTURE_SIZE]; pe_ctx_t c;
        fixture(b, bits);
        TASK_CHECK(fixture_open(&c, b, sizeof(b)) == LIBPE_E_OK);
        pe_exports_t *e = pe_exports(&c);
        TASK_CHECK(e != NULL && e->err == LIBPE_E_OK);
        TASK_CHECK(e->functions_count == 2 && e->functions != NULL);
        TASK_CHECK(e->name != NULL && strcmp(e->name, "fixture.dll") == 0);
        TASK_CHECK(e->functions[0].ordinal == 7);
        TASK_CHECK(e->functions[0].hint == 0);
        TASK_CHECK(e->functions[0].address == 0x1400);
        TASK_CHECK(strcmp(e->functions[0].name, "Alpha") == 0);
        TASK_CHECK(e->functions[0].fwd_name == NULL);
        TASK_CHECK(e->functions[1].ordinal == 8);
        TASK_CHECK(e->functions[1].hint == 1);
        TASK_CHECK(e->functions[1].address == 0x10a0);
        TASK_CHECK(strcmp(e->functions[1].name, "Beta") == 0);
        TASK_CHECK(e->functions[1].fwd_name != NULL
            && strcmp(e->functions[1].fwd_name, "other.Func") == 0);
        fixture_close(&c);
    }
    return 0;
}

static int case_no_export_directory(void) {
    unsigned char b[FIXTURE_SIZE]; pe_ctx_t c;
    /* Export directory RVA is zero: successful empty result. */
    fixture(b, 32);
    put32(b, DIR0_PE32, 0);
    TASK_CHECK(fixture_open(&c, b, sizeof(b)) == LIBPE_E_OK);
    pe_exports_t *e = pe_exports(&c);
    TASK_CHECK(e != NULL && e->err == LIBPE_E_OK);
    TASK_CHECK(e->functions_count == 0 && e->functions == NULL && e->name == NULL);
    fixture_close(&c);
    /* No data directories at all (PE32+): still a successful empty result. */
    fixture(b, 64);
    put32(b, DIR0_PE64 - 4, 0); /* NumberOfRvaAndSizes = 0 */
    TASK_CHECK(fixture_open(&c, b, sizeof(b)) == LIBPE_E_OK);
    e = pe_exports(&c);
    TASK_CHECK(e != NULL && e->err == LIBPE_E_OK);
    TASK_CHECK(e->functions_count == 0 && e->functions == NULL && e->name == NULL);
    fixture_close(&c);
    return 0;
}

static int case_huge_count_limited_stack(void) {
    pid_t pid = fork();
    TASK_CHECK(pid >= 0);
    if (pid == 0) {
        /* A crash (e.g. stack overflow from a count-sized VLA) must not be
         * able to masquerade as success: only a clean _exit(0) passes. */
        struct rlimit lim = {1024 * 1024, 1024 * 1024};
        struct rlimit core = {0, 0};
        if (setrlimit(RLIMIT_STACK, &lim) || setrlimit(RLIMIT_CORE, &core))
            _exit(90);
        unsigned char *p = malloc(FIXTURE_SIZE);
        if (!p)
            _exit(91);
        pe_ctx_t c;
        /* Huge NumberOfFunctions with a much shorter mapped table. */
        fixture(p, 64);
        put32(p, EXP_NUM_FUNCTIONS, 0x10000000u);
        if (fixture_open(&c, p, FIXTURE_SIZE))
            _exit(92);
        pe_exports_t *e = pe_exports(&c);
        if (!check_empty_error(e))
            _exit(93);
        fixture_close(&c);
        /* Huge NumberOfNames must be rejected before allocation as well. */
        fixture(p, 32);
        put32(p, EXP_NUM_NAMES, 0x40000000u);
        if (fixture_open(&c, p, FIXTURE_SIZE))
            _exit(94);
        e = pe_exports(&c);
        if (!check_empty_error(e))
            _exit(95);
        fixture_close(&c);
        free(p);
        _exit(0);
    }
    int st;
    TASK_CHECK(waitpid(pid, &st, 0) == pid);
    TASK_CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    return 0;
}

static int case_short_eat_no_partial(void) {
    unsigned char b[FIXTURE_SIZE]; pe_ctx_t c;
    fixture(b, 32);
    put32(b, EXP_ADDR_FUNCTIONS, 0x1dfc); /* EAT starts near end of file */
    put32(b, 0xffc, 0x1400); /* only one of two entries is mapped */
    TASK_CHECK(fixture_open(&c, b, sizeof(b)) == LIBPE_E_OK);
    pe_exports_t *e = pe_exports(&c);
    TASK_CHECK(check_empty_error(e));
    /* No usable success prefix may survive, and the error result is cached. */
    TASK_CHECK(pe_exports(&c) == e);
    TASK_CHECK(check_empty_error(e));
    fixture_close(&c);
    return 0;
}

static int case_error_after_nonfinal_entry(void) {
    unsigned char b[FIXTURE_SIZE]; pe_ctx_t c;
    fixture(b, 32);
    put32(b, EXP_NUM_NAMES, 3);
    put32(b, 0x254, 0x1f00); /* names[1] -> outside the mapping */
    put32(b, 0x258, 0x1098); /* names[2] -> "Beta" (never reached) */
    put16(b, 0x264, 1); /* ordinals[2] = 1 */
    TASK_CHECK(fixture_open(&c, b, sizeof(b)) == LIBPE_E_OK);
    pe_exports_t *e = pe_exports(&c);
    /* names[0] was fine, the failure happens at a later entry: the whole
     * parse must still fail with an empty result. */
    TASK_CHECK(check_empty_error(e));
    fixture_close(&c);
    return 0;
}

static int case_ordinal_out_of_range(void) {
    unsigned char b[FIXTURE_SIZE]; pe_ctx_t c;
    fixture(b, 64);
    put16(b, 0x262, 2); /* name ordinal 2 >= NumberOfFunctions (2) */
    TASK_CHECK(fixture_open(&c, b, sizeof(b)) == LIBPE_E_OK);
    pe_exports_t *e = pe_exports(&c);
    TASK_CHECK(check_empty_error(e));
    fixture_close(&c);
    return 0;
}

static int case_base_plus_slot_overflow(void) {
    unsigned char b[FIXTURE_SIZE]; pe_ctx_t c;
    /* Base + slot index would wrap the 32-bit ordinal. */
    fixture(b, 32);
    put32(b, EXP_BASE, 0xffffffffu);
    TASK_CHECK(fixture_open(&c, b, sizeof(b)) == LIBPE_E_OK);
    pe_exports_t *e = pe_exports(&c);
    TASK_CHECK(check_empty_error(e));
    fixture_close(&c);
    /* The largest non-wrapping base still parses. */
    fixture(b, 32);
    put32(b, EXP_BASE, 0xfffffffeu);
    TASK_CHECK(fixture_open(&c, b, sizeof(b)) == LIBPE_E_OK);
    e = pe_exports(&c);
    TASK_CHECK(e != NULL && e->err == LIBPE_E_OK && e->functions_count == 2);
    TASK_CHECK(e->functions[0].ordinal == 0xfffffffeu);
    TASK_CHECK(e->functions[1].ordinal == 0xffffffffu);
    fixture_close(&c);
    return 0;
}

static int case_forwarder_dir_boundaries(void) {
    unsigned char b[FIXTURE_SIZE]; pe_ctx_t c;
    /* Address exactly at the end of [va, va+Size) is NOT a forwarder. */
    fixture(b, 32);
    put32(b, 0x244, 0x1100); /* EAT[1] = dir end */
    TASK_CHECK(fixture_open(&c, b, sizeof(b)) == LIBPE_E_OK);
    pe_exports_t *e = pe_exports(&c);
    TASK_CHECK(e != NULL && e->err == LIBPE_E_OK);
    TASK_CHECK(e->functions[1].address == 0x1100);
    TASK_CHECK(e->functions[1].fwd_name == NULL);
    fixture_close(&c);
    /* NUL at the last byte inside the range: valid empty forwarder string. */
    fixture(b, 32);
    put32(b, 0x244, 0x10ff); /* byte at ofs 0x2ff is already zero */
    TASK_CHECK(fixture_open(&c, b, sizeof(b)) == LIBPE_E_OK);
    e = pe_exports(&c);
    TASK_CHECK(e != NULL && e->err == LIBPE_E_OK);
    TASK_CHECK(e->functions[1].fwd_name != NULL && e->functions[1].fwd_name[0] == '\0');
    fixture_close(&c);
    /* No NUL inside the directory range: must fail even though zero bytes
     * exist in the mapping right past the range end. */
    fixture(b, 32);
    put32(b, 0x244, 0x10f8);
    memset(b + 0x2f8, 'A', 8); /* covers RVA 0x10f8..0x10ff */
    TASK_CHECK(fixture_open(&c, b, sizeof(b)) == LIBPE_E_OK);
    e = pe_exports(&c);
    TASK_CHECK(check_empty_error(e));
    fixture_close(&c);
    return 0;
}

/* --------------------------------------------------------------- strings */

static int case_dll_name_missing_nul(void) {
    unsigned char b[FIXTURE_SIZE]; pe_ctx_t c;
    fixture(b, 32);
    memset(b + 0x270, 'X', FIXTURE_SIZE - 0x270); /* no NUL before map end */
    TASK_CHECK(fixture_open(&c, b, sizeof(b)) == LIBPE_E_OK);
    pe_exports_t *e = pe_exports(&c);
    TASK_CHECK(check_empty_error(e));
    fixture_close(&c);
    return 0;
}

static int case_function_name_missing_nul(void) {
    unsigned char b[FIXTURE_SIZE]; pe_ctx_t c;
    fixture(b, 64);
    memset(b + 0x290, 'Y', FIXTURE_SIZE - 0x290); /* "Alpha" loses its NUL */
    TASK_CHECK(fixture_open(&c, b, sizeof(b)) == LIBPE_E_OK);
    pe_exports_t *e = pe_exports(&c);
    TASK_CHECK(check_empty_error(e));
    fixture_close(&c);
    return 0;
}

static int case_forwarder_missing_nul(void) {
    unsigned char b[FIXTURE_SIZE]; pe_ctx_t c;
    fixture(b, 32);
    memset(b + 0x2a0, 'Z', FIXTURE_SIZE - 0x2a0); /* "other.Func" unterminated */
    TASK_CHECK(fixture_open(&c, b, sizeof(b)) == LIBPE_E_OK);
    pe_exports_t *e = pe_exports(&c);
    TASK_CHECK(check_empty_error(e));
    fixture_close(&c);
    return 0;
}

static int case_long_names_not_truncated(void) {
    unsigned char b[FIXTURE_SIZE]; pe_ctx_t c;
    fixture(b, 32);
    /* 500-byte function name at ofs 0x300 (RVA 0x1100). */
    memset(b + 0x300, 'L', 500);
    b[0x300 + 500] = '\0';
    put32(b, 0x250, 0x1100); /* names[0] */
    /* 400-byte DLL name at ofs 0x600 (RVA 0x1400). */
    memset(b + 0x600, 'D', 400);
    b[0x600 + 400] = '\0';
    put32(b, EXP_NAME, 0x1400);
    TASK_CHECK(fixture_open(&c, b, sizeof(b)) == LIBPE_E_OK);
    pe_exports_t *e = pe_exports(&c);
    TASK_CHECK(e != NULL && e->err == LIBPE_E_OK);
    /* Longer than the old fixed 300-byte buffer: no truncation. */
    TASK_CHECK(e->name != NULL && strlen(e->name) == 400);
    TASK_CHECK(e->name[0] == 'D' && e->name[399] == 'D');
    TASK_CHECK(strlen(e->functions[0].name) == 500);
    TASK_CHECK(e->functions[0].name[0] == 'L' && e->functions[0].name[499] == 'L');
    fixture_close(&c);
    return 0;
}

static int case_duplicate_names_last_wins(void) {
    unsigned char b[FIXTURE_SIZE]; pe_ctx_t c;
    fixture(b, 32);
    put16(b, 0x262, 0); /* both names reference slot 0 */
    TASK_CHECK(fixture_open(&c, b, sizeof(b)) == LIBPE_E_OK);
    pe_exports_t *e = pe_exports(&c);
    TASK_CHECK(e != NULL && e->err == LIBPE_E_OK && e->functions_count == 2);
    /* The last name referencing a slot wins, together with its hint. */
    TASK_CHECK(strcmp(e->functions[0].name, "Beta") == 0);
    TASK_CHECK(e->functions[0].hint == 1);
    /* The other slot stays unnamed but keeps an empty name. */
    TASK_CHECK(e->functions[1].name != NULL && e->functions[1].name[0] == '\0');
    TASK_CHECK(e->functions[1].hint == 0);
    fixture_close(&c);
    return 0;
}

static int case_unnamed_and_zero_rva_slots(void) {
    unsigned char b[FIXTURE_SIZE]; pe_ctx_t c;
    fixture(b, 64);
    put32(b, EXP_NUM_NAMES, 0); /* everything exported by ordinal */
    put32(b, 0x244, 0); /* EAT[1] = zero RVA, slot is kept */
    TASK_CHECK(fixture_open(&c, b, sizeof(b)) == LIBPE_E_OK);
    pe_exports_t *e = pe_exports(&c);
    TASK_CHECK(e != NULL && e->err == LIBPE_E_OK && e->functions_count == 2);
    TASK_CHECK(e->functions[0].ordinal == 7 && e->functions[0].address == 0x1400);
    TASK_CHECK(e->functions[0].name != NULL && e->functions[0].name[0] == '\0');
    TASK_CHECK(e->functions[0].hint == 0 && e->functions[0].fwd_name == NULL);
    TASK_CHECK(e->functions[1].ordinal == 8 && e->functions[1].address == 0);
    TASK_CHECK(e->functions[1].name != NULL && e->functions[1].name[0] == '\0');
    TASK_CHECK(e->functions[1].fwd_name == NULL);
    fixture_close(&c);
    return 0;
}

/* -------------------------------------------------------------- lifetime */

static int case_repeated_reads_cached(void) {
    unsigned char b[FIXTURE_SIZE]; pe_ctx_t c;
    fixture(b, 32);
    TASK_CHECK(fixture_open(&c, b, sizeof(b)) == LIBPE_E_OK);
    pe_exports_t *e1 = pe_exports(&c);
    pe_exports_t *e2 = pe_exports(&c);
    TASK_CHECK(e1 != NULL && e1 == e2 && e1->err == LIBPE_E_OK);
    TASK_CHECK(e1->functions_count == 2);
    fixture_close(&c);
    /* The empty error result is cached identically. */
    fixture(b, 32);
    put32(b, EXP_ADDR_FUNCTIONS, 0x1dfc);
    TASK_CHECK(fixture_open(&c, b, sizeof(b)) == LIBPE_E_OK);
    e1 = pe_exports(&c);
    e2 = pe_exports(&c);
    TASK_CHECK(e1 != NULL && e1 == e2);
    TASK_CHECK(check_empty_error(e1));
    fixture_close(&c);
    return 0;
}

static int case_dealloc_releases_all(void) {
    unsigned char b[FIXTURE_SIZE]; pe_ctx_t c;
    fixture(b, 32);
    TASK_CHECK(fixture_open(&c, b, sizeof(b)) == LIBPE_E_OK);
    pe_exports_t *e = pe_exports(&c);
    TASK_CHECK(e != NULL && e->err == LIBPE_E_OK);
    TASK_CHECK(e->functions[1].fwd_name != NULL); /* fwd_name is allocated */
    pe_exports_dealloc(e); /* frees name, functions, names and fwd_name */
    c.cached_data.exports = NULL;
    /* A fresh parse after dealloc reproduces the full result. */
    e = pe_exports(&c);
    TASK_CHECK(e != NULL && e->err == LIBPE_E_OK);
    TASK_CHECK(strcmp(e->functions[1].fwd_name, "other.Func") == 0);
    fixture_close(&c); /* deallocs the second result */
    pe_exports_dealloc(NULL); /* must be a no-op */
    /* Dealloc of an empty error result must be safe too. */
    fixture(b, 32);
    put32(b, EXP_ADDR_FUNCTIONS, 0x1dfc);
    TASK_CHECK(fixture_open(&c, b, sizeof(b)) == LIBPE_E_OK);
    e = pe_exports(&c);
    TASK_CHECK(check_empty_error(e));
    fixture_close(&c);
    return 0;
}

static int case_isolation_and_readonly(void) {
    unsigned char bad[FIXTURE_SIZE], good[FIXTURE_SIZE], pristine[FIXTURE_SIZE];
    pe_ctx_t c1, c2;
    fixture(bad, 32);
    put32(bad, EXP_ADDR_FUNCTIONS, 0x1dfc); /* corrupt: short EAT */
    fixture(good, 64);
    memcpy(pristine, good, FIXTURE_SIZE);
    TASK_CHECK(fixture_open(&c1, bad, sizeof(bad)) == LIBPE_E_OK);
    pe_exports_t *e1 = pe_exports(&c1);
    TASK_CHECK(check_empty_error(e1));
    /* A bad file must not affect another ctx parsing a good file. */
    TASK_CHECK(fixture_open(&c2, good, sizeof(good)) == LIBPE_E_OK);
    pe_exports_t *e2 = pe_exports(&c2);
    TASK_CHECK(e2 != NULL && e2->err == LIBPE_E_OK && e2->functions_count == 2);
    TASK_CHECK(strcmp(e2->name, "fixture.dll") == 0);
    TASK_CHECK(strcmp(e2->functions[0].name, "Alpha") == 0);
    /* The first ctx keeps its own error result. */
    TASK_CHECK(check_empty_error(e1));
    /* Input bytes stay read-only. */
    TASK_CHECK(memcmp(good, pristine, FIXTURE_SIZE) == 0);
    fixture_close(&c1);
    fixture_close(&c2);
    return 0;
}

/* ------------------------------------------------------------ registry */

const struct task_case task_cases[] = {
    {"tables", "normal_pe32_pe64", case_normal_pe32_pe64},
    {"tables", "no_export_directory", case_no_export_directory},
    {"tables", "huge_count_limited_stack", case_huge_count_limited_stack},
    {"tables", "short_eat_no_partial", case_short_eat_no_partial},
    {"tables", "error_after_nonfinal_entry", case_error_after_nonfinal_entry},
    {"tables", "ordinal_out_of_range", case_ordinal_out_of_range},
    {"tables", "base_plus_slot_overflow", case_base_plus_slot_overflow},
    {"tables", "forwarder_dir_boundaries", case_forwarder_dir_boundaries},
    {"strings", "dll_name_missing_nul", case_dll_name_missing_nul},
    {"strings", "function_name_missing_nul", case_function_name_missing_nul},
    {"strings", "forwarder_missing_nul", case_forwarder_missing_nul},
    {"strings", "long_names_not_truncated", case_long_names_not_truncated},
    {"strings", "duplicate_names_last_wins", case_duplicate_names_last_wins},
    {"strings", "unnamed_and_zero_rva_slots", case_unnamed_and_zero_rva_slots},
    {"lifetime", "repeated_reads_cached", case_repeated_reads_cached},
    {"lifetime", "dealloc_releases_all", case_dealloc_releases_all},
    {"lifetime", "isolation_and_readonly", case_isolation_and_readonly},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
