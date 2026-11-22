#pragma once
#include <stdint.h>
#include "h264_macroblock_encode.h"
#include "../common/h264_frame.h"

/*
 * Optional P-macroblock partition motion search (P_16x16/P_16x8/P_8x16/
 * P_8x8 with 8x8/8x4/4x8/4x4 sub-partitions).
 *
 * This is a separate translation unit (h264_partition_search.cpp) rather
 * than a header-only inline implementation because the exhaustive shape
 * search is large and would otherwise be duplicated into every translation
 * unit. The declaration lives here so encoder/h264_encoder.h can stay a
 * plain header; the implementation is linked from the library target (see
 * CMakeLists.txt) and from task_support/run.sh's production-source glob.
 *
 * The search is deliberately exhaustive and deterministic:
 *  - every partition's integer-pel motion is found over the same
 *    +/-motionSearchRange window the existing 16x16 search uses;
 *  - each candidate's cost is the partition's raw luma SAD plus
 *    L * signed-Exp-Golomb bits of its actual MVD, where
 *    L = 1 + floor(picture QP / 6);
 *  - a candidate tie is broken by raw SAD, then |dx|+|dy|, then mvY, then
 *    mvX;
 *  - shapes are compared by the sum of their partitions' costs plus the
 *    mb_type/sub_mb_type codeword bits, with the smaller mb_type (and, for
 *    an 8x8 region, the smaller sub_mb_type) winning a total-cost tie;
 *  - only already-selected earlier partitions' MVs take part in later
 *    partitions' predictors; trial MVs never escape a rejected shape.
 *
 * See docs/partition-search.md for the full rule description.
 */

namespace tinyh264 {

/// One selected partition, in the same order it is signalled on the wire.
struct PartitionSearchPart {
  uint8_t bx = 0, by = 0;  ///< top-left position in 4x4 units within the MB
  uint8_t pw = 0, ph = 0;  ///< size in 4x4 units
  int16_t mv[2] = {0, 0};  ///< final MV, quarter-pel units
  int16_t mvd[2] = {0, 0}; ///< actual MVD (mv minus the H.264 predictor)
  int32_t sad = 0;         ///< raw luma SAD for this partition
};

/// Result of one macroblock's exhaustive partition/shape search.
struct PartitionSearchResult {
  uint8_t mbType = 0;      ///< P mb_type: 0=16x16, 1=16x8, 2=8x16, 3=8x8
  uint8_t subMbType[4] = {0, 0, 0, 0}; ///< valid when mbType == 3
  int numParts = 0;        ///< 1..16
  PartitionSearchPart parts[16];
  int32_t totalSad = 0;    ///< sum of all selected partitions' raw SAD
  int64_t totalCost = 0;   ///< SAD + L * (mvd + shape) syntax bits
};

/**
 * Runs the exhaustive partition search for the macroblock described by
 * `ctx` against the single reference picture `ref`, for picture QP
 * `targetQp` and search window +/-`range`. Writes the winning shape,
 * partition layout, MVs and MVDs into `*out`. Does not touch `ctx.frame`
 * or the macroblock-info table: all trial state is local, so a rejected
 * candidate can never leak into a later candidate or neighbour.
 *
 * Returns false only for an invalid argument (should not happen for the
 * validated sizes the encoder uses).
 */
bool searchP16Partitions(const MbEncodeContext& ctx, const Frame& ref,
                         int range, int targetQp,
                         PartitionSearchResult* out);

/**
 * Writes the macroblock_layer() syntax and reconstructs one Inter
 * macroblock using an already-selected partition layout from
 * searchP16Partitions(). This is the "finish" half of the partition
 * encoder: it re-runs motion compensation for every selected partition
 * (so an earlier Intra trial that clobbered the prediction planes cannot
 * survive), then quantizes/codes the whole-macroblock residual and writes
 * mb_type/sub_mb_type/mvd_l0/cbp/residual. `r.parts[].mvd` must already
 * be the actual MVD against the predictors this same macroblock's
 * neighbourhood produces, exactly as the search left them in the
 * macroblock-info table. Returns the running QP after this macroblock.
 */
int encodeInterPartitionMacroblock(BitWriter& bw, MbEncodeContext& ctx,
                                    const Frame& ref,
                                    const PartitionSearchResult& r,
                                    int qpPrev, int targetQp);

}  // namespace tinyh264
