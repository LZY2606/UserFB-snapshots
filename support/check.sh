#!/bin/sh
set -eu
test -s test/decode/time_sequence.cpp || { echo 'missing test/decode/time_sequence.cpp'; exit 2; }
bash support/build.sh
c++ -std=c++17 -Isupport -Iinclude -I.build/include support/test_main.cpp test/decode/time_sequence.cpp .build/libaribcaption.a -o .build/time-sequence
.build/time-sequence
