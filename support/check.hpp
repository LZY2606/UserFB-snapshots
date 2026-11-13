#ifndef CHECK_ROW_CHECK_HPP
#define CHECK_ROW_CHECK_HPP
#define LODEPNG_NO_COMPILE_CPP
extern "C" {
#include "lodepng.h"
size_t unit_alloc_current(void);
size_t unit_alloc_peak(void);
void unit_alloc_reset_peak(void);
}
#include <vector>
#include <string>
#include <stdexcept>
struct UFCase { const char* name; const char* group; void (*run)(); };
inline size_t& unit_checks() { static size_t value = 0; return value; }
#define UNIT_CHECK(x) do { ++unit_checks(); if(!(x)) throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " #x); } while(0)
void register_tests(std::vector<UFCase>& cases);
#endif
