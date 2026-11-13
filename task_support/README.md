The task support is authored C99 code, distributed under the root MIT license.
It adds input construction, an existing-API smoke check, and a test execution wrapper.
It contains no deformation, spatial acceleration, animation sampler, or ray intersection implementation.

`bash task_support/run.sh baseline` creates a small glTF plus a 224-byte local buffer in a temporary directory, loads it, and runs six selected upstream programs.
No download or package manager is used. The original `test/test_all.py` is not invoked; it retrieves external sample models and is outside this selected local test set.

`bash task_support/run.sh target` also compiles `examples/cgltf_skin_bvh.c`, `test/skin_bvh_test.c`, and the fixed `task_support/driver.c` with the loader implementation in a separate translation unit. The test file must not define `main` or `CGLTF_IMPLEMENTATION`. It exports `const TargetCase target_cases[]` and `const size_t target_count` declared by `cases.h`. Each case has a group, a unique name and a void function; use `TARGET_CHECK` for content assertions. At least six cases must run across deform, bounds and query. The wrapper checks each completed group and final completion outside the test process. It then builds `examples/skin_bvh_example.c` as a separate program, runs it on the generated scene, and checks the `SKIN_EXAMPLE_HIT` line.

The fixture is authored from numeric arrays by `fixture.c`; no third-party model assets are needed. The buffer contains float positions with a 16-byte stride, U8 joints and normalized weights, U16 triangle indices, two translated inverse-bind matrices, and a sparse morph. The two skinned instances have different morph weights and large mesh-node translations; a third instance is static. The baseline checks only loading and raw source fields, not the requested world deformation or picking.
