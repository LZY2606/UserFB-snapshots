// Licensed to Elasticsearch B.V. under one or more contributor
// license agreements. See the NOTICE file distributed with
// this work for additional information regarding copyright
// ownership. Elasticsearch B.V. licenses this file to you under
// the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

package aucoalesce

import (
	"strings"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"github.com/elastic/go-libaudit/v2/auparse"
)

const execveSyscallLine = `type=SYSCALL msg=audit(1700000000.001:42): arch=c000003e syscall=59 success=yes exit=0 pid=12 ppid=1 uid=1000 gid=1000 comm="echo" exe="/bin/echo"`

const execveProctitleLine = `type=PROCTITLE msg=audit(1700000000.001:42): proctitle=73686F7274`

const execveCwdLine = `type=CWD msg=audit(1700000000.001:42): cwd="/home/test"`

const execvePathLine = `type=PATH msg=audit(1700000000.001:42): item=0 name="/bin/echo" inode=1 dev=00:00 mode=0100755 ouid=0 ogid=0 nametype=NORMAL`

func parseMessages(t *testing.T, lines ...string) []*auparse.AuditMessage {
	t.Helper()
	msgs := make([]*auparse.AuditMessage, 0, len(lines))
	for _, line := range lines {
		msg, err := auparse.ParseLogLine(line)
		require.NoError(t, err)
		msgs = append(msgs, msg)
	}
	return msgs
}

func coalesceLines(t *testing.T, lines ...string) *Event {
	t.Helper()
	event, err := CoalesceMessages(parseMessages(t, lines...))
	require.NoError(t, err)
	require.NotNil(t, event)
	return event
}

func warningStrings(event *Event) []string {
	warnings := make([]string, 0, len(event.Warnings))
	for _, w := range event.Warnings {
		warnings = append(warnings, w.Error())
	}
	return warnings
}

func joinedWarnings(event *Event) string {
	return strings.Join(warningStrings(event), "\n")
}

