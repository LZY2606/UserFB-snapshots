//============================================================================
// EmberGL rolling-exposure motion example (CPU only, no window/GPU).
//
// Renders two large triangles whose depth order swaps during the exposure and
// whose screen positions change row by row, then writes the frame to a binary
// PPM (P6) at the path given by the first command line argument.
//============================================================================
#include "egl_device.h"
#include "egl_motion.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>

using namespace egl;

namespace
{

enum {FB_WIDTH=120, FB_HEIGHT=80};
typedef pixel<pixfmt_r8g8b8a8> pixel_t;

// self-contained rasterizer configuration (same knobs as the fixed-capacity
// rasterizer_memory used by the rest of the library)
struct motion_example_cfg: rasterizer_memory_cfg_base
{
  enum {tile_width=32, tile_height=32, max_dma_transfers=0};
  enum {depth_format=depthfmt_float32};
};
typedef graphics_device_mem<pixfmt_r8g8b8a8, FB_WIDTH, FB_HEIGHT> device_t;

EGL_INLINE motion_vertex mv(float x0, float y0, float z0, float w0,
                            float x1, float y1, float z1, float w1,
                            float u, float v, float r, float g, float b)
{
  return motion_vertex(vec4f(x0, y0, z0, w0), vec4f(x1, y1, z1, w1),
                       vec2f(u, v), color_rgbaf(r, g, b, 1.0f));
}

bool write_ppm(const char *path, const pixel_t *frame, unsigned width, unsigned height)
{
  FILE *fp=std::fopen(path, "wb");
  if(!fp)
    return false;
  std::fprintf(fp, "P6\n%u %u\n255\n", width, height);
  for(unsigned i=0; i<width*height; ++i)
  {
    unsigned char rgb[3]={frame[i].c.r, frame[i].c.g, frame[i].c.b};
    // transparent background becomes a dark gray so the rolling shapes read clearly
    if(frame[i].c.a==0) {rgb[0]=24; rgb[1]=24; rgb[2]=32;}
    std::fwrite(rgb, 1, 3, fp);
  }
  std::fclose(fp);
  return true;
}

} // namespace

int main(int argc, char **argv)
{
  if(argc<2)
  {
    std::fprintf(stderr, "usage: %s output.ppm\n", argv[0]);
    return 1;
  }

  static pixel_t frame[FB_WIDTH*FB_HEIGHT];
  device_t device(frame);
  rasterizer_memory<device_t, motion_example_cfg> memory;
  std::memset(frame, 0, sizeof(frame));
  memory.init(device);
  device.clear_depth(cleardepth_max);

  // Two faces that interleave through the exposure.
  //  * red face: starts near the camera on the left, recedes and drifts right
  //  * blue face: starts far on the right, approaches and drifts left
  // Their depth trajectories cross, so the winning color hands over from red
  // to blue between the top and bottom scanlines while the shapes deform.
  motion_vertex vertices[6];
  // red face (indices 0..2)
  vertices[0]=mv(-1.1f, -0.9f, 0.15f, 1.0f,   0.2f, -0.9f, 0.95f, 1.0f, 0,0, 1.0f,0.15f,0.15f);
  vertices[1]=mv( 0.3f, -0.9f, 0.15f, 1.0f,   1.1f, -0.9f, 0.95f, 1.0f, 1,0, 1.0f,0.35f,0.35f);
  vertices[2]=mv(-0.2f,  0.9f, 0.15f, 1.0f,   0.6f,  0.9f, 0.95f, 1.0f, 0,1, 1.0f,0.55f,0.55f);
  // blue face (indices 3..5), overlapping the red face and moving the other way
  vertices[3]=mv( 1.1f, -0.7f, 0.95f, 1.0f,  -0.2f, -0.7f, 0.15f, 1.0f, 0,0, 0.2f,0.35f,1.0f);
  vertices[4]=mv(-0.3f, -0.7f, 0.95f, 1.0f,  -1.1f, -0.7f, 0.15f, 1.0f, 1,0, 0.35f,0.55f,1.0f);
  vertices[5]=mv( 0.2f,  0.95f,0.95f, 1.0f,  -0.6f, 0.95f,0.15f, 1.0f, 0,1, 0.5f,0.7f,1.0f);

  static const uint8_t indices[6]={0,1,2, 3,4,5};
  motion_cluster_data data(vertices, 6, indices, 2);

  motion_stats stats;
  motion_cluster_psc<motion_default_psc_base, motion_vertex> pso(data, &stats);
  device.dispatch_pso(pso);
  device.commit();

  if(!write_ppm(argv[1], frame, FB_WIDTH, FB_HEIGHT))
  {
    std::fprintf(stderr, "failed to write %s\n", argv[1]);
    return 2;
  }

  std::printf("wrote %s (%dx%d) shaded=%zu transforms=%zu row_updates=%zu fans=%zu "
              "tight_bins=%zu fullscreen_bins=%zu\n",
              argv[1], FB_WIDTH, FB_HEIGHT, stats.num_shaded_pixels,
              stats.num_transforms, stats.num_row_updates, stats.num_fan_pieces,
              stats.num_bins_tight, stats.num_bins_fullscreen);
  return 0;
}
