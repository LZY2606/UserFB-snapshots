/*
 * Independent reference model for the HEVC intra reference-sample machinery.
 *
 * This module re-implements, from the ITU-T H.265 specification text, the parts
 * that the libde265 intra border code is checked against:
 *
 *   6.4.1     neighbouring block availability
 *   6.5.1/2   tile-scan and minimum-transform-block Z-scan address derivations
 *   8.4.4.2.2 reference sample substitution
 *   8.4.4.2.3 reference sample filtering
 *   8.4.4.2.4 planar intra prediction
 *   8.4.4.2.5 DC intra prediction
 *   8.4.4.2.6 angular intra prediction
 *
 * It deliberately includes none of libde265's headers and calls none of its
 * functions.  All inputs are the geometry/metadata description stored in
 * ref_scene (which the test also programs into the real SPS/PPS/de265_image
 * objects) plus pixel data that the test itself has written.
 */

#ifndef DE265_TESTS_INTRA_BORDER_REFMODEL_H
#define DE265_TESTS_INTRA_BORDER_REFMODEL_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

// Intra prediction mode numbers, as defined by HEVC.  libde265's
// IntraPredMode enumerators use the same numbering.
enum ref_intra_mode
{
  REF_INTRA_PLANAR     = 0,
  REF_INTRA_DC         = 1,
  REF_INTRA_ANGULAR_10 = 10,
  REF_INTRA_ANGULAR_18 = 18,
  REF_INTRA_ANGULAR_26 = 26,
  REF_INTRA_ANGULAR_34 = 34
};

struct ref_scene
{
  int picW = 0, picH = 0;   // luma samples (coded size, not conformance cropped)
  int log2Ctb = 0;
  int log2MinTb = 0;
  int log2MinCb = 0;

  int subW = 1, subH = 1;   // chroma subsampling
  int chromaArrayType = 1;  // 0 = mono, 1 = 4:2:0, 2 = 4:2:2, 3 = 4:4:4
  int bitDepth = 8;

  bool strongIntraSmoothing = false;
  bool constrainedIntra = false;
  bool intraSmoothingDisabled = false;

  // Tile boundaries, in CTB units.
  std::vector<int> colBd;
  std::vector<int> rowBd;

  // Slice id per CTB, raster order.
  std::vector<int> sliceOfCtb;

  // 1 = inter coded, per minimum coding block, raster order.
  std::vector<uint8_t> interCb;

  int ctbSize() const { return 1 << log2Ctb; }
  int minCbSize() const { return 1 << log2MinCb; }
  int picWidthInCtbs() const { return (picW + ctbSize() - 1) / ctbSize(); }
  int picHeightInCtbs() const { return (picH + ctbSize() - 1) / ctbSize(); }
  int picWidthInCbs() const { return (picW + minCbSize() - 1) / minCbSize(); }
  int picHeightInCbs() const { return (picH + minCbSize() - 1) / minCbSize(); }
};

int ref_ctb_addr_ts(const ref_scene& sc, int ctbX, int ctbY);      // 6.5.1
int ref_tile_id(const ref_scene& sc, int ctbX, int ctbY);
int ref_min_tb_addr_zs(const ref_scene& sc, int tbX, int tbY);     // 6.5.2

// 6.4.1 (with the coding constraints of 8.4.4.2): is the sample at luma
// position (xN,yN) available as a neighbour of the block whose top-left luma
// position is (xB,yB)?  Four independent reasons make it unavailable:
//   - it is outside the picture,
//   - it is in another tile or another slice,
//   - its minimum transform block has not been decoded yet (Z-scan order),
//   - constrained intra prediction is on and it is in an inter-coded block.
bool ref_neighbour_available(const ref_scene& sc, int xB, int yB, int xN, int yN);

inline int ref_log2(int v)
{
  int l = 0;
  while ((1 << l) < v) l++;
  return l;
}

