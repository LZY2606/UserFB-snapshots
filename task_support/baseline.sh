#!/bin/bash
set -euo pipefail
.task-build/baseline
.task-build/mkvmuxer_sample -i .task-build/baseline-av.webm -o .task-build/sample-remux.webm > .task-build/sample.stdout
.task-build/baseline .task-build/sample-remux.webm
printf '%s\n' 'OLD_PATHS roundtrip_audio=PASS roundtrip_av=PASS upstream_sample=PASS'