// TestExecveAssemblyRecords covers argv reassembly across multiple EXECVE
// records of one event.
func TestExecveAssemblyRecords(t *testing.T) {
	t.Run("single record", func(t *testing.T) {
		event := coalesceLines(t,
			execveSyscallLine,
			`type=EXECVE msg=audit(1700000000.001:42): argc=3 a0="echo" a1="alpha" a2="beta"`)
		assert.Equal(t, []string{"echo", "alpha", "beta"}, event.Process.Args)
		assert.Empty(t, event.Warnings)
	})

	t.Run("split across two records", func(t *testing.T) {
		event := coalesceLines(t,
			execveSyscallLine,
			`type=EXECVE msg=audit(1700000000.001:42): argc=3 a0="echo" a1="alpha"`,
			`type=EXECVE msg=audit(1700000000.001:42): a2="beta"`)
		assert.Equal(t, []string{"echo", "alpha", "beta"}, event.Process.Args)
		assert.Empty(t, event.Warnings)
	})

	t.Run("argc repeated consistently", func(t *testing.T) {
		event := coalesceLines(t,
			execveSyscallLine,
			`type=EXECVE msg=audit(1700000000.001:42): argc=2 a0="echo"`,
			`type=EXECVE msg=audit(1700000000.001:42): argc=2 a1="alpha"`)
		assert.Equal(t, []string{"echo", "alpha"}, event.Process.Args)
		assert.Empty(t, event.Warnings)
	})

	t.Run("argument order comes from indexes not record or field order", func(t *testing.T) {
		event := coalesceLines(t,
			execveSyscallLine,
			`type=EXECVE msg=audit(1700000000.001:42): a2="beta"`,
			`type=EXECVE msg=audit(1700000000.001:42): a1="alpha" argc=3 a0="echo"`)
		assert.Equal(t, []string{"echo", "alpha", "beta"}, event.Process.Args)
		assert.Empty(t, event.Warnings)
	})

	t.Run("extra arguments beyond argc are contradictory", func(t *testing.T) {
		event := coalesceLines(t,
			execveSyscallLine,
			`type=EXECVE msg=audit(1700000000.001:42): argc=2 a0="echo" a1=""`,
			`type=EXECVE msg=audit(1700000000.001:42): a2="hello world" a3=4142`,
			`type=EXECVE msg=audit(1700000000.001:42): a4="regex=8"`,
		)
		// argc says 2 but five arguments were received: contradictory.
		assert.Nil(t, event.Process.Args)
		assert.Contains(t, joinedWarnings(event), "outside the declared argc=2")
	})

	t.Run("mixed argument representations", func(t *testing.T) {
		event := coalesceLines(t,
			execveSyscallLine,
			`type=EXECVE msg=audit(1700000000.001:42): argc=5 a0="echo" a1=""`,
			`type=EXECVE msg=audit(1700000000.001:42): a2="hello world" a3=4142`,
			`type=EXECVE msg=audit(1700000000.001:42): a4="regex=8"`,
		)
		assert.Equal(t, []string{"echo", "", "hello world", "AB", "regex=8"}, event.Process.Args)
		assert.Empty(t, event.Warnings)
	})

	t.Run("proctitle is display only and never fills argv", func(t *testing.T) {
		event := coalesceLines(t,
			execveSyscallLine,
			`type=EXECVE msg=audit(1700000000.001:42): argc=2 a0="echo" a1="alpha"`,
			execveProctitleLine)
		assert.Equal(t, []string{"echo", "alpha"}, event.Process.Args)
		assert.Equal(t, "short", event.Process.Title)
	})

	t.Run("proctitle does not complete a partial argv", func(t *testing.T) {
		event := coalesceLines(t,
			execveSyscallLine,
			`type=EXECVE msg=audit(1700000000.001:42): argc=3 a0="echo" a1="alpha"`,
			execveProctitleLine)
		assert.Nil(t, event.Process.Args)
		assert.Equal(t, "short", event.Process.Title)
		assert.Contains(t, joinedWarnings(event), "argument a2 is missing")
	})

	t.Run("incomplete argv keeps syscall path and process fields", func(t *testing.T) {
		event := coalesceLines(t,
			execveSyscallLine,
			`type=EXECVE msg=audit(1700000000.001:42): argc=3 a0="echo"`,
			execveCwdLine,
			execvePathLine,
			execveProctitleLine)
		assert.Nil(t, event.Process.Args)
		assert.Equal(t, "12", event.Process.PID)
		assert.Equal(t, "echo", event.Process.Name)
		assert.Equal(t, "/bin/echo", event.Process.Exe)
		assert.Equal(t, "/home/test", event.Process.CWD)
		assert.Equal(t, "execve", event.Data["syscall"])
		require.Len(t, event.Paths, 1)
		assert.Equal(t, "/bin/echo", event.Paths[0]["name"])
		assert.Contains(t, joinedWarnings(event), "argument a1 is missing")
	})
}

