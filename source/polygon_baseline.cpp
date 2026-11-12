#include "tinyply.h"
#include <cassert>
#include <iostream>
#include <sstream>
#include <vector>
#include <cstring>
int main() {
  std::string text = "ply\nformat ascii 1.0\nelement face 2\nproperty list uchar uint vertex_indices\nproperty list uchar float texcoord\nend_header\n3 0 1 2 6 0 0 1 0 0 1\n4 0 2 3 4 8 0 0 0 1 1 1 1 0\n";
  std::istringstream input(text);
  tinyply::PlyFile f;
  assert(f.parse_header(input));
  auto indices = f.request_properties_from_element("face", {"vertex_indices"});
  auto uv = f.request_properties_from_element("face", {"texcoord"});
  f.read(input);
  assert((indices->list_sizes == std::vector<size_t>{3,4}));
  assert((uv->list_sizes == std::vector<size_t>{6,8}));
  assert(indices->buffer.size_bytes() == 7 * sizeof(uint32_t));
  assert(uv->buffer.size_bytes() == 14 * sizeof(float));
  for (bool binary : {false,true}) {
    std::ostringstream encoded(std::ios::out | std::ios::binary);
    f.write(encoded,binary);
    std::istringstream stream(encoded.str(),std::ios::in | std::ios::binary);
    tinyply::PlyFile g; assert(g.parse_header(stream));
    auto a = g.request_properties_from_element("face", {"vertex_indices"});
    auto b = g.request_properties_from_element("face", {"texcoord"});
    g.read(stream);
    assert(a->list_sizes == indices->list_sizes && b->list_sizes == uv->list_sizes);
    assert(a->buffer.size_bytes() == indices->buffer.size_bytes());
    assert(std::memcmp(a->buffer.get(),indices->buffer.get(),a->buffer.size_bytes()) == 0);
    assert(b->buffer.size_bytes() == uv->buffer.size_bytes());
    assert(std::memcmp(b->buffer.get(),uv->buffer.get(),b->buffer.size_bytes()) == 0);
  }
  std::cout << "mixed polygons parsed/transcoded PASS indices=3,4 uv=6,8\n";
}
