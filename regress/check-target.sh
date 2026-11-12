#!/bin/sh
set -eu
sh regress/run-contract.sh
test -s regress/verify-rdfa.cjs
node regress/verify-rdfa.cjs
