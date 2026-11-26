/* udis86 - libudis86/amx.h
 *
 * Read-only Intel AMX tile static analysis. The analyzer decodes the
 * caller supplied basic blocks with udis86, propagates 64-bit GPR
 * constants over the explicit control-flow graph, tracks the tile
 * configuration state through LDTILECFG/STTILECFG/TILERELEASE and
 * derives the per-row memory footprint of tile load/store instructions.
 *
 * No instruction supplied to the analyzer is ever executed. Every value
 * handed back through struct ud_amx_report is owned by the report and
 * remains valid until ud_amx_report_free() is called; repeated queries do
 * not mutate the caller's input and never reference decoder scratch
 * buffers.
 */
#ifndef UD_AMX_H
#define UD_AMX_H

#include "udis86.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UD_AMX_TILES        8
#define UD_AMX_CFG_SIZE     64   /* gate size of the tile config blob */
#define UD_AMX_MAX_ROWS     16   /* palette 1 architectural limits */
#define UD_AMX_MAX_COLSB    64
#define UD_AMX_NO_CFG       ((unsigned) -1)

/* A read-only virtual address region backing config memory. */
struct ud_amx_region {
    uint64_t             addr;
    uint64_t             size;
    const uint8_t       *data; /* size bytes, never written */
};

/* A basic block: instruction bytes and explicit successor addresses. */
struct ud_amx_block {
    uint64_t             addr;
    const uint8_t       *bytes;
    size_t               size;
    const uint64_t      *succ;
    size_t               nsucc;
};

/* Optional entry GPR constant (UD_R_RAX .. UD_R_R15). */
struct ud_amx_gpr_init {
    unsigned             reg;
    uint64_t             value;
};

struct ud_amx_input {
    const struct ud_amx_block   *blocks;
    size_t                       nblocks;
    uint64_t                     entry;
    const struct ud_amx_gpr_init *gpr_init;
    size_t                       ngpr_init;
    const struct ud_amx_region  *regions;
    size_t                       nregions;
};

struct ud_amx_tile_shape {
    uint16_t             rows;
    uint16_t             colsb;
};

enum ud_amx_opr_kind {
    UD_AMX_OPR_NONE = 0,
    UD_AMX_OPR_GPR,
    UD_AMX_OPR_TILE,      /* tmm0-tmm7, distinct from vectors */
    UD_AMX_OPR_VECTOR,    /* xmm/ymm */
    UD_AMX_OPR_SEG,
    UD_AMX_OPR_MEM,
    UD_AMX_OPR_IMM,
    UD_AMX_OPR_JIMM,
    UD_AMX_OPR_CONST
};

/* Decoded memory operand. On tile data instructions index_reg is the
 * SIB index register and plays the stride role: it must not be folded
 * into the first-row effective address. */
struct ud_amx_mem {
    int                  present;
    int                  rip_relative;
    unsigned             base_reg;   /* UD_NONE when absent */
    unsigned             index_reg;  /* UD_NONE when absent */
    unsigned             scale;      /* decoded SIB scale, 0 == factor 1 */
    int64_t              disp;       /* sign extended displacement */
};

struct ud_amx_opr {
    enum ud_amx_opr_kind kind;
    unsigned             reg;   /* ud_type register id for register ops */
    unsigned             tile;  /* 0..7 for UD_AMX_OPR_TILE */
    unsigned             size;
    int64_t              imm;
    struct ud_amx_mem    mem;
};

enum ud_amx_site_kind {
    UD_AMX_SITE_LDTILECFG = 0,
    UD_AMX_SITE_STTILECFG,
    UD_AMX_SITE_TILERELEASE,
    UD_AMX_SITE_TILEZERO,
    UD_AMX_SITE_TILELOADD,
    UD_AMX_SITE_TILELOADDT1,
    UD_AMX_SITE_TILESTORED,
    UD_AMX_SITE_TDPBSSD
};

enum ud_amx_state_kind {
    UD_AMX_ST_KNOWN = 0,   /* resolved palette 1 configuration */
    UD_AMX_ST_UNKNOWN,     /* configuration cannot be located/trusted */
    UD_AMX_ST_RELEASED     /* palette 0 / TILERELEASE */
};

