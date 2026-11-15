#!/bin/bash
set -euo pipefail
go test -count=1 -v ./task_support
go test -count=1 -v . -run '^(TestDemuxerNextDataPATPMT|TestPacketPoolBoundedPESCompletesWithoutNextStart)$'
