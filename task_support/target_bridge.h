/*
 * Test bridge between the independent C++ entropy reference and the
 * real static stages of the production spectral decoder.
 *
 * The bridge is compiled as plain C and #includes ../src/spec.c, so it
 * can drive the file-local stages (get_quantized, get_residual,
 * get_lsb, estimate_noise, fill_noise, ...) over raw packet bytes.
 *
 * Expected values are never computed by the functions under test: the
 * C++ side brings its own plan and independent reference results.
 */

#ifndef LC3_TARGET_BRIDGE_H
#define LC3_TARGET_BRIDGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BRIDGE_MAX_NE 400   /* ordinary LC3 max: 10 ms at 48 kHz */

/* ---- Format data accessors (tables.c probability/context data) ---- */

uint8_t bridge_lut(int high_rate, int half, int state, int k);
const void* bridge_spectrum_model(int index);
const void* bridge_tns_order_model(int index);
const void* bridge_tns_coeff_model(int index);

/* ---- Static spectral stages over a raw packet ---- */

int bridge_estimate_noise(int dt, int bw, const float* x, int nq);

int bridge_resolve_modes(int sr, int nbytes, int* lsb_mode);
int bridge_gain_offset(int sr, int nbytes);
float bridge_unquantize_gain(int g_int);

/* Single-stream run of every production spectral stage on a raw packet,
 * returning snapshots after quantization, after residual/LSB correction,
 * and after gain+noise filling. One bitstream context is used throughout
 * so the residual/LSB stage starts exactly where quantization stopped.
 * Expectations always come from the C++ reference. */
typedef struct {
    int ret;
    int nf;
    uint16_t nf_seed_quant;
    uint16_t nf_seed_final;
    int bits_left_after_quant;
    float xq[BRIDGE_MAX_NE];      /* integer-valued, post get_quantized */
    float xref[BRIDGE_MAX_NE];    /* post get_residual / get_lsb */
    float xfinal[BRIDGE_MAX_NE];  /* post gain + noise filling */
    int error;
} bridge_stages_t;

void bridge_spec_stages(const uint8_t* data, int nbytes,
    int dt, int sr, int bw, int nq, int lsb_mode, int g_idx,
    bridge_stages_t* out);

/* ---- Production bitstream plane writer/reader for cross-checks ---- */

#define BRIDGE_OP_SYMBOL 0   /* AC symbol, .model = spectrum model idx */
#define BRIDGE_OP_BIT    1   /* one ordinary bit */
#define BRIDGE_OP_BITS   2   /* .nbits ordinary bits, LSB first */

typedef struct {
    int kind;
    int model;
    unsigned sym;
    int nbits;
    unsigned value;
} bridge_op_t;

typedef struct {
    unsigned value[256];
    int symbols[256];
    int nops;
    int bits_left;
    int error;
} bridge_plane_t;

/* Write a packet with the production writer, following the op plan. */
void bridge_prod_write(int nbytes,
    const bridge_op_t* ops, int nops, uint8_t* out);

/* Read a packet with the production reader, following the same op plan
 * (symbols identify which model; bit ops give the width to consume). */
void bridge_prod_read(const uint8_t* data, int nbytes,
    const bridge_op_t* ops, int nops, bridge_plane_t* out);

/* ---- LC3_ENTROPY_TRACE read-only observation point ----
 * Declared unconditionally so the C++ test can call them regardless of
 * the macro on its own translation unit. The real, state-holding
 * implementation exists only under LC3_ENTROPY_TRACE in target_bridge.c;
 * a macro-off build gets empty stubs, so ordinary production builds add
 * no state, callback or allocation. */
void bridge_capture_spectrum(const float* x, int ne);
int bridge_trace_copy(float* dst, int cap);
void bridge_trace_reset(void);
int bridge_trace_frames(void);

#ifdef __cplusplus
}
#endif

#endif /* LC3_TARGET_BRIDGE_H */
