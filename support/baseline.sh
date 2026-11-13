#!/bin/sh
set -eu
bash support/build.sh
c++ -std=c++11 -ISource/C++/Core -ISource/C++/Codecs -ISource/C++/Crypto -ISource/C++/MetaData support/baseline.cpp .build/libap4.a -o .build/baseline
.build/baseline
