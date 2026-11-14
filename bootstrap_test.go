package main

import (
	"os"
	"testing"
)

// Prepare only an empty guest configuration before the original main.init.
var testBootstrap = func() bool {
	testing.Init()
	os.Args = append(os.Args, "-guests-allowed", "-guests-quota=author-guest", "-quotaDir=task_support/emptyquota", "-users=task_support/empty-users")
	return true
}()
