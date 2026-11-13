/*
 * H.265 video codec.
 * Copyright (c) 2013-2014 struktur AG, Dirk Farin <farin@struktur.de>
 *
 * This file is part of libde265.
 *
 * libde265 is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * libde265 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with libde265.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef DE265_DEBLOCK_H
#define DE265_DEBLOCK_H

#include "libde265/decctx.h"

void add_deblocking_tasks(image_unit* imgunit);
void apply_deblocking_filter(de265_image* img); //decoder_context* ctx);

// Row-oriented entry points shared by the in-place filter pipeline.
bool derive_edgeFlags_CTBRow(de265_image* img, uint16_t ctby);
void derive_boundaryStrength(de265_image* img, bool vertical,
                             int yStart,int yEnd, int xStart,int xEnd);
void edge_filtering_luma(de265_image* img, bool vertical,
                         int yStart,int yEnd, int xStart,int xEnd);
void edge_filtering_chroma(de265_image* img, bool vertical,
                           int yStart,int yEnd, int xStart,int xEnd);

#endif
