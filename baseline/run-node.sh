#!/bin/sh
set -eu
node --disable-warning=ExperimentalWarning --import ./task_support/loader.mjs "$1"
