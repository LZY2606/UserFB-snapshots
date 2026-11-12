package checks
import "testing"
// Check records a successful content assertion for the target runner.
func Check(t *testing.T, ok bool) {
 t.Helper()
 if !ok { t.Fatal("content assertion failed") }
 t.Log("CHECK_CONTENT_CHECK")
}
