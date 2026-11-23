#include "kvazaar.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 64
#define H 64
#define N 4

static void fill(kvz_picture *p, int frame) {
  for (int c = 0; c < 3; ++c) {
    int w = c ? W/2 : W, h = c ? H/2 : H, st = c ? p->stride_c : p->stride;
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
        p->data[c][y*st+x] = (unsigned char)((13*x + 7*y + 29*((x/8)^(y/8)) + c*47 + frame*9) & 255);
  }
}

int main(void) {
  const kvz_api *api = kvz_api_get(8);
  kvz_config *cfg = api->config_alloc();
  api->config_init(cfg);
  const char *opts[][2] = {{"gop","0"},{"ref","1"},{"bipred","0"},{"wpp","0"},
    {"cpuid","0"},{"deblock","0"},{"sao","off"},{"info","0"},{"force-inter","1"},
    {"weighted-pred","1"},{"hash","none"},{"me","hexbs"},{"subme","4"}};
  for (size_t i = 0; i < sizeof(opts)/sizeof(opts[0]); ++i)
    if (!api->config_parse(cfg, opts[i][0], opts[i][1])) { printf("parse fail %s\n", opts[i][0]); return 1; }
  cfg->width = W; cfg->height = H; cfg->owf = 0; cfg->threads = 1;
  cfg->enable_logging_output = 0;
  kvz_encoder *enc = api->encoder_open(cfg);
  if (!enc) { printf("open failed\n"); return 1; }
  api->config_destroy(cfg);

  for (int i = 0; i < N + 2; ++i) {
    kvz_picture *in = NULL, *rec = NULL, *src = NULL;
    kvz_data_chunk *ch = NULL; uint32_t bytes = 0; kvz_frame_info info;
    if (i < N) {
      in = api->picture_alloc(W, H);
      fill(in, i);
      in->pts = i;
      if (i > 0) {
        in->wp.enabled = 1;
        in->wp.denom = (i % 2) ? 1 : 0;
        in->wp.weight[0] = (1 << in->wp.denom) + (i - 2) * 4;
        in->wp.weight[1] = (1 << in->wp.denom) - 3;
        in->wp.weight[2] = (1 << in->wp.denom) + 3;
        in->wp.offset[0] = i * 5 - 5;
        in->wp.offset[1] = -2 + i;
        in->wp.offset[2] = 4 - i;
      }
    }
    int r = api->encoder_encode(enc, in, &ch, &bytes, &rec, &src, &info);
    if (!r) { printf("encode failed at %d\n", i); return 1; }
    if (in) api->picture_free(in);
    if (ch) {
      printf("frame out poc=%d slice=%d bytes=%u\n", info.poc, info.slice_type, bytes);
      api->chunk_free(ch);
      api->picture_free(rec);
      api->picture_free(src);
    }
  }
  api->encoder_close(enc);
  printf("SMOKE_OK\n");
  return 0;
}
