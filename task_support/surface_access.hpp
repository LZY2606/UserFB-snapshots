#pragma once
// Access to the unmodified sequential production translation unit, for small headless clients.
#define main smallpt_original_main
#include "../cpp-smallpt/cpp-smallpt/src/cpp-smallpt.cpp"
#undef main
