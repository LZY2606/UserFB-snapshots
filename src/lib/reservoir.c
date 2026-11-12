/* reservoir.c
 * Layer3 bit reservoir: Described in C.1.5.4.2.2 of the IS
 *
 * When the encoder is initialised through shine_initialise_reservoir()
 * with a positive max_bytes, ResvMax holds the reservoir capacity in
 * bits and the functions at the bottom of this file implement the
 * actual cross-frame main-data placement: each coded frame's main data
 * is appended to a pending byte stream and physical frames (header,
 * side information and the main-data bytes they point to) are only
 * assembled once every byte they must carry is available.
 */

#include "reservoir.h"
#include "bitstream.h"
#include "huffman.h"
#include "l3bitstream.h"
#include "l3loop.h"
#include "layer3.h"
#include "types.h"

/*
 * shine_max_reservoir_bits:
 * ------------
 * Called at the beginning of each granule to get the max bit
 * allowance for the current granule based on reservoir size
 * and perceptual entropy.
 */
int shine_max_reservoir_bits(double *pe, shine_global_config *config) {
  int max_bits;
  int mean_bits = config->mean_bits;

  (void)pe; /* No psychoacoustic model: pe is always zero. */

  mean_bits /= config->wave.channels;
  max_bits = mean_bits;

  if (max_bits > 4095)
    max_bits = 4095;
  if (!config->ResvMax)
    return max_bits;

  /* Reservoir mode: on top of the mean allowance, offer an even share
   * of the bits still stored to each remaining granule/channel of this
   * frame. The offer never exceeds the remaining reservoir, so ResvSize
   * cannot go negative, and the 12 bit part2_3_length limit is enforced
   * on every granule. */
  if (config->resv_slots_left > 0)
    max_bits += config->ResvSize / config->resv_slots_left;
  if (max_bits > 4095)
    max_bits = 4095;
  return max_bits;
}

/*
 * shine_ResvAdjust:
 * -----------
 * Called after a granule's bit allocation. Readjusts the size of
 * the reservoir to reflect the granule's usage.
 */
void shine_ResvAdjust(gr_info *gi, shine_global_config *config) {
  config->ResvSize +=
      (config->mean_bits / config->wave.channels) - gi->part2_3_length;
}

/*
 * shine_ResvFrameEnd:
 * -------------
 * Called after all granules in a frame have been allocated. Makes sure
 * that the reservoir size is within limits, possibly by adding stuffing
 * bits. Note that stuffing bits are added by increasing a granule's
 * part2_3_length. The bitstream formatter will detect this and write the
 * appropriate stuffing bits to the bitstream.
 */
void shine_ResvFrameEnd(shine_global_config *config) {
  gr_info *gi;
  int gr, ch, ancillary_pad, stuffingBits;
  int over_bits;
  shine_side_info_t *l3_side = &config->side_info;

  ancillary_pad = 0;

  /* just in case mean_bits is odd, this is necessary... */
  if ((config->wave.channels == 2) && (config->mean_bits & 1))
    config->ResvSize += 1;

  over_bits = config->ResvSize - config->ResvMax;
  if (over_bits < 0)
    over_bits = 0;

  config->ResvSize -= over_bits;
  stuffingBits = over_bits + ancillary_pad;

  /* we must be byte aligned */
  if ((over_bits = config->ResvSize % 8)) {
    stuffingBits += over_bits;
    config->ResvSize -= over_bits;
  }

  if (stuffingBits) {
    /*
     * plan a: put all into the first granule
     * This was preferred by someone designing a
     * real-time decoder...
     */
    gi = (gr_info *)&(l3_side->gr[0].ch[0]);

    if (gi->part2_3_length + stuffingBits < 4095)
      gi->part2_3_length += stuffingBits;
    else {
      /* plan b: distribute throughout the granules */
      for (gr = 0; gr < config->mpeg.granules_per_frame; gr++)
        for (ch = 0; ch < config->wave.channels; ch++) {
          int extraBits, bitsThisGr;
          gr_info *gi = (gr_info *)&(l3_side->gr[gr].ch[ch]);
          if (!stuffingBits)
            break;
          extraBits = 4095 - gi->part2_3_length;
          bitsThisGr = extraBits < stuffingBits ? extraBits : stuffingBits;
          gi->part2_3_length += bitsThisGr;
          stuffingBits -= bitsThisGr;
        }
      /*
       * If any stuffing bits remain, they become ancillary bits at the
       * tail of this frame's main data. The bitstream formatter writes
       * them out when l3side->resvDrain is set, so the physical frame
       * always keeps its exact size.
       */
      l3_side->resvDrain = stuffingBits;
    }
  }
}

/*
 * Cross-frame main-data placement.
 *
 * All physical frames of a stream keep their CBR size. If their
 * main-data areas (everything after header and side information) are
 * concatenated into a single byte stream, frame i's scalefactor and
 * Huffman data occupies the bytes [C_i - B_i, C_i - B_i + L_i/8), where
 * C_i is the cumulated main-data capacity before frame i, B_i the
 * main_data_begin value written in its side information and L_i its
 * total part2_3_length (plus any ancillary drain bits). The reservoir
 * accounting above guarantees 0 <= B_i <= ResvMax/8 and that consecutive
 * intervals never overlap nor point before the stream head.
 */