// Produce the full 4*nT+1 reference samples:
//   border[-1-y] = component sample (xB-1, yB+y), y = 0..2nT-1
//   border[   0] = component sample (xB-1, yB-1)
//   border[ 1+x] = component sample (xB+x, yB-1), x = 0..2nT-1
// avail[] is the 0/1 availability mask for the same indices.  Substitution is
// applied exactly as in 8.4.4.2.2.
template <class pixel_t>
void ref_reference_samples(const ref_scene& sc,
                           const pixel_t* plane, ptrdiff_t stride,
                           int cIdx, int xB, int yB, int nT,
                           pixel_t* border, uint8_t* avail)
{
  const int sw = (cIdx == 0) ? 1 : sc.subW;
  const int sh = (cIdx == 0) ? 1 : sc.subH;
  const int bx = xB * sw;
  const int by = yB * sh;

  int nAvail = 0;
  for (int i = -2 * nT; i <= 2 * nT; i++) {
    int cx, cy;
    if (i < 0)       { cx = xB - 1; cy = yB + (-i - 1); }
    else if (i == 0) { cx = xB - 1; cy = yB - 1; }
    else             { cx = xB + (i - 1); cy = yB - 1; }

    const bool a = ref_neighbour_available(sc, bx, by, cx * sw, cy * sh);
    avail[i] = a ? 1 : 0;
    if (a) {
      border[i] = plane[(ptrdiff_t)cx + (ptrdiff_t)cy * stride];
      nAvail++;
    }
  }

  if (nAvail == 0) {
    const pixel_t mid = (pixel_t)(1 << (sc.bitDepth - 1));
    for (int i = -2 * nT; i <= 2 * nT; i++) border[i] = mid;
  }
  else if (nAvail != 4 * nT + 1) {
    // The first available sample in scan order (-2nT .. 2nT) is the
    // substitution source for the leading gap; later gaps copy the previous
    // (already substituted) sample.
    int first = -2 * nT;
    while (!avail[first]) first++;
    const pixel_t firstValue = border[first];

    if (!avail[-2 * nT]) border[-2 * nT] = firstValue;
    for (int i = -2 * nT + 1; i <= 2 * nT; i++)
      if (!avail[i]) border[i] = border[i - 1];
  }
}

// 8.4.4.2.3 reference sample filtering.  The process is only invoked for luma
// (and for chroma in 4:4:4), matching the condition under which
// decode_intra_prediction() calls it.
inline int ref_filter_flag(int nT, int mode)
{
  if (mode == REF_INTRA_DC || nT == 4) return 0;

  const int minDistVerHor = std::min(std::abs(mode - 26), std::abs(mode - 10));
  switch (nT) {
  case 8:  return (minDistVerHor > 7) ? 1 : 0;
  case 16: return (minDistVerHor > 1) ? 1 : 0;
  case 32: return (minDistVerHor > 0) ? 1 : 0;
  case 64: return 0;
  default: return 0;
  }
}

inline bool ref_filter_is_active(const ref_scene& sc, int nT, int cIdx, int mode)
{
  if (sc.intraSmoothingDisabled) return false;
  if (!(cIdx == 0 || sc.chromaArrayType == 3)) return false;
  return ref_filter_flag(nT, mode) != 0;
}

