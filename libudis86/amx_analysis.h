/* udis86 - libudis86/amx_analysis.h
 *
 * Read-only, non-executing AMX tile analysis built on top of the udis86
 * decoder. The caller supplies basic blocks, an entry point, explicit
 * successors and read-only virtual address regions; the analyzer walks
 * the reachable control-flow graph, propagates 64-bit GPR constants and
 * tile-configuration state, and reports every AMX tile use point with
 * the configuration sources reachable there and the per-row memory
 * footprint of tile loads/stores.
 *
 * The returned report owns all of its storage. Repeated analyses never
 * mutate the caller's input and never alias decoder scratch buffers.
 */
#ifndef UD_AMX_ANALYSIS_H
#define UD_AMX_ANALYSIS_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UD_AMX_TILE_COUNT     8
#define UD_AMX_CONFIG_SIZE    64
#define UD_AMX_ROW_UNKNOWN    ((unsigned) -1)

/* A read-only virtual address region backing tile configuration bytes. */
struct ud_amx_ro_region {
    uint64_t       addr;
    const uint8_t *data;
    size_t         size;
};

/*
 * A basic block: its runtime address, the decoded instruction bytes and
 * the explicit virtual addresses of every successor block. An empty
 * successor list terminates control flow at this block.
 */
struct ud_amx_block {
    uint64_t         addr;
    const uint8_t   *bytes;
    size_t           size;
    const uint64_t  *succs;
    size_t           nsuccs;
};

/* A constant value for a 64-bit GPR on entry to the analysis. */
struct ud_amx_entry_reg {
    enum ud_type reg;   /* UD_R_RAX .. UD_R_R15 */
    uint64_t     value;
};

struct ud_amx_input {
    const struct ud_amx_block     *blocks;
    size_t                         nblocks;
    uint64_t                       entry;
    const struct ud_amx_entry_reg *entry_regs;
    size_t                         nentry_regs;
    const struct ud_amx_ro_region *regions;
    size_t                         nregions;
};

enum ud_amx_config_kind {
    UD_AMX_CFG_UNKNOWN  = 0,  /* a configuration exists but cannot be read */
    UD_AMX_CFG_RELEASED = 1,  /* palette 0 / TILERELEASE: no tiles configured */
    UD_AMX_CFG_PALETTE1 = 2   /* palette 1 with eight rows/colsb pairs */
};

/*
 * One reachable tile configuration. Configurations with identical
 * content are merged, while every LDTILECFG address that produced the
 * content is retained in sources[].
 */
struct ud_amx_config {
    enum ud_amx_config_kind kind;
    uint8_t  rows[UD_AMX_TILE_COUNT];   /* valid for PALETTE1, else zero */
    uint16_t colsb[UD_AMX_TILE_COUNT];  /* valid for PALETTE1, else zero */
    uint64_t *sources;                  /* LDTILECFG instruction addresses */
    size_t    nsources;
};

enum ud_amx_tile_role {
    UD_AMX_TILE_READ  = 1,
    UD_AMX_TILE_WRITE = 2,
    UD_AMX_TILE_RDWR  = 3
};

/* A tile register operand, in instruction operand order. */
struct ud_amx_tile_ref {
    unsigned               tile;      /* 0..7 */
    unsigned               op_index;  /* operand position, 0-based */
    enum ud_amx_tile_role  role;
};

/*
 * Symbolic/resolved memory descriptor. For tile load/store the SIB index
 * register is always reported as the stride register: it is never added
 * into the first-row address.
 */
struct ud_amx_mem_desc {
    int          present;
    enum ud_type base_reg;     /* UD_NONE when absent */
    enum ud_type index_reg;    /* stride register for tile ops; UD_NONE */
    uint8_t      scale;
    int64_t      disp;
    int          base_known;
    uint64_t     base_value;   /* first-row address when base resolves */
    int          stride_known;
    uint64_t     stride_value;
    int          is_rip;
    int          stride_role;  /* index acts as AMX row stride */
};

/* One half-open byte interval [lo, hi) of a single tile row. */
struct ud_amx_row_range {
    unsigned row;             /* UD_AMX_ROW_UNKNOWN in symbolic entries */
    uint64_t lo;
    uint64_t hi;
    int      complete;        /* zero when an address component is missing */
};

/*
 * Footprint of one tile instruction against one reachable configuration
 * source. Overlapping rows keep their individual row numbers; a missing
 * base/stride yields a single symbolic row entry instead of a guessed
 * enclosing interval.
 */
struct ud_amx_footprint {
    const struct ud_amx_config *config;
    unsigned                    tile;
    struct ud_amx_row_range    *rows;
    size_t                      nrows;
    int                         symbolic;
    const char                 *unknown_what; /* static string, or NULL */
};

/* One AMX instruction in program order. */
struct ud_amx_use {
    uint64_t                    insn_addr;
    enum ud_mnemonic_code       mnemonic;
    unsigned                    insn_len;

    struct ud_amx_tile_ref      tiles[4];
    size_t                      ntiles;

    struct ud_amx_mem_desc      mem;

    /* Every configuration reachable at this use point. */
    struct ud_amx_config      **configs;
    size_t                      nconfigs;

    /* TDPBSSD dimension verdict per entry of configs[]: -1 unknown, 0/1. */
    int                        *cfg_dims;

    /* One footprint per configuration for tile load/store only. */
    struct ud_amx_footprint    *footprints;
    size_t                      nfootprints;

    int                         is_config_load;  /* LDTILECFG */
    int                         is_config_write; /* STTILECFG */
    int                         dims_compatible; /* aggregate TDPBSSD verdict */
};

struct ud_amx_report {
    struct ud_amx_use *uses;
    size_t             nuses;

    /* Instructions that conservatively discarded GPR/config knowledge. */
    uint64_t          *clobber_sites;
    size_t             nclobber_sites;
};

/*
 * Analyze the supplied CFG. Returns a malloc'd report on success (never
 * NULL for a well-formed input whose entry block exists), or NULL when
 * the input is malformed. Release with ud_amx_report_free().
 */
struct ud_amx_report *ud_amx_analyze(const struct ud_amx_input *input);
void ud_amx_report_free(struct ud_amx_report *report);

/* Non-zero for the eight supported AMX mnemonics. */
int ud_amx_is_mnemonic(enum ud_mnemonic_code m);

#ifdef __cplusplus
}
#endif

#endif /* UD_AMX_ANALYSIS_H */
