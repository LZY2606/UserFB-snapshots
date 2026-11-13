package pt

import "testing"

// targetCheck records a successful content assertion for the fixed runner.
func targetCheck(t *testing.T, ok bool, label string) {
	t.Helper()
	if !ok {
		t.Fatalf("content assertion failed: %s", label)
	}
	t.Logf("TARGET_CONTENT %s", label)
}
