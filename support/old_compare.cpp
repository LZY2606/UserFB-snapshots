/* Call selected real upstream tests, with a reliable nonzero failure exit. */
#define main upstream_unused_main
#define DISABLE_SLOW
#include "../lodepng_unittest.cpp"
#undef main
int main() {
  try {
    testPNGCodec();
    testPredefinedFilters();
    testCompressZlib();
    testHuffmanCodeLengths();
    testCustomZlibDecompress();
    testCustomInflate();
    std::cout << "LODEPNG_OLD_SELECTED_PASS groups=6" << std::endl;
  } catch(...) { std::cerr << "selected upstream test failed\n"; return 1; }
  return 0;
}