int shine_resv_init(shine_global_config *config, int max_bytes) {
  int cap_max, cap_min, qsize, frame_max, need;

  config->ResvMax = 8 * max_bytes;
  config->ResvSize = 0;

  /* Main-data capacity of the largest and smallest physical frame.
   * Frames only differ by the single padding byte. */
  cap_max = config->mpeg.whole_slots_per_frame + 1 - config->sideinfo_len / 8;
  cap_min = cap_max - 1;
  if (cap_min < 1)
    cap_min = 1;

  /* A frame is emitted as soon as every byte of its main-data area has
   * been produced, so the cumulated capacity of the queued frames stays
   * below max_bytes plus one frame's capacity. */
  qsize = (max_bytes + cap_max) / cap_min + 3;
  frame_max = config->sideinfo_len / 8 + cap_max;

  config->resv_qsize = qsize;
  config->resv_qhead = 0;
  config->resv_qcount = 0;
  config->resv_queue =
      (shine_side_info_t *)calloc(qsize, sizeof(shine_side_info_t));
  config->resv_qbegin = (int *)calloc(qsize, sizeof(int));
  config->resv_qcap = (int *)calloc(qsize, sizeof(int));
  config->resv_qpad = (int *)calloc(qsize, sizeof(int));
  config->resv_qcum = (long long *)calloc(qsize, sizeof(long long));

  config->resv_pending_size = max_bytes + 2 * cap_max + 16;
  config->resv_pending =
      (unsigned char *)malloc((size_t)config->resv_pending_size);
  config->resv_pending_base = 0;
  config->resv_stream_filled = 0;
  config->resv_stream_capacity = 0;
  config->resv_out = 0;

  shine_open_bit_stream(&config->mbs, cap_max + max_bytes + 16);
  shine_open_bit_stream(&config->hbs, 64);

  if (!config->resv_queue || !config->resv_qbegin || !config->resv_qcap ||
      !config->resv_qpad || !config->resv_qcum || !config->resv_pending ||
      !config->mbs.data || !config->hbs.data)
    return -1;

  /* The output buffer must hold every frame a single call can emit. */
  need = qsize * frame_max + 16;
  if (config->bs.data_size < need) {
    unsigned char *data =
        (unsigned char *)realloc(config->bs.data, (size_t)need);
    if (!data)
      return -1;
    config->bs.data = data;
    config->bs.data_size = need;
  }

  return 0;
}

void shine_resv_close(shine_global_config *config) {
  if (!config->ResvMax)
    return;
  free(config->resv_queue);
  free(config->resv_qbegin);
  free(config->resv_qcap);
  free(config->resv_qpad);
  free(config->resv_qcum);
  free(config->resv_pending);
  shine_close_bit_stream(&config->mbs);
  shine_close_bit_stream(&config->hbs);
}

static void shine_resv_emit(shine_global_config *config);

void shine_resv_store_frame(shine_global_config *config, int cap) {  int idx, n = config->mbs.data_position;
  long long off;

  /* Append this frame's main data to the pending byte stream. */
  off = config->resv_stream_filled - config->resv_pending_base;
  if (off + n > config->resv_pending_size)
    return; /* Cannot happen: the stream is bounded by max_bytes. */
  memcpy(config->resv_pending + off, config->mbs.data, (size_t)n);
  config->resv_stream_filled += n;

  /* Queue the frame's side information for later assembly. */
  if (config->resv_qcount >= config->resv_qsize)
    return; /* Cannot happen: the queue is bounded by max_bytes. */
  idx = (config->resv_qhead + config->resv_qcount) % config->resv_qsize;
  config->resv_queue[idx] = config->side_info;
  config->resv_qbegin[idx] = config->resv_frame_begin;
  config->resv_qcap[idx] = cap;
  config->resv_qpad[idx] = config->mpeg.padding;
  config->resv_qcum[idx] = config->resv_stream_capacity;
  config->resv_qcount++;
  config->resv_stream_capacity += cap;

  shine_resv_emit(config);
}

static void shine_resv_emit(shine_global_config *config) {
  long long base = config->resv_pending_base;

  while (config->resv_qcount) {
    int idx = config->resv_qhead;
    long long cum = config->resv_qcum[idx];
    int cap = config->resv_qcap[idx];
    int saved_padding;

    if (cum + cap > config->resv_stream_filled)
      break; /* This frame still points at bytes not yet produced. */

    config->hbs.data_position = 0;
    config->hbs.cache = 0;
    config->hbs.cache_bits = 32;
    saved_padding = config->mpeg.padding;
    config->mpeg.padding = config->resv_qpad[idx];
    shine_encode_side_info(config, &config->resv_queue[idx],
                           (unsigned)config->resv_qbegin[idx], &config->hbs);
    config->mpeg.padding = saved_padding;
    shine_bitstream_byte_flush(&config->hbs);

    memcpy(config->bs.data + config->resv_out, config->hbs.data,
           (size_t)config->hbs.data_position);
    config->resv_out += config->hbs.data_position;
    memcpy(config->bs.data + config->resv_out,
           config->resv_pending + (cum - base), (size_t)cap);
    config->resv_out += cap;

    config->resv_pending_base = cum + cap;
    config->resv_qhead = (config->resv_qhead + 1) % config->resv_qsize;
    config->resv_qcount--;
  }

  /* Compact the pending stream: drop the bytes just emitted. */
  if (config->resv_pending_base != base) {
    long long keep = config->resv_stream_filled - config->resv_pending_base;
    memmove(config->resv_pending,
            config->resv_pending + (config->resv_pending_base - base),
            (size_t)keep);
  }
}

void shine_resv_flush_frames(shine_global_config *config) {
  /* The tail of the main-data stream belongs to no logical frame: pad
   * it with zero bytes so every queued physical frame can be delivered
   * complete. No silent audio frame is added. */
  while (config->resv_stream_filled < config->resv_stream_capacity) {
    long long off = config->resv_stream_filled - config->resv_pending_base;
    if (off >= config->resv_pending_size)
      break; /* Cannot happen: bounded by max_bytes. */
    config->resv_pending[off] = 0;
    config->resv_stream_filled++;
  }
  shine_resv_emit(config);
}
