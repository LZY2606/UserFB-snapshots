#!/bin/sh
set -eu
for f in tests/ambi_contract.cpp tests/ambi_reference.cpp tests/ambi_reference.hpp tests/run_ambi_contract.sh docs/ambi-regression.md; do
 if [ ! -s "$f" ]; then echo "MISSING_TARGET_FILE $f" >&2; exit 1; fi
done
sh baseline/install.sh
cmake --build build --target ambi_contract -j 4
ALSOFT_CONF="$PWD/baseline/alsoft.conf" ctest --test-dir build --no-tests=error -R '^ambi_contract$' -V > build/ambi-contract.log 2>&1
cat build/ambi-contract.log
for group in normalization rotation live-update queue; do
 grep -E "PASS $group cases=[1-9][0-9]* max_error=" build/ambi-contract.log > /dev/null
done
sh tests/run_ambi_contract.sh > build/ambi-mutations.log 2>&1
cat build/ambi-mutations.log
for variant in scale listener hoa; do
 grep -x "DETECTED $variant" build/ambi-mutations.log > /dev/null
done
