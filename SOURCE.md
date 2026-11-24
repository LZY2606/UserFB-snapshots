This is a self-contained production-source export of ultravideo/uvg266,
commit d82f9cb22d0360f15f7771ebdd10a3336a16b62e (BSD-3-Clause).
All src/, cmake/, original license/credit files, documentation and upstream
tests are byte-for-byte copies of that commit. task_support/ is the existing
task build/registration wrapper, unchanged from support.patch.

The greatest gitlink (60e25ce7658ced49f03b52ac14c553cf6ea33de4) is an
optional upstream unit-test framework, not a production library dependency.
task_support/run.sh sets UVG_BUILD_TESTS=OFF and UVG_SYNC_SUBMODULES=OFF;
its target links the production static library and tests/forced_idr.c directly.
The optional greatest checkout, .gitmodules, hosted CI recipes, Dockerfile
and CI FFmpeg/VTM downloader are excluded. Original test sources and
LICENSE.EXT.greatest are retained; this export does not claim the greatest
suite or external decoder tests have run. Complete source hashes and the
exact exclusions are recorded in the accompanying seed-export.json.

GitHub identifies uvg266 and Kvazaar as separate non-fork repositories.
uvg266 describes itself as based on Kvazaar. For repository-use accounting
this task groups both source identities; changing the repository name is
not a claim of new source lineage or a reset of prior usage.
