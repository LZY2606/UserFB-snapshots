#!/bin/bash
set -euo pipefail
c++ -std=c++11 -O1 -DNO_OMP -Isrc -Isupport "$@"