enum ud_amx_tile_role {
    UD_AMX_TILE_READ = 0,
    UD_AMX_TILE_WRITE,
    UD_AMX_TILE_RDWR
};

struct ud_amx_tile_ref {
    unsigned             tile;       /* 0..7 */
    unsigned             opr_index;  /* operand position, order preserved */
    enum ud_amx_tile_role role;
};

/* One reachable configuration possibility at a tile use point. */
struct ud_amx_state {
    enum ud_amx_state_kind kind;
    unsigned             cfg_id;     /* index into report->configs */
    const uint64_t      *load_sites; /* LDTILECFG addresses reaching here */
    size_t               nload_sites;
    const uint64_t      *cfg_addrs;  /* config blob addresses loaded */
    size_t               ncfg_addrs;
    const uint64_t      *release_sites;
    size_t               nrelease_sites;
    uint64_t             note;       /* origin site for UNKNOWN states */
    int                  dims_compatible; /* TDPBSSD: 1 yes, 0 no, -1 n/a */
};

/* Reasons a footprint row cannot be resolved to concrete addresses. */
#define UD_AMX_MISS_BASE    (1u << 0)
#define UD_AMX_MISS_STRIDE  (1u << 1)
#define UD_AMX_MISS_CONFIG  (1u << 2)
#define UD_AMX_MISS_SHAPE   (1u << 3)

struct ud_amx_row {
    unsigned             tile;
    unsigned             row;        /* row number kept even on overlap */
    unsigned             cfg_id;
    int                  resolved;
    uint64_t             lo;         /* half-open [lo, hi) byte interval */
    uint64_t             hi;
    unsigned             missing;    /* UD_AMX_MISS_* when unresolved */
    /* symbolic roles retained when concrete addresses are unavailable */
    struct ud_amx_mem    sym;
};

struct ud_amx_site {
    enum ud_amx_site_kind kind;
    uint64_t             addr;
    unsigned             length;
    int                  config_load;
    int                  config_write;
    int                  released;
    struct ud_amx_mem    mem;        /* config or data memory operand */
    const struct ud_amx_tile_ref *tiles;
    unsigned             ntiles;
    const struct ud_amx_state *states;
    size_t               nstates;
    const struct ud_amx_row *rows;  /* tile data instructions only */
    size_t               nrows;
    unsigned             missing;
};

struct ud_amx_insn {
    uint64_t             addr;
    uint64_t             block_addr;
    unsigned             length;
    unsigned             mnemonic;   /* enum ud_mnemonic_code */
    const char          *mnemonic_str;
    const struct ud_amx_opr *oprs;
    unsigned             noprs;
    /* GPR constants on entry to this instruction */
    const uint64_t      *gpr_val;    /* 16 slots, RAX .. R15 */
    const unsigned char *gpr_known;  /* 16 slots */
    int                  site_index; /* -1 when not an AMX instruction */
    int                  cleared;    /* conservative clear happened here */
};

/* A deduplicated palette 1 configuration content. Provenance (which
 * loads carried it to a given point) lives on struct ud_amx_state so
 * only reachable sources are reported at a use point. */
struct ud_amx_config {
    int                  start_row;
    struct ud_amx_tile_shape shape[UD_AMX_TILES];
};

struct ud_amx_report {
    const struct ud_amx_insn  *insns;
    size_t                      ninsns;
    const struct ud_amx_site  *sites;
    size_t                      nsites;
    const struct ud_amx_config *configs;
    size_t                      nconfigs;
    const uint64_t            *clear_sites;
    size_t                      nclear_sites;
};

/* Runs the analysis. Returns NULL on malformed caller input. The input
 * is treated as read-only and is never executed. */
const struct ud_amx_report *ud_amx_analyze(const struct ud_amx_input *in);

/* Releases all storage owned by the report. */
void ud_amx_report_free(const struct ud_amx_report *report);

/* Helpers exported for callers and tests. */
int ud_amx_is_tile_reg(unsigned ud_reg);
int ud_amx_is_vector_reg(unsigned ud_reg);
int ud_amx_is_gpr64(unsigned ud_reg);
unsigned ud_amx_gpr_slot(unsigned ud_reg); /* 0..15, undef for non-GPR */
const char *ud_amx_site_kind_str(enum ud_amx_site_kind kind);

#ifdef __cplusplus
}
#endif

#endif /* UD_AMX_H */
