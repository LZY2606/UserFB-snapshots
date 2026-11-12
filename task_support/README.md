The support scripts use C++17 and the C++ standard library only. Run them from
any directory with bash. No package manager or network operation is performed.
`CXX` may name a compatible compiler; otherwise `c++` is used.

`baseline.sh` exercises the existing constant-width stroke path. Its three
checks cover an analytic butt-capped rectangle, a closed square with a hole,
and the existing conv_curve -> conv_stroke -> rasterizer chain including a
repeated point and repeated rewind. It does not implement pressure stroking.

Add the new tests at `task_tests/pressure.cpp`. Do not define main there.
Include "registry.hpp", and register cases with TASK_CASE("profile", name),
TASK_CASE("offset", name), TASK_CASE("joins", name), TASK_CASE("render", name).
The body receives `task::Case& c`; use `c.check(condition, explanation)` for
each content assertion. Each registered case must execute at least one check.
There is no skip facility. New production implementation may use
`agg-src/src/agg_pressure_stroke.cpp`, which target.sh compiles when present.

The fixed runner directly links this new file, runs every registered case in
all four required groups, and fails on missing groups or zero assertions.
The shell additionally requires group totals and final completion before
running the old checks. Early exit(0) cannot substitute for completing them.
Generate any demonstration PPM from the render tests into `task_output/`.
These wrappers validate execution, not the correctness of new assertions;
the geometric assertions and the pressure stroke implementation remain to do.
