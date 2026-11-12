#pragma once
#include <cstddef>
#include <stdexcept>
class Checks {
    std::size_t checks_ = 0;
public:
    void require(bool condition, const char* message = "numeric assertion failed") {
        ++checks_;
        if (!condition) throw std::runtime_error(message);
    }
    std::size_t count() const { return checks_; }
};