// TestExecveAssemblyFragments covers reassembly of long arguments split
// into aN_len / aN[K] fragments.
func TestExecveAssemblyFragments(t *testing.T) {
	t.Run("quoted fragments across records", func(t *testing.T) {
		event := coalesceLines(t,
			execveSyscallLine,
			`type=EXECVE msg=audit(1700000000.001:42): argc=2 a0="echo" a1_len=11 a1[0]="hello "`,
			`type=EXECVE msg=audit(1700000000.001:42): a1[1]="world"`)
		assert.Equal(t, []string{"echo", "hello world"}, event.Process.Args)
		assert.Empty(t, event.Warnings)
	})

	t.Run("hex fragments validated by hex character count", func(t *testing.T) {
		// "hello" is 5 bytes but 10 hex characters; a0_len counts the
		// hex characters of the audit output representation.
		event := coalesceLines(t,
			execveSyscallLine,
			`type=EXECVE msg=audit(1700000000.001:42): argc=1 a0_len=10 a0[0]=68656C`,
			`type=EXECVE msg=audit(1700000000.001:42): a0[1]=6C6F`)
		assert.Equal(t, []string{"hello"}, event.Process.Args)
		assert.Empty(t, event.Warnings)
	})

	t.Run("fragments concatenate in numeric not string order", func(t *testing.T) {
		// Twelve one-character fragments listed out of order; fragment 10
		// and 11 must sort after 9, not before 2.
		event := coalesceLines(t,
			execveSyscallLine,
			`type=EXECVE msg=audit(1700000000.001:42): argc=1 a0_len=12 a0[10]="j" a0[0]="a" a0[5]="f" a0[1]="b"`,
			`type=EXECVE msg=audit(1700000000.001:42): a0[11]="k" a0[2]="c" a0[9]="i" a0[3]="d"`,
			`type=EXECVE msg=audit(1700000000.001:42): a0[4]="e" a0[6]="g" a0[8]="h" a0[7]="g"`)
		assert.Equal(t, []string{"abcdefgghijk"}, event.Process.Args)
		assert.Empty(t, event.Warnings)
	})

	t.Run("mixed normal and fragmented arguments", func(t *testing.T) {
		event := coalesceLines(t,
			execveSyscallLine,
			`type=EXECVE msg=audit(1700000000.001:42): argc=3 a0="echo" a1="alpha" a2_len=5 a2[0]="ga"`,
			`type=EXECVE msg=audit(1700000000.001:42): a2[1]="mm"`,
			`type=EXECVE msg=audit(1700000000.001:42): a2[2]="a"`)
		assert.Equal(t, []string{"echo", "alpha", "gamma"}, event.Process.Args)
		assert.Empty(t, event.Warnings)
	})

	t.Run("length mismatch", func(t *testing.T) {
		event := coalesceLines(t,
			execveSyscallLine,
			`type=EXECVE msg=audit(1700000000.001:42): argc=1 a0_len=12 a0[0]="hello" a0[1]="world"`)
		assert.Nil(t, event.Process.Args)
		assert.Contains(t, joinedWarnings(event), "a0_len=12 does not match")
	})

	t.Run("missing fragment", func(t *testing.T) {
		event := coalesceLines(t,
			execveSyscallLine,
			`type=EXECVE msg=audit(1700000000.001:42): argc=1 a0_len=11 a0[0]="hello" a0[2]="world"`)
		assert.Nil(t, event.Process.Args)
		assert.Contains(t, joinedWarnings(event), "argument a0 is missing fragment [1]")
	})

	t.Run("missing length declaration", func(t *testing.T) {
		event := coalesceLines(t,
			execveSyscallLine,
			`type=EXECVE msg=audit(1700000000.001:42): argc=1 a0[0]="hello" a0[1]="world"`)
		assert.Nil(t, event.Process.Args)
		assert.Contains(t, joinedWarnings(event), "a0_len is missing")
	})

	t.Run("odd number of hex characters", func(t *testing.T) {
		event := coalesceLines(t,
			execveSyscallLine,
			`type=EXECVE msg=audit(1700000000.001:42): argc=1 a0_len=3 a0[0]=686`)
		assert.Nil(t, event.Process.Args)
		assert.Contains(t, joinedWarnings(event), "not valid hexadecimal")
	})

	t.Run("non hex characters", func(t *testing.T) {
		event := coalesceLines(t,
			execveSyscallLine,
			`type=EXECVE msg=audit(1700000000.001:42): argc=1 a0_len=4 a0[0]=68XY`)
		assert.Nil(t, event.Process.Args)
		assert.Contains(t, joinedWarnings(event), "not valid hexadecimal")
	})

	t.Run("mixed fragment representations", func(t *testing.T) {
		event := coalesceLines(t,
			execveSyscallLine,
			`type=EXECVE msg=audit(1700000000.001:42): argc=1 a0_len=8 a0[0]="hel" a0[1]=6C6F`)
		assert.Nil(t, event.Process.Args)
		assert.Contains(t, joinedWarnings(event), "mixes quoted and hexadecimal")
	})

	t.Run("duplicate fragment index", func(t *testing.T) {
		event := coalesceLines(t,
			execveSyscallLine,
			`type=EXECVE msg=audit(1700000000.001:42): argc=1 a0_len=3 a0[0]="foo"`,
			`type=EXECVE msg=audit(1700000000.001:42): a0[0]="foo"`)
		assert.Nil(t, event.Process.Args)
		assert.Contains(t, joinedWarnings(event), `duplicate field "a0[0]"`)
	})
}

