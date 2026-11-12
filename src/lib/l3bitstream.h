#ifndef shine_BITSTREAM_H
#define shine_BITSTREAM_H

#include "types.h"

void shine_format_bitstream(shine_global_config *config);

/* Write one frame's header and side information (with the given
 * main_data_begin value) into the given bitstream. */
void shine_encode_side_info(shine_global_config *config, shine_side_info_t *si,
                            unsigned main_data_begin, bitstream_t *bs);

#endif
