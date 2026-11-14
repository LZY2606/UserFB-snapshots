#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
rm -rf .task-build/classes
mkdir -p .task-build/classes
find src/main/java/crawlercommons/sitemaps src/main/java/crawlercommons/mimetypes -name '*.java' ! -name SiteMapTester.java ! -name SiteMapCrossSubmitValidator.java > .task-build/sources
printf '%s\n' src/main/java/crawlercommons/filters/URLFilter.java src/main/java/crawlercommons/utils/URLUtils.java src/main/java/crawlercommons/url/CrawlerURL.java task_support/Fixture.java task_support/Compatibility.java task_support/Baseline.java task_support/Checks.java task_support/TargetDriver.java >> .task-build/sources
javac -encoding UTF-8 --release 11 --add-modules jdk.httpserver -classpath .task-build/classes -sourcepath src/main/java:vendor/commons-io/src/main/java:vendor/slf4j/src/main/java:task_support -d .task-build/classes @.task-build/sources
