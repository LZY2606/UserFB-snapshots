set -euo pipefail
cmake -S . -B "$1" -DBUILD_COMMAND_LINE_TOOLS=OFF -DBUILD_SHARED_LIBS=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build "$1" --target pe-parse -j 4
