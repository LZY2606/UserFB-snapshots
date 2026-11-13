package util

import (
	taskassert "github.com/headzoo/surf/task_support/assert"
	"testing"
)

func TestFileExists(t *testing.T) {
	ut := taskassert.New(t)
	ex := FileExists("./util_test.go")
	ut.AssertTrue(ex)

	ex = FileExists("./util.txt")
	ut.AssertFalse(ex)
}
