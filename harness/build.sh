#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .build
rm -rf .build/classes
mkdir -p .build/classes
find src/main/java harness -name '*.java' -print | LC_ALL=C sort > .build/sources.txt
printf '%s\n' src/test/java/org/jsoup/TextUtil.java src/test/java/org/jsoup/parser/HtmlParserTest.java src/test/java/org/jsoup/parser/HtmlTreeBuilderTest.java >> .build/sources.txt
if [[ "${1:-baseline}" == target ]]; then
  test -s contract/TemplateParsingTest.java || { echo 'MISSING_TEMPLATE_TEST' >&2; exit 2; }
  printf '%s\n' contract/TemplateParsingTest.java >> .build/sources.txt
fi
javac --release 8 -encoding UTF-8 -d .build/classes @.build/sources.txt