// TestExecveAssemblyEvidence covers incomplete or contradictory evidence:
// the argv must not be published and a warning must locate the record,
// argument, and fragment involved.
func TestExecveAssemblyEvidence(t *testing.T) {
	t.Run("conflicting argc across records", func(t *testing.T) {
		event := coalesceLines(t,
			execveSyscallLine,
			`type=EXECVE msg=audit(1700000000.001:42): argc=2 a0="echo" a1="alpha"`,
			`type=EXECVE msg=audit(1700000000.001:42): argc=3 a2="beta"`)
		assert.Nil(t, event.Process.Args)
		warnings := joinedWarnings(event)
		assert.Contains(t, warnings, "record 1")
		assert.Contains(t, warnings, `argc="3" conflicts with argc="2"`)
	})

	t.Run("duplicate field with identical value", func(t *testing.T) {
		event := coalesceLines(t,
			execveSyscallLine,
			`type=EXECVE msg=audit(1700000000.001:42): argc=2 a0="echo" a1="alpha"`,
			`type=EXECVE msg=audit(1700000000.001:42): a1="alpha"`)
		assert.Nil(t, event.Process.Args)
		warnings := joinedWarnings(event)
		assert.Contains(t, warnings, `duplicate field "a1"`)
		assert.Contains(t, warnings, "record 1")
	})

	t.Run("whole value and fragments for the same argument", func(t *testing.T) {
		event := coalesceLines(t,
			execveSyscallLine,
			`type=EXECVE msg=audit(1700000000.001:42): argc=1 a0="echo" a0_len=4 a0[0]="ec" a0[1]="ho"`)
		assert.Nil(t, event.Process.Args)
		assert.Contains(t, joinedWarnings(event), "argument a0 has both a whole value")
	})

	t.Run("missing argc", func(t *testing.T) {
		event := coalesceLines(t,
			execveSyscallLine,
			`type=EXECVE msg=audit(1700000000.001:42): a0="echo" a1="alpha"`)
		assert.Nil(t, event.Process.Args)
		assert.Contains(t, joinedWarnings(event), "argc not found")
	})

	t.Run("missing middle argument", func(t *testing.T) {
		event := coalesceLines(t,
			execveSyscallLine,
			`type=EXECVE msg=audit(1700000000.001:42): argc=3 a0="echo" a2="beta"`)
		assert.Nil(t, event.Process.Args)
		assert.Contains(t, joinedWarnings(event), "argument a1 is missing (argc=3)")
	})

	t.Run("NUL byte in hex argument", func(t *testing.T) {
		event := coalesceLines(t,
			execveSyscallLine,
			`type=EXECVE msg=audit(1700000000.001:42): argc=1 a0=410042`)
		assert.Nil(t, event.Process.Args)
		assert.Contains(t, joinedWarnings(event), "NUL byte")
	})

	t.Run("huge argc does not allocate or hang", func(t *testing.T) {
		event := coalesceLines(t,
			execveSyscallLine,
			`type=EXECVE msg=audit(1700000000.001:42): argc=18446744073709551615 a0="echo"`)
		assert.Nil(t, event.Process.Args)
		assert.Contains(t, joinedWarnings(event), "argument a1 is missing")
	})

	t.Run("argc overflowing uint64", func(t *testing.T) {
		event := coalesceLines(t,
			execveSyscallLine,
			`type=EXECVE msg=audit(1700000000.001:42): argc=99999999999999999999999 a0="echo"`)
		assert.Nil(t, event.Process.Args)
		assert.Contains(t, joinedWarnings(event), "failed to parse argc")
	})

	t.Run("argument index overflowing uint64", func(t *testing.T) {
		event := coalesceLines(t,
			execveSyscallLine,
			`type=EXECVE msg=audit(1700000000.001:42): argc=1 a99999999999999999999999="echo" a0="echo"`)
		assert.Nil(t, event.Process.Args)
		assert.Contains(t, joinedWarnings(event), "invalid index")
	})

	t.Run("argument outside declared argc", func(t *testing.T) {
		event := coalesceLines(t,
			execveSyscallLine,
			`type=EXECVE msg=audit(1700000000.001:42): argc=1 a0="echo" a1="sneaky"`)
		assert.Nil(t, event.Process.Args)
		assert.Contains(t, joinedWarnings(event), "argument a1 is outside the declared argc=1")
	})

	t.Run("unterminated quoted value", func(t *testing.T) {
		event := coalesceLines(t,
			execveSyscallLine,
			`type=EXECVE msg=audit(1700000000.001:42): argc=1 a0="echo`)
		assert.Nil(t, event.Process.Args)
		assert.Contains(t, joinedWarnings(event), "unterminated quoted value")
	})

	t.Run("length declaration without value", func(t *testing.T) {
		event := coalesceLines(t,
			execveSyscallLine,
			`type=EXECVE msg=audit(1700000000.001:42): argc=1 a0_len=4`)
		assert.Nil(t, event.Process.Args)
		assert.Contains(t, joinedWarnings(event), "argument a0 has a length declaration but no value")
	})
}