template <class pixel_t>
void ref_filter_border(const ref_scene& sc, pixel_t* p, int nT, int cIdx, int mode)
{
  if (!ref_filter_is_active(sc, nT, cIdx, mode)) return;

  int biIntFlag = 0;
  if (sc.strongIntraSmoothing && cIdx == 0 && nT == 32) {
    const int threshold = 1 << (sc.bitDepth - 5);
    const int d1 = std::abs((int)p[0] + (int)p[64] - 2 * (int)p[32]);
    const int d2 = std::abs((int)p[0] + (int)p[-64] - 2 * (int)p[-32]);
    if (d1 < threshold && d2 < threshold) biIntFlag = 1;
  }

  pixel_t pF_mem[4 * 64 + 1];
  pixel_t* pF = &pF_mem[2 * 64];

  if (biIntFlag) {
    pF[-2 * nT] = p[-2 * nT];
    pF[ 2 * nT] = p[ 2 * nT];
    pF[      0] = p[      0];

    for (int i = 1; i <= 63; i++) {
      pF[-i] = (pixel_t)((int)p[0] + (((int)i * ((int)p[-64] - (int)p[0]) + 32) >> 6));
      pF[ i] = (pixel_t)((int)p[0] + (((int)i * ((int)p[ 64] - (int)p[0]) + 32) >> 6));
    }
  }
  else {
    pF[-2 * nT] = p[-2 * nT];
    pF[ 2 * nT] = p[ 2 * nT];

    for (int i = -(2 * nT - 1); i <= 2 * nT - 1; i++) {
      pF[i] = (pixel_t)(((int)p[i + 1] + 2 * (int)p[i] + (int)p[i - 1] + 2) >> 2);
    }
  }

  for (int i = -2 * nT; i <= 2 * nT; i++) p[i] = pF[i];
}

template <class pixel_t>
void ref_predict_planar(pixel_t* dst, int dstStride, int nT, const pixel_t* border)
{
  const int Log2_nT = ref_log2(nT);

  for (int y = 0; y < nT; y++)
    for (int x = 0; x < nT; x++) {
      const int v = ((nT - 1 - x) * (int)border[-1 - y] + (x + 1) * (int)border[1 + nT] +
                     (nT - 1 - y) * (int)border[1 + x] + (y + 1) * (int)border[-1 - nT] + nT)
                    >> (Log2_nT + 1);
      dst[x + y * dstStride] = (pixel_t)v;
    }
}

template <class pixel_t>
void ref_predict_dc(pixel_t* dst, int dstStride, int nT, int cIdx, const pixel_t* border)
{
  const int Log2_nT = ref_log2(nT);

  int dcVal = 0;
  for (int i = 0; i < nT; i++) {
    dcVal += (int)border[i + 1];
    dcVal += (int)border[-i - 1];
  }
  dcVal = (dcVal + nT) >> (Log2_nT + 1);

  if (cIdx == 0 && nT < 32) {
    dst[0] = (pixel_t)(((int)border[-1] + 2 * dcVal + (int)border[1] + 2) >> 2);
    for (int x = 1; x < nT; x++)
      dst[x] = (pixel_t)(((int)border[x + 1] + 3 * dcVal + 2) >> 2);
    for (int y = 1; y < nT; y++)
      dst[y * dstStride] = (pixel_t)(((int)border[-y - 1] + 3 * dcVal + 2) >> 2);
    for (int y = 1; y < nT; y++)
      for (int x = 1; x < nT; x++)
        dst[x + y * dstStride] = (pixel_t)dcVal;
  }
  else {
    for (int y = 0; y < nT; y++)
      for (int x = 0; x < nT; x++)
        dst[x + y * dstStride] = (pixel_t)dcVal;
  }
}

