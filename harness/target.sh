#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
bash harness/build.sh target
java -ea -cp .build/classes:src/test/resources TemplateHarness | tee .build/template-target.log
grep -Eq '^TEMPLATE_CONTRACT_OK cases=[1-9][0-9]* groups=3$' .build/template-target.log
java -ea -cp .build/classes:src/test/resources LegacyCases
