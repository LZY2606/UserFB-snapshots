#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
task_build=$(mktemp -d "${TMPDIR:-/tmp}/elfio-group.XXXXXX")
trap 'rm -rf "$task_build"' EXIT
case "${1:-baseline}" in
 baseline)
  "${CXX:-c++}" -std=c++17 -I. task_support/baseline.cpp -o "$task_build/base"
  "$task_build/base" ;;
 target)
  test -s tests/group_view_contract.cpp
  test -s elfio/elfio_section_links.hpp
  test -s elfio/elfio_group_view.hpp
  "${CXX:-c++}" -std=c++17 -I. task_support/driver.cpp tests/group_view_contract.cpp -o "$task_build/target"
  "$task_build/target"
  bash task_support/run.sh baseline ;;
 *) exit 2 ;;
esac
