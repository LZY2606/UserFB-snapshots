#include <FastBVH.h>

#include <cstdio>
#include <vector>

//! \brief A small, window-less example of four-ray packet traversal.
//!
//! The example builds a tiny scene of coloured boxes, then renders a small
//! image by packing four horizontally adjacent pixels into a single
//! @ref FastBVH::PacketTraverser query. Every pixel colour comes straight from
//! one of the four lanes' intersection results, which makes the packet output
//! easy to inspect: the image is just a PPM file, no window or graphics
//! library involved.

namespace {

using namespace FastBVH;

using Float = double;

//! A coloured axis-aligned box primitive.
struct Box final {
  Vector3<Float> min;
  Vector3<Float> max;
  Vector3<Float> colour;
};

//! Converts a box primitive to its bounding box.
struct BoxConverter final {
  BBox<Float> operator()(const Box& b) const noexcept { return BBox<Float>(b.min, b.max); }
};

//! Returns exactly one candidate root per box, using the closed interval
//! slab test that also handles axis-parallel directions.
struct BoxIntersector final {
  Intersection<Float, Box> operator()(const Box& b, const Ray<Float>& ray) const noexcept {
    Float near = -std::numeric_limits<Float>::infinity();
    Float far = std::numeric_limits<Float>::infinity();

    for (unsigned int axis = 0; axis < 3; ++axis) {
      const Float origin = ray.o[axis];
      const Float direction = ray.d[axis];

      if (direction == Float(0)) {
        if (origin < b.min[axis] || origin > b.max[axis]) {
          return Intersection<Float, Box>{};
        }
        continue;
      }

      Float t0 = (b.min[axis] - origin) / direction;
      Float t1 = (b.max[axis] - origin) / direction;
      if (t0 > t1) {
        std::swap(t0, t1);
      }
      if (t0 > near) {
        near = t0;
      }
      if (t1 < far) {
        far = t1;
      }
      if (near > far) {
        return Intersection<Float, Box>{};
      }
    }

    return Intersection<Float, Box>{near, &b};
  }
};

constexpr std::size_t width = 128;
constexpr std::size_t height = 48;

//! Builds a small scene of coloured boxes spread out along the x/y plane.
std::vector<Box> makeScene() {
  std::vector<Box> boxes;

  for (int y = 0; y < 6; ++y) {
    for (int x = 0; x < 12; ++x) {
      const Float x0 = Float(x * 10 + 1);
      const Float y0 = Float(y * 10 + 1);

      Vector3<Float> colour{
          Float((x * 3 + 1) % 7) / Float(6),
          Float((y * 5 + 2) % 9) / Float(8),
          Float((x + y) % 5) / Float(4),
      };

      boxes.push_back(Box{{x0, y0, Float(20)}, {x0 + 4, y0 + 4, Float(22)}, colour});
    }
  }

  return boxes;
}

}  // namespace

int main() {
  std::vector<Box> boxes = makeScene();

  auto bvh = DefaultBuilder<Float>{}(boxes, BoxConverter{});

  printf("Scene: %u boxes, %u BVH nodes\n", (unsigned int)boxes.size(), (unsigned int)bvh.getNodes().size());

  PacketTraverser<Float, Box, BoxIntersector> packet(bvh, BoxIntersector{});

  std::vector<unsigned char> pixels(width * height * 3, 0);

  const std::array<Float, 4> tmin{{0, 0, 0, 0}};
  const std::array<Float, 4> tmax{{std::numeric_limits<Float>::infinity(), std::numeric_limits<Float>::infinity(),
                                  std::numeric_limits<Float>::infinity(), std::numeric_limits<Float>::infinity()}};

  uint64_t node_loads = 0;
  uint64_t box_tests = 0;
  uint64_t primitive_tests = 0;

  // Render one row of pixels at a time. Within each row, four neighbouring
  // pixels are traced together as a single packet sharing one stack.
  for (std::size_t y = 0; y < height; ++y) {
    for (std::size_t x = 0; x < width; x += 4) {
      std::array<Ray<Float>, 4> rays{{
          Ray<Float>(Vector3<Float>{Float(x + 0) + Float(0.5), Float(y) + Float(0.5), Float(0)}, Vector3<Float>{0, 0, 1}),
          Ray<Float>(Vector3<Float>{Float(x + 1) + Float(0.5), Float(y) + Float(0.5), Float(0)}, Vector3<Float>{0, 0, 1}),
          Ray<Float>(Vector3<Float>{Float(x + 2) + Float(0.5), Float(y) + Float(0.5), Float(0)}, Vector3<Float>{0, 0, 1}),
          Ray<Float>(Vector3<Float>{Float(x + 3) + Float(0.5), Float(y) + Float(0.5), Float(0)}, Vector3<Float>{0, 0, 1}),
      }};

      PacketStats stats;
      auto hits = packet.traverse(rays, tmin, tmax, 0x0F, &stats);

      node_loads += stats.node_loads;
      box_tests += stats.box_lane_tests;
      primitive_tests += stats.primitive_lane_tests;

      for (int lane = 0; lane < 4; ++lane) {
        const auto& hit = hits[lane];
        unsigned char* pixel = &pixels[(y * width + x + lane) * 3];

        if (hit && hit.object != nullptr) {
          pixel[0] = static_cast<unsigned char>(hit.object->colour.x * 255);
          pixel[1] = static_cast<unsigned char>(hit.object->colour.y * 255);
          pixel[2] = static_cast<unsigned char>(hit.object->colour.z * 255);
        } else {
          // Miss: a dark blue background.
          pixel[0] = 8;
          pixel[1] = 12;
          pixel[2] = 32;
        }
      }
    }
  }

  FILE* file = std::fopen("packet.ppm", "wb");
  if (file == nullptr) {
    std::fprintf(stderr, "Could not open packet.ppm for writing\n");
    return 1;
  }

  std::fprintf(file, "P6\n%u %u\n255\n", (unsigned int)width, (unsigned int)height);
  std::fwrite(pixels.data(), sizeof(unsigned char), pixels.size(), file);
  std::fclose(file);

  printf("Wrote packet.ppm\n");
  printf("Packet queries: %u\n", (unsigned int)((width / 4) * height));
  printf("node_loads=%llu box_lane_tests=%llu primitive_lane_tests=%llu\n", (unsigned long long)node_loads,
         (unsigned long long)box_tests, (unsigned long long)primitive_tests);

  return 0;
}
