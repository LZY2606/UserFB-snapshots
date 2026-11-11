/*
 * Geometry / availability part of the independent reference model.
 * See intra_border_refmodel.h for the purpose of this module.
 */

#include "intra_border_refmodel.h"

int ref_tile_id(const ref_scene& sc, int ctbX, int ctbY)
{
  int tx = 0, ty = 0;
  for (size_t i = 0; i + 1 < sc.colBd.size(); i++)
    if (ctbX >= sc.colBd[i]) tx = (int)i;
  for (size_t i = 0; i + 1 < sc.rowBd.size(); i++)
    if (ctbY >= sc.rowBd[i]) ty = (int)i;

  const int numTileCols = (int)sc.colBd.size() - 1;
  return ty * numTileCols + tx;
}

int ref_ctb_addr_ts(const ref_scene& sc, int ctbX, int ctbY)
{
  // 6.5.1: CTBs are ordered tile by tile (tiles themselves in raster order),
  // and raster order within a tile.
  const int numTileCols = (int)sc.colBd.size() - 1;
  const int numTileRows = (int)sc.rowBd.size() - 1;

  int addr = 0;
  for (int ty = 0; ty < numTileRows; ty++)
    for (int tx = 0; tx < numTileCols; tx++) {
      const int tileW = sc.colBd[tx + 1] - sc.colBd[tx];
      const int tileH = sc.rowBd[ty + 1] - sc.rowBd[ty];
      if (ctbX >= sc.colBd[tx] && ctbX < sc.colBd[tx + 1] &&
          ctbY >= sc.rowBd[ty] && ctbY < sc.rowBd[ty + 1]) {
        return addr + (ctbY - sc.rowBd[ty]) * tileW + (ctbX - sc.colBd[tx]);
      }
      addr += tileW * tileH;
    }

  return -1;
}

int ref_min_tb_addr_zs(const ref_scene& sc, int tbX, int tbY)
{
  // 6.5.2: tile-scan CTB address followed by a Z-scan inside the CTB.
  const int shift = sc.log2Ctb - sc.log2MinTb;
  const int ctbX = tbX >> shift;
  const int ctbY = tbY >> shift;

  const int ctbAddrTS = ref_ctb_addr_ts(sc, ctbX, ctbY);
  if (ctbAddrTS < 0) return -1;

  int addr = ctbAddrTS << (2 * shift);

  const int x = tbX - (ctbX << shift);
  const int y = tbY - (ctbY << shift);
  int p = 0;
  for (int i = 0; i < shift; i++) {
    const int m = 1 << i;
    p += (m & x ? m * m : 0) + (m & y ? 2 * m * m : 0);
  }

  return addr + p;
}

bool ref_neighbour_available(const ref_scene& sc, int xB, int yB, int xN, int yN)
{
  // Outside the coded picture.
  if (xN < 0 || yN < 0 || xN >= sc.picW || yN >= sc.picH) return false;

  const int ctbX = xN >> sc.log2Ctb;
  const int ctbY = yN >> sc.log2Ctb;
  const int curCtbX = xB >> sc.log2Ctb;
  const int curCtbY = yB >> sc.log2Ctb;

  // Different slice segment.
  const int idxN = ctbX + ctbY * sc.picWidthInCtbs();
  const int idxC = curCtbX + curCtbY * sc.picWidthInCtbs();
  if (idxN < 0 || idxN >= (int)sc.sliceOfCtb.size()) return false;
  if (sc.sliceOfCtb[idxN] != sc.sliceOfCtb[idxC]) return false;

  // Different tile.
  if (ref_tile_id(sc, ctbX, ctbY) != ref_tile_id(sc, curCtbX, curCtbY)) return false;

  // Minimum transform block not decoded yet (Z-scan order).
  const int tbN = ref_min_tb_addr_zs(sc, xN >> sc.log2MinTb, yN >> sc.log2MinTb);
  const int tbC = ref_min_tb_addr_zs(sc, xB >> sc.log2MinTb, yB >> sc.log2MinTb);
  if (tbN < 0 || tbC < 0 || tbN > tbC) return false;

  // Constrained intra prediction: no samples from inter coded blocks.
  if (sc.constrainedIntra) {
    const int cbX = xN >> sc.log2MinCb;
    const int cbY = yN >> sc.log2MinCb;
    const int cbIdx = cbX + cbY * sc.picWidthInCbs();
    if (cbIdx < 0 || cbIdx >= (int)sc.interCb.size()) return false;
    if (sc.interCb[cbIdx]) return false;
  }

  return true;
}
