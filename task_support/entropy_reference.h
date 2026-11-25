/*
 * Independent entropy reference for the LC3 spectral arithmetic coder.
 *
 * This header and entropy_reference.cpp are a from-scratch description of
 * the LC3 packet entropy layer:
 *
 *   - a plain byte array,
 *   - a backward, LSB-first cursor for the ordinary bit plane,
 *   - a 24-bit interval (arithmetic) coder reading forward,
 *   - symbol resolution by linear interval search,
 *   - the spectral pair/escape/context/LSB/residual/noise/gain machinery.
 *
 * It never calls the production bits.c/spec.c readers, writers, budget or
 * termination code, and it does not copy their accumulator cache or binary
 * search lookup. The probability distributions and spectral context
 * constants of tables.c are format data and are reached only through the
 * C bridge accessors declared below.
 *
 * No production header is included: the arithmetic model layout is mirrored
 * by an ABI-identical POD type, so this file compiles as plain C++17.
 */

#ifndef LC3_ENTROPY_REFERENCE_H
#define LC3_ENTROPY_REFERENCE_H

#include <cstdint>
#include <vector>

namespace entref {

/* ABI-identical mirror of struct lc3_ac_symbol / struct lc3_ac_model:
 * 17 cumulative-frequency intervals, each a (low, range) uint16 pair. */
struct AcModelPod {
    uint16_t low;
    uint16_t range;
};
struct AcModelTable {
    AcModelPod s[17];
};

/* C bridge accessors into the tables.c format data, implemented in
 * target_bridge.c. The reference never dereferences production structs. */
extern "C" {
uint8_t bridge_lut(int high_rate, int half, int state, int k);
const void* bridge_spectrum_model(int index);
const void* bridge_tns_order_model(int index);
const void* bridge_tns_coeff_model(int index);
}

static inline const AcModelTable* spectrum_model(int index)
{
    return static_cast<const AcModelTable*>(bridge_spectrum_model(index));
}
static inline const AcModelTable* tns_order_model(int index)
{
    return static_cast<const AcModelTable*>(bridge_tns_order_model(index));
}
static inline const AcModelTable* tns_coeff_model(int index)
{
    return static_cast<const AcModelTable*>(bridge_tns_coeff_model(index));
}

/** Number of encoded spectral coefficients for a duration/rate. */
int ne_of(int dt, int sr);

/** Frame side info width for the coded coefficient count. */
int nbits_nq_of(int dt, int sr);

/** Production gain offset formula, evaluated independently. */
int gain_offset(int sr, int nbytes);

/** High-rate / LSB mode thresholds from the packet size. */
bool high_rate_mode(int sr, int nbytes);
bool lsb_allowed(int sr, int nbytes);

/** Unquantization gain, evaluated as double pow(10, g/28). */
double unquant_gain(int g_int);

/* ------------------------------------------------------------------ */
/* Byte storage                                                        */
/* ------------------------------------------------------------------ */

struct ByteStore {
    std::vector<uint8_t> b;
    explicit ByteStore(int nbytes) : b(nbytes, 0) {}
    int size() const { return (int)b.size(); }
};

/* ------------------------------------------------------------------ */
/* Backward ordinary-bit cursor, LSB first from the last byte.        */
/* Bit j lives in byte (nbytes-1-j/8), at bit position (j mod 8).     */
/* ------------------------------------------------------------------ */

struct BackWriter {
    ByteStore& store;
    long pos = 0;
    explicit BackWriter(ByteStore& s) : store(s) {}
    void write_bit(int bit);
    void write(unsigned value, int n);
    long bits_done() const { return pos; }
};

struct BackReader {
    const uint8_t* p;
    int nbytes;
    long pos;           /* absolute ordinary-bit index from packet end */
    long start;         /* first index owned by this reader */
    long limit;         /* exclusive upper index that may be consumed */
    BackReader(const uint8_t* data, int len, long start_pos, long bit_limit);
    bool read_bit(int& out);            /* false: budget exhausted */
    bool read(int n, unsigned& out);    /* false: budget exhausted */
    long bits_left() const { return limit - pos; }
    long consumed() const { return pos - start; }
};

/* ------------------------------------------------------------------ */
/* 24-bit interval arithmetic coder, written forward.                 */
/* Symbol coding uses the raw cumulative-frequency intervals; decode  */
/* picks a symbol by linear search through the intervals.             */
/* ------------------------------------------------------------------ */

struct AcEncoder {
    ByteStore* store = nullptr;
    uint32_t low = 0, range = 0xffffff;
    int cache = -1, carry = 0, carry_count = 0;
    int fw = 0;                  /* next forward byte index */
    long shift_events = 0;
    long carry_events = 0;
    long pending_ff_peak = 0;

    void begin(ByteStore& s) { store = &s; }
    void put_symbol(const AcModelTable* model, unsigned sym);
    void write_renorm();
    void shift_byte();
    int range_bits() const;
    int pending_bits() const;
    /* Flush gap zeros (ordinary side) and terminate the interval coder,
     * sharing the final byte with the backward bit plane. */
    void terminate(BackWriter& bw);
};

struct AcDecoder {
    const uint8_t* p;
    int nbytes;
    uint32_t low = 0, range = 0xffffff;
    int fw = 3;
    bool error = false;
    long searches = 0;