// TestExecveAssemblyIdentity covers determinism and cache safety: repeated
// coalescing of the same messages must yield the same Args and warnings,
// and messages with mismatched event identity must be rejected.
func TestExecveAssemblyIdentity(t *testing.T) {
	t.Run("repeated coalescing gives identical args and warnings", func(t *testing.T) {
		msgs := parseMessages(t,
			execveSyscallLine,
			`type=EXECVE msg=audit(1700000000.001:42): argc=3 a0="echo" a1_len=5 a1[0]="alp"`,
			`type=EXECVE msg=audit(1700000000.001:42): a1[1]="ha" a2="beta"`)

		first, err := CoalesceMessages(msgs)
		require.NoError(t, err)
		second, err := CoalesceMessages(msgs)
		require.NoError(t, err)

		assert.Equal(t, []string{"echo", "alpha", "beta"}, first.Process.Args)
		assert.Equal(t, first.Process.Args, second.Process.Args)
		assert.Equal(t, warningStrings(first), warningStrings(second))
	})

	t.Run("repeated coalescing of incomplete evidence", func(t *testing.T) {
		msgs := parseMessages(t,
			execveSyscallLine,
			`type=EXECVE msg=audit(1700000000.001:42): argc=3 a0="echo"`,
			`type=EXECVE msg=audit(1700000000.001:42): a2="beta"`)

		first, err := CoalesceMessages(msgs)
		require.NoError(t, err)
		second, err := CoalesceMessages(msgs)
		require.NoError(t, err)

		assert.Nil(t, first.Process.Args)
		assert.Equal(t, first.Process.Args, second.Process.Args)
		assert.Equal(t, warningStrings(first), warningStrings(second))
		assert.NotEmpty(t, first.Warnings)
	})

	t.Run("coalescing does not consume cached message data", func(t *testing.T) {
		msgs := parseMessages(t,
			execveSyscallLine,
			`type=EXECVE msg=audit(1700000000.001:42): argc=2 a0="echo"`,
			`type=EXECVE msg=audit(1700000000.001:42): a1="alpha"`)

		_, err := CoalesceMessages(msgs)
		require.NoError(t, err)

		// The parsed data of each record is still intact afterwards.
		data, err := msgs[1].Data()
		require.NoError(t, err)
		assert.Equal(t, "2", data["argc"])
		assert.Equal(t, "echo", data["a0"])

		// A standalone partial EXECVE record returns the fields it
		// actually carries instead of failing because other arguments
		// live in later records.
		data, err = msgs[2].Data()
		require.NoError(t, err)
		assert.Equal(t, "alpha", data["a1"])
	})

	t.Run("mismatched sequence numbers are rejected", func(t *testing.T) {
		msgs := parseMessages(t,
			execveSyscallLine,
			`type=EXECVE msg=audit(1700000000.001:43): argc=1 a0="echo"`)
		event, err := CoalesceMessages(msgs)
		require.Error(t, err)
		assert.Nil(t, event)
		assert.Contains(t, err.Error(), "sequence")
	})

	t.Run("mismatched timestamps are rejected", func(t *testing.T) {
		msgs := parseMessages(t,
			execveSyscallLine,
			`type=EXECVE msg=audit(1700000000.002:42): argc=1 a0="echo"`)
		event, err := CoalesceMessages(msgs)
		require.Error(t, err)
		assert.Nil(t, event)
		assert.Contains(t, err.Error(), "timestamp")
	})
}
