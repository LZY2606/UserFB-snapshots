#!/bin/sh
set -eu
test -s Source/C++/Test/Tracks/Tx3gTest.cpp || { echo 'missing Tx3gTest.cpp'; exit 2; }
bash support/build.sh
c++ -std=c++11 -Isupport -ISource/C++/Core -ISource/C++/Codecs -ISource/C++/Crypto -ISource/C++/MetaData support/test_main.cpp Source/C++/Test/Tracks/Tx3gTest.cpp .build/libap4.a -o .build/tx3g
.build/tx3g
