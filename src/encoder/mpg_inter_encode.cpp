// Production (non-template) parts of the optional zero-motion P-picture
// encoder - see mpg_inter_encode.h's file comment for the full design.
// Kept out of the header so the inter mode's non-template machinery has
// one compiled home under src/ (the rest of the encoder stays header-only
// templates, matching the decoder's layout); task_support/run.sh picks
// every src/**.cpp up automatically, and the CMake/ESP-IDF builds compile
// this translation unit as part of the library sources.
#include "encoder/mpg_inter_encode.h"

#include <cmath>
#include <cstdlib>

#include "decoder/mpg_macroblock.h"  // kInterAcShift, shared with decodeInterBlock()

namespace tinympg {

int32_t dequantInterMagnitude(int32_t mag, int qscale, int32_t matrixVal) {
  int32_t dq = ((2 * mag + 1) * qscale * matrixVal) >> kInterAcShift;
  return (dq - 1) | 1;
}

int32_t quantizeInterCoef(int32_t coef, int qscale, int32_t matrixVal) {
  if (qscale <= 0) qscale = 1;
  if (matrixVal <= 0) matrixVal = 1;
  // Algebraic center: decode weights the level as (2*mag+1), so the
  // matching round-to-nearest estimate is |coef|*16/(qscale*w) - 1/2.
  int32_t estimate =
      (int32_t)std::lround((float)std::abs(coef) * 16.0f / ((float)qscale * (float)matrixVal) -
                           0.5f);
  if (estimate < 0) estimate = 0;
  if (estimate > kMaxAcEscapeMagnitude) estimate = kMaxAcEscapeMagnitude;

  int32_t bestMag = 0;
  int32_t bestErr = std::abs(coef);  // magnitude 0 -> dequantizes to 0
  for (int32_t mag = estimate - 2; mag <= estimate + 2; mag++) {
    if (mag <= 0 || mag > kMaxAcEscapeMagnitude) continue;
    int32_t dq = dequantInterMagnitude(mag, qscale, matrixVal);
    int32_t err = std::abs(std::abs(coef) - dq);
    if (err < bestErr || (err == bestErr && mag < bestMag)) {
      bestErr = err;
      bestMag = mag;
    }
  }
  return coef < 0 ? -bestMag : bestMag;
}

void writeMbAddrIncrement(BitWriter& bw, int value) {
  while (value > 33) {
    bw.writeVlc(kMbAddrIncCode[kMbAddrIncEscapeIndex],
                kMbAddrIncBits[kMbAddrIncEscapeIndex]);  // macroblock_escape: +33
    value -= 33;
  }
  // Table row value-1 encodes a real increment of `value` (rows 0..32).
  bw.writeVlc(kMbAddrIncCode[value - 1], kMbAddrIncBits[value - 1]);
}

void writeInterBlock(BitWriter& bw, const int32_t levels[64]) {
  int lastCoded = -1;  // scan position of the previous coded coefficient (-1: none yet)
  for (int zz = 0; zz < 64; zz++) {
    int32_t level = levels[kZigzag[zz]];
    if (level == 0) continue;
    int mag = level < 0 ? -level : level;
    if (zz == 0 && mag == 1) {
      // decodeInterBlock()'s first-coefficient shortcut: '1' + sign.
      bw.flag(true);
      bw.flag(level < 0);
    } else {
      int run = zz - lastCoded - 1;
      writeAcCoefficient(bw, run, level);
    }
    lastCoded = zz;
  }
  bw.writeVlc(kAcCode[kAcEobIndex], kAcBits[kAcEobIndex]);
}

void writeInterMotionOnlyMacroblock(BitWriter& bw) {
  // Table B.2 row 2: explicit forward MV, no residual ('001').
  bw.writeVlc(kMbTypePCode[2], kMbTypePBits[2]);
  // f_code == 1, zero motion_code per component ('1', Table B.10 row 0):
  // each decodes to the (0,0) predictor with no residual bits.
  bw.writeVlc(kMotionCode[0], kMotionCodeBits[0]);  // horizontal
  bw.writeVlc(kMotionCode[0], kMotionCodeBits[0]);  // vertical
}

bool encodeInterBlockLevels(const uint8_t* srcPlane, int srcStride, const uint8_t* refPlane,
                             int refStride, int baseX, int baseY, int qscale,
                             int32_t levels[64]) {
  int32_t residual[64];
  for (int y = 0; y < 8; y++) {
    const uint8_t* srcRow = srcPlane + (size_t)(baseY + y) * srcStride + baseX;
    const uint8_t* refRow = refPlane + (size_t)(baseY + y) * refStride + baseX;
    for (int x = 0; x < 8; x++) {
      residual[y * 8 + x] = (int32_t)srcRow[x] - (int32_t)refRow[x];  // signed, +/-255
    }
  }

  int32_t coef[64];
  fdct8x8(residual, coef);

  bool any = false;
  for (int j = 0; j < 64; j++) {
    levels[j] = quantizeInterCoef(coef[j], qscale, (int32_t)kDefaultNonIntraMatrix[j]);
    if (levels[j] != 0) any = true;
  }
  return any;
}

}  // namespace tinympg