    AcDecoder(const uint8_t* data, int len);
    unsigned get_symbol_linear(const AcModelTable* model);
    void read_renorm();
    int range_bits() const;
    /* Independent logical remaining bit count, from the format-level
     * forward/backward cursor positions (the same bookkeeping as a
     * specification reader), not from any production cache. */
    int logical_bits_left(int nbytes, long back_consumed) const;
};

/* ------------------------------------------------------------------ */
/* Spectral plan and independently decoded result.                    */
/* ------------------------------------------------------------------ */

struct PairTrace {
    int pair = 0;
    int half = 0;
    uint8_t state_in = 0;
    int model_base[4] = {0, 0, 0, 0};
    int escape_layers = 0;      /* number of escape (16) symbols */
    std::vector<unsigned> symbols;
    std::vector<int> lsb_bits;  /* ordinary bits consumed between symbols */
    int sign_bits = 0;
    int q0 = 0, q1 = 0;         /* signed integer quantized pair */
    bool stop_at_sign = false;  /* budget ran out before a sign bit */
};

struct SpecPlan {
    int dt = 0, sr = 0, nbytes = 0;
    int bw = 0;
    int nq = 0;
    bool lsb_mode = false;
    int g_idx = 0, nf = 0;
    std::vector<int> xq;              /* nq signed integer coefficients */
    std::vector<int> residual_bits;   /* explicit plan when LSB is off */
    std::vector<int> lsb_extra_bits;  /* explicit plan for LSB-on unit
                                         sign insertion positions */
};

struct SpecResult {
    bool ok = false;
    bool stopped_early = false;
    int stop_pair = -1;
    const char* stop_reason = "";
    std::vector<int> quant;           /* integer quantized values */
    std::vector<double> refined;      /* after residual/LSB correction */
    std::vector<double> gained;       /* after gain and noise filling */
    uint16_t nf_seed = 0;             /* final seed (after LSB adds) */
    uint16_t nf_seed_quant = 0;       /* seed right after quantization */
    std::vector<PairTrace> pairs;
    long total_sign_bits = 0;
    long total_ordinary_bits = 0;
};

/* Plan-only description of every coded pair: escape layers, sign
 * positions and context evolution, derived from the integer plan. */
std::vector<PairTrace> ref_describe_plan(const SpecPlan& plan);

/* Independent encoder for the spectral section of a packet. */
void ref_encode_spectrum(const SpecPlan& plan,
                         AcEncoder& ac, BackWriter& bw);

/* Independent decoder. The ordinary reader budget makes truncation
 * explicit: reading stops at a pair boundary, before a sign bit, or in
 * the middle of a pair, never reading past the budget as implicit zeros.
 * The coder and reader are passed in so a full-frame reader can first
 * walk the side info and TNS symbols from the packet start. */
SpecResult ref_decode_spectrum(const SpecPlan& plan,
                               AcDecoder& ac, BackReader& bw);

/* Noise factor estimator, mirrored for cross-checking the bridge. */
int ref_estimate_noise(int dt, int bw,
                       const std::vector<int>& xq, int nq);

/* ------------------------------------------------------------------ */
/* Full frame building for the frames group.                          */
/* ------------------------------------------------------------------ */

struct TnsPlan {
    bool enabled = false;
    bool lpc_weighting = false;
    int order = 0;
    int rc[8] = {0};
};

struct FramePlan {
    int dt = 0, sr = 0, nbytes = 0;
    int bw = 0;
    SpecPlan spec;
    TnsPlan tns;
    bool pitch_present = false;
    int sns_lfcb = 0, sns_hfcb = 0;   /* plain SNS side data plan */
};

/* Write a complete legal packet with the independent writer. The TNS
 * arithmetic symbols and the spectral arithmetic symbols share one
 * forward interval-coder state. */
std::vector<uint8_t> ref_build_frame(const FramePlan& plan);

/* Drive the independent reader from the very first packet byte, walking
 * the known side info and TNS symbols before the spectrum. Returns the
 * independently decoded spectrum plus the ordinary/forward bit counts. */
struct FrameReadback {
    SpecResult spec;
    long ordinary_consumed = 0;
    long header_ordinary = 0;   /* ordinary bits before the spectrum */
    long tns_symbols = 0;
    bool ok = false;
};
FrameReadback ref_read_frame(const FramePlan& plan,
                             const uint8_t* data, int nbytes);

/* As ref_read_frame, but caps the ordinary plane at `ordinary_cap`
 * bits (absolute index from the packet end). Used to prove the decoder
 * stops explicitly at a pair/sign boundary instead of zero-filling. */
FrameReadback ref_read_frame_budget(const FramePlan& plan,
                                    const uint8_t* data, int nbytes,
                                    long ordinary_cap);

} /* namespace entref */

#endif /* LC3_ENTROPY_REFERENCE_H */