template <class pixel_t>
void ref_predict_angular(pixel_t* dst, int dstStride, int bitDepth,
                         bool disableIntraBoundaryFilter,
                         int mode, int nT, int cIdx, const pixel_t* border)
{
  // Table 8-4 / 8-5 of the specification.
  static const int angleTab[35] = {
    0, 0, 32, 26, 21, 17, 13, 9, 5, 2, 0, -2, -5, -9, -13, -17, -21, -26,
    -32, -26, -21, -17, -13, -9, -5, -2, 0, 2, 5, 9, 13, 17, 21, 26, 32
  };
  static const int invAngleTab[15] = {
    -4096, -1638, -910, -630, -482, -390, -315, -256,
    -315, -390, -482, -630, -910, -1638, -4096
  };

  const int intraPredAngle = angleTab[mode];

  pixel_t ref_mem[4 * 64 + 1];
  pixel_t* ref = &ref_mem[2 * 64];

  auto clip = [bitDepth](int v) {
    const int maxv = (1 << bitDepth) - 1;
    return (pixel_t)(v < 0 ? 0 : (v > maxv ? maxv : v));
  };

  if (mode >= 18) {
    for (int x = 0; x <= nT; x++) ref[x] = border[x];

    if (intraPredAngle < 0) {
      const int invAngle = invAngleTab[mode - 11];
      if ((nT * intraPredAngle) >> 5 < -1) {
        for (int x = (nT * intraPredAngle) >> 5; x <= -1; x++)
          ref[x] = border[0 - ((x * invAngle + 128) >> 8)];
      }
    }
    else {
      for (int x = nT + 1; x <= 2 * nT; x++) ref[x] = border[x];
    }

    for (int y = 0; y < nT; y++)
      for (int x = 0; x < nT; x++) {
        const int iIdx  = ((y + 1) * intraPredAngle) >> 5;
        const int iFact = ((y + 1) * intraPredAngle) & 31;
        if (iFact != 0)
          dst[x + y * dstStride] = (pixel_t)(((32 - iFact) * (int)ref[x + iIdx + 1] +
                                             iFact * (int)ref[x + iIdx + 2] + 16) >> 5);
        else
          dst[x + y * dstStride] = ref[x + iIdx + 1];
      }

    if (mode == REF_INTRA_ANGULAR_26 && cIdx == 0 && nT < 32 && !disableIntraBoundaryFilter) {
      for (int y = 0; y < nT; y++)
        dst[0 + y * dstStride] = clip((int)border[1] + (((int)border[-1 - y] - (int)border[0]) >> 1));
    }
  }
  else {
    for (int x = 0; x <= nT; x++) ref[x] = border[-x];

    if (intraPredAngle < 0) {
      const int invAngle = invAngleTab[mode - 11];
      if ((nT * intraPredAngle) >> 5 < -1) {
        for (int x = (nT * intraPredAngle) >> 5; x <= -1; x++)
          ref[x] = border[((x * invAngle + 128) >> 8)];
      }
    }
    else {
      for (int x = nT + 1; x <= 2 * nT; x++) ref[x] = border[-x];
    }

    for (int y = 0; y < nT; y++)
      for (int x = 0; x < nT; x++) {
        const int iIdx  = ((x + 1) * intraPredAngle) >> 5;
        const int iFact = ((x + 1) * intraPredAngle) & 31;
        if (iFact != 0)
          dst[x + y * dstStride] = (pixel_t)(((32 - iFact) * (int)ref[y + iIdx + 1] +
                                             iFact * (int)ref[y + iIdx + 2] + 16) >> 5);
        else
          dst[x + y * dstStride] = ref[y + iIdx + 1];
      }

    if (mode == REF_INTRA_ANGULAR_10 && cIdx == 0 && nT < 32 && !disableIntraBoundaryFilter) {
      for (int x = 0; x < nT; x++)
        dst[x] = clip((int)border[-1] + (((int)border[1 + x] - (int)border[0]) >> 1));
    }
  }
}

// Predict a complete nT*nT output block from the reference samples in border.
template <class pixel_t>
void ref_predict(const ref_scene& sc, pixel_t* dst, int dstStride,
                 int mode, int nT, int cIdx, bool disableIntraBoundaryFilter,
                 const pixel_t* border)
{
  if (mode == REF_INTRA_PLANAR)
    ref_predict_planar(dst, dstStride, nT, border);
  else if (mode == REF_INTRA_DC)
    ref_predict_dc(dst, dstStride, nT, cIdx, border);
  else
    ref_predict_angular(dst, dstStride, sc.bitDepth, disableIntraBoundaryFilter,
                        mode, nT, cIdx, border);
}

#endif
