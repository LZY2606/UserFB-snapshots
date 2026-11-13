#!/bin/sh
set -eu
mkdir -p .task-build
# The fixed driver, not a submitted shell script, owns registration and completion.
${CXX:-c++} -std=c++11 -O2 -I. -Ivendor/linear/include task_support/main.cpp task_tests/harmonizer.cpp -o .task-build/target
set +e
.task-build/target > .task-build/target.stdout 2> .task-build/target.stderr
status=$?
set -e
cat .task-build/target.stdout
cat .task-build/target.stderr >&2
[ "$status" -eq 0 ] || exit "$status"
awk '
BEGIN { want["equivalence"]=1; want["sharing"]=1; want["automation"]=1; want["stream"]=1 }
/^GROUP [a-z]+ cases=[1-9][0-9]* checks=[1-9][0-9]* DONE$/ { if (!($2 in want) || seen[$2]++) bad=1; ++count }
/^HARMONIZER_TARGET_COMPLETE groups=4$/ { ++final; if(count!=4) bad=1 }
END { if(bad || count!=4 || final!=1) exit 1; for(g in want) if(seen[g]!=1) exit 1 }
' .task-build/target.stdout
sh task_support/baseline.sh
