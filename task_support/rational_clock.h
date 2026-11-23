// C++17 bounded, absolute rational clock for the public forward reference.
#pragma once
#include <cstdint>
namespace task_reference {
struct Point { std::uint64_t integer; double fraction; };
// S,D are the integer rates of this interpolation stage, not the outer rates.
// Init is exactly the stage's constructor-computed InitFracPos (binary64).
// Valid here: j <= 2^48, S,D <= 768000, 0 <= Init < 1.
inline Point phase_at(std::uint64_t j, std::uint64_t S,
                      std::uint64_t D, double Init) {
    const auto a = (j / D) * S;
    const auto b = (j % D) * S;
    const auto q = a + b / D;
    const auto r = b % D;
    const double t = Init + static_cast<double>(r) / static_cast<double>(D);
    const bool carry = t >= 1.0;
    return {q + static_cast<unsigned>(carry), t - static_cast<unsigned>(carry)};
}
inline std::uint64_t output_frames(std::uint64_t N,
                                  std::uint64_t S, std::uint64_t D) {
    return (N / S) * D + ((N % S) * D + S - 1) / S;
}
}
