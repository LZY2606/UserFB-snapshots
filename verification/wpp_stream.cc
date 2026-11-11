/*
 * Full-stream regression witness for the intra prediction path.
 *
 * Decodes a real HEVC bitstream through the public API
 * (de265_push_data / de265_decode) with one and with four worker threads and
 * compares every decoded frame plane by plane against a reference raw YUV file
 * produced by FFmpeg.  The stream is required to actually use WPP
 * (entropy_coding_sync_enabled_flag set), otherwise the test fails instead of
 * silently testing nothing.
 *
 * Internal headers are used only to verify the WPP flag of the parsed PPS; the
 * expected pixel data always comes from FFmpeg, never from libde265 itself.
 *
 * usage: wpp_stream <stream.h265> <ffmpeg-raw-yuv420p>
 */

#include "libde265/de265.h"

// Only used to inspect the parsed PPS after decoding.
#include "libde265/decctx.h"
#include "libde265/pps.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

static bool stream_uses_wpp(de265_decoder_context* dc)
{
  decoder_context* ctx = reinterpret_cast<decoder_context*>(dc);
  for (int i = 0; i < DE265_MAX_PPS_SETS; i++) {
    if (ctx->has_pps(i) && ctx->get_pps(i)->entropy_coding_sync_enabled_flag)
      return true;
  }
  return false;
}

static int decode_and_compare(const std::vector<char>& stream,
                              const std::vector<char>& expected,
                              int num_threads)
{
  de265_decoder_context* ctx = de265_new_decoder();
  if (!ctx) {
    std::fprintf(stderr, "threads=%d: de265_new_decoder failed\n", num_threads);
    return 1;
  }

  if (de265_start_worker_threads(ctx, num_threads) != DE265_OK) {
    std::fprintf(stderr, "threads=%d: could not start worker threads\n", num_threads);
    de265_free_decoder(ctx);
    return 1;
  }

  de265_error err = de265_push_data(ctx, stream.data(), stream.size(), 0, nullptr);
  if (!de265_isOK(err)) {
    std::fprintf(stderr, "threads=%d: de265_push_data failed (%d)\n", num_threads, err);
    de265_free_decoder(ctx);
    return 1;
  }
  if (!de265_isOK(de265_flush_data(ctx))) {
    std::fprintf(stderr, "threads=%d: de265_flush_data failed\n", num_threads);
    de265_free_decoder(ctx);
    return 1;
  }

  size_t expected_pos = 0;
  int frame = 0;
  int failures = 0;
  int more = 1;

  for (int tries = 0; more && tries < 10000; tries++) {
    err = de265_decode(ctx, &more);
    if (!de265_isOK(err)) {
      std::fprintf(stderr, "threads=%d: de265_decode failed (%d)\n", num_threads, err);
      failures++;
      break;
    }

    while (const de265_image* img = de265_get_next_picture(ctx)) {
      for (int c = 0; c < 3; c++) {
        int stride = 0;
        const uint8_t* plane = de265_get_image_plane(img, c, &stride);
        const int w = de265_get_image_width(img, c);
        const int h = de265_get_image_height(img, c);

        for (int y = 0; y < h; y++) {
          if (expected_pos + static_cast<size_t>(w) > expected.size()) {
            std::fprintf(stderr, "threads=%d: frame %d plane %d: "
                                 "reference YUV is too short\n", num_threads, frame, c);
            failures++;
            break;
          }

          if (std::memcmp(plane + static_cast<size_t>(y) * stride,
                          &expected[expected_pos], static_cast<size_t>(w)) != 0) {
            int x = 0;
            while (x < w && plane[static_cast<size_t>(y) * stride + x] ==
                            static_cast<uint8_t>(expected[expected_pos + x]))
              x++;
            std::fprintf(stderr, "threads=%d: frame %d plane %d pixel (%d,%d): "
                                 "expected %u actual %u\n",
                         num_threads, frame, c, x, y,
                         static_cast<unsigned>(expected[expected_pos + x]),
                         static_cast<unsigned>(plane[static_cast<size_t>(y) * stride + x]));
            failures++;
            break;
          }

          expected_pos += static_cast<size_t>(w);
        }
      }
      frame++;
    }
  }

  if (expected_pos != expected.size()) {
    std::fprintf(stderr, "threads=%d: decoded %d frames (%zu bytes), reference has %zu bytes\n",
                 num_threads, frame, expected_pos, expected.size());
    failures++;
  }

  // The PPS is parsed during de265_decode(), so verify WPP only now.
  if (!stream_uses_wpp(ctx)) {
    std::fprintf(stderr, "threads=%d: stream does not use WPP "
                         "(entropy_coding_sync_enabled_flag is not set)\n", num_threads);
    failures++;
  }

  de265_free_decoder(ctx);

  if (failures == 0)
    std::printf("wpp-stream threads=%d: %d frames match FFmpeg plane by plane\n",
                num_threads, frame);

  return failures == 0 ? 0 : 1;
}

int main(int argc, char** argv)
{
  if (argc != 3) {
    std::fprintf(stderr, "usage: %s <stream.h265> <ffmpeg-raw-yuv420p>\n", argv[0]);
    return 2;
  }

  std::ifstream stream_in(argv[1], std::ios::binary);
  std::vector<char> stream((std::istreambuf_iterator<char>(stream_in)),
                           std::istreambuf_iterator<char>());
  if (stream.empty()) {
    std::fprintf(stderr, "could not read stream %s\n", argv[1]);
    return 2;
  }

  std::ifstream ref_in(argv[2], std::ios::binary);
  std::vector<char> expected((std::istreambuf_iterator<char>(ref_in)),
                             std::istreambuf_iterator<char>());
  if (expected.empty()) {
    std::fprintf(stderr, "could not read reference YUV %s\n", argv[2]);
    return 2;
  }

  int failures = 0;
  if (decode_and_compare(stream, expected, 1) != 0) failures++;
  if (decode_and_compare(stream, expected, 4) != 0) failures++;

  if (failures == 0)
    std::printf("WPP regression: single-thread and four-worker decodes match FFmpeg\n");

  return failures == 0 ? 0 : 1;
}
