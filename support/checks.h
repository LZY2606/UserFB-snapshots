#pragma once
#include <stdexcept>
#include <iostream>
struct Checks { int count=0; void require(bool x) { ++count; if(!x) throw std::runtime_error("numeric assertion"); } };
