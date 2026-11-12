set -euo pipefail
export GOPROXY=off GOSUMDB=off
go run -mod=vendor ./checks/baseline.go
go test -mod=vendor ./parser -count=1 -run '^Test(ParseUSNAfterPaddingWithZeroLeadingLength|CarveUSNFindsEightByteAlignedRecordsOnce|CarveUSNReadsLongRecordsAcrossBuffers|CarveUSNReportsRecordsNearTheEndOnce|CarveUSNWithTinyClusterSizeTerminates)$' -v
echo USN_OLD_TESTS_OK
