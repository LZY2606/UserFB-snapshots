The starting witness calls the unchanged upstream curve geometry, BVH builder,
and line-based intersector with two sets of width controls. It is not a round
swept-sphere implementation. It also runs the upstream triangle regression in
both modes and checks its actual diagnostic output.

Add task_tests/target.cpp with no main. Include task_support/registry.hpp and
implement register_task_tests(Registry&). Register named cases using
r.add("envelope", "name", [](Case& c) { c.check(condition,"diagnostic"); });
The other mandatory groups are intervals, traversal and render. Every case must
execute a content assertion. The fixed main counts assertions and completion;
the process-external awk checks all four groups and final completion, even if a
case calls exit(0). There is no skip facility. Test failures return nonzero.

Run bash task_support/target.sh. It compiles the specified C++ test source,
runs every registered case, validates completion, then runs the starting/legacy
checks. It does not supply the future round-tube algorithm or its test answers.
