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
	"encoding/hex"
	"fmt"
	"regexp"
	"sort"
	"strconv"
	"strings"

	"github.com/elastic/go-libaudit/v2/auparse"
)

var (
	// execveArgKeyPattern matches whole argument values (a0, a1, ...).
	execveArgKeyPattern = regexp.MustCompile(`^a([0-9]+)$`)
	// execveArgLenPattern matches argument length declarations (a0_len, ...).
	execveArgLenPattern = regexp.MustCompile(`^a([0-9]+)_len$`)
	// execveArgFragPattern matches argument fragments (a0[0], a0[1], ...).
	execveArgFragPattern = regexp.MustCompile(`^a([0-9]+)\[([0-9]+)\]$`)
)

// execveValue is a raw field value together with the index of the EXECVE
// record (within the event) that carried it.
type execveValue struct {
	raw    string
	record int
}

// execveArgEvidence collects all evidence received for a single argv slot.
type execveArgEvidence struct {
	whole  *execveValue           // aN=value
	length *execveValue           // aN_len=value
	frags  map[uint64]execveValue // aN[K]=value
}

// execveAssembler reconstructs the process argv from all EXECVE records of
// one audit event. Any incomplete or contradictory evidence is reported as a
// warning and prevents the argv from being published as a whole; a partially
// trusted argument list is never returned.
type execveAssembler struct {
	event  *Event
	failed bool
}

func (a *execveAssembler) warnf(format string, args ...interface{}) {
	a.failed = true
	a.event.Warnings = append(a.event.Warnings, fmt.Errorf("execve: "+format, args...))
}

// addExecveRecords reconstructs the process argv from all EXECVE records of
// the event and stores it in event.Process.Args. The kernel splits long
// execve events across multiple EXECVE records and splits long arguments
// into aN_len / aN[K] fragments, so the argv is reassembled from the
// combined evidence of every record. Argument order is determined solely by
// the numeric N in aN, never by field or record order. If the evidence is
// incomplete or contradictory, Args is left unset and warnings are added;
// all other parsed event fields are retained.
func addExecveRecords(execves []*auparse.AuditMessage, event *Event) {
	a := &execveAssembler{event: event}

	var argcValues []execveValue
	args := map[uint64]*execveArgEvidence{}
	seen := make(map[string]int) // field key -> record index of first occurrence

	argEvidence := func(idx uint64) *execveArgEvidence {
		ev := args[idx]
		if ev == nil {
			ev = &execveArgEvidence{}
			args[idx] = ev
		}
		return ev
	}

	for recIdx, msg := range execves {
		fields, err := msg.ExecveFields()
		if err != nil {
			a.warnf("record %d: failed to extract fields: %v", recIdx, err)
			continue
		}
		for _, f := range fields {
			key, val := f.Key, f.Value

			// argc may appear in only the first record, but later records
			// may repeat it; every occurrence must agree.
			if key == "argc" {
				argcValues = append(argcValues, execveValue{val, recIdx})
				continue
			}

			// A duplicated raw field must not silently overwrite the first
			// occurrence, even if the values are identical.
			if first, dup := seen[key]; dup {
				a.warnf("record %d: duplicate field %q (first seen in record %d)", recIdx, key, first)
				continue
			}
			seen[key] = recIdx

			if m := execveArgFragPattern.FindStringSubmatch(key); m != nil {
				argIdx, ok1 := a.parseIndex(m[1], recIdx, key)
				fragIdx, ok2 := a.parseIndex(m[2], recIdx, key)
				if !ok1 || !ok2 {
					continue
				}
				ev := argEvidence(argIdx)
				if ev.frags == nil {
					ev.frags = map[uint64]execveValue{}
				}
				ev.frags[fragIdx] = execveValue{val, recIdx}
				continue
			}
			if m := execveArgLenPattern.FindStringSubmatch(key); m != nil {
				argIdx, ok := a.parseIndex(m[1], recIdx, key)
				if !ok {
					continue
				}
				argEvidence(argIdx).length = &execveValue{val, recIdx}
				continue
			}
			if m := execveArgKeyPattern.FindStringSubmatch(key); m != nil {
				argIdx, ok := a.parseIndex(m[1], recIdx, key)
				if !ok {
					continue
				}
				argEvidence(argIdx).whole = &execveValue{val, recIdx}
				continue
			}
			// Not an argv field; ignore it.
		}
	}

	if len(argcValues) == 0 {
		a.warnf("argc not found in any EXECVE record")
		return
	}
	argcRaw := argcValues[0]
	for _, v := range argcValues[1:] {
		if v.raw != argcRaw.raw {
			a.warnf("record %d: argc=%q conflicts with argc=%q from record %d",
				v.record, v.raw, argcRaw.raw, argcRaw.record)
		}
	}
	argc, err := strconv.ParseUint(argcRaw.raw, 10, 64)
	if err != nil {
		a.warnf("record %d: failed to parse argc=%q: %v", argcRaw.record, argcRaw.raw, err)
		return
	}
	event.Data["argc"] = argcRaw.raw

	// Arguments outside of the declared argc are contradictory evidence.
	var extra []uint64
	for idx := range args {
		if idx >= argc {
			extra = append(extra, idx)
		}
	}
	sort.Slice(extra, func(i, j int) bool { return extra[i] < extra[j] })
	for _, idx := range extra {
		a.warnf("argument a%d is outside the declared argc=%d", idx, argc)
	}
	if a.failed {
		return
	}

	// Do not preallocate based on the declared argc: a forged huge argc
	// must not cause a huge allocation. The loop below is bounded by the
	// number of arguments actually received because it stops at the first
	// missing one.
	argv := make([]string, 0, len(args))
	for idx := uint64(0); idx < argc; idx++ {
		ev, found := args[idx]
		if !found {
			a.warnf("argument a%d is missing (argc=%d)", idx, argc)
			return
		}
		arg, ok := a.assembleArg(idx, ev)
		if !ok {
			return
		}
		argv = append(argv, arg)
	}
	event.Process.Args = argv
}

// parseIndex parses an argument or fragment index. Abnormally large indexes
// that overflow uint64 are reported rather than trusted.
func (a *execveAssembler) parseIndex(s string, recIdx int, key string) (uint64, bool) {
	idx, err := strconv.ParseUint(s, 10, 64)
	if err != nil {
		a.warnf("record %d: field %q has an invalid index: %v", recIdx, key, err)
		return 0, false
	}
	return idx, true
}

// assembleArg reconstructs a single argument from its evidence.
func (a *execveAssembler) assembleArg(idx uint64, ev *execveArgEvidence) (string, bool) {
	if ev.whole != nil && len(ev.frags) > 0 {
		a.warnf("argument a%d has both a whole value (record %d) and %d fragment(s)",
			idx, ev.whole.record, len(ev.frags))
		return "", false
	}

	var value string
	var repLen int

	switch {
	case ev.whole != nil:
		v, n, ok := a.decodeValue(idx, -1, *ev.whole)
		if !ok {
			return "", false
		}
		value, repLen = v, n

	case len(ev.frags) > 0:
		if ev.length == nil {
			a.warnf("argument a%d is fragmented but a%d_len is missing", idx, idx)
			return "", false
		}
		// Fragments must be contiguous from 0 to the last one and are
		// concatenated in numeric order (fragment 10 comes after 9, not
		// before 2 as string ordering would imply).
		for k := uint64(0); k < uint64(len(ev.frags)); k++ {
			if _, found := ev.frags[k]; !found {
				a.warnf("argument a%d is missing fragment [%d]", idx, k)
				return "", false
			}
		}
		// All fragments of one argument must use the same representation.
		quoted := isQuotedValue(ev.frags[0].raw)
		var sb strings.Builder
		for k := uint64(0); k < uint64(len(ev.frags)); k++ {
			f := ev.frags[k]
			if isQuotedValue(f.raw) != quoted {
				a.warnf("record %d: argument a%d fragment [%d] mixes quoted and hexadecimal representations",
					f.record, idx, k)
				return "", false
			}
			v, n, ok := a.decodeValue(idx, int64(k), f)
			if !ok {
				return "", false
			}
			sb.WriteString(v)
			repLen += n
		}
		value = sb.String()

	default:
		a.warnf("argument a%d has a length declaration but no value", idx)
		return "", false
	}

	// Validate aN_len against the length of the value in its audit output
	// representation: the unquoted byte count for quoted values, and the
	// hexadecimal character count (not the decoded byte count) for hex
	// values.
	if ev.length != nil {
		declared, err := strconv.ParseUint(strings.Trim(ev.length.raw, `"'`), 10, 64)
		if err != nil {
			a.warnf("record %d: failed to parse a%d_len=%q: %v", ev.length.record, idx, ev.length.raw, err)
			return "", false
		}
		if declared != uint64(repLen) {
			a.warnf("argument a%d: declared a%d_len=%d does not match the received data length %d",
				idx, idx, declared, repLen)
			return "", false
		}
	}

	// NUL is not a legal execve argument byte.
	if strings.IndexByte(value, 0) != -1 {
		a.warnf("argument a%d contains a NUL byte, which is not valid in an execve argument", idx)
		return "", false
	}

	return value, true
}

// decodeValue decodes a whole argument value or a single fragment. It
// returns the decoded bytes and the length of the value in its audit output
// representation (used to validate aN_len). Quoted values keep their raw
// bytes as-is; unquoted values are interpreted as hexadecimal. No shell
// parsing or UTF-8 replacement is performed.
func (a *execveAssembler) decodeValue(argIdx uint64, fragIdx int64, v execveValue) (string, int, bool) {
	where := fmt.Sprintf("argument a%d", argIdx)
	if fragIdx >= 0 {
		where = fmt.Sprintf("argument a%d fragment [%d]", argIdx, fragIdx)
	}

	raw := v.raw
	if isQuotedValue(raw) {
		return raw[1 : len(raw)-1], len(raw) - 2, true
	}
	if len(raw) > 0 && (raw[0] == '"' || raw[0] == '\'') {
		a.warnf("record %d: %s has an unterminated quoted value", v.record, where)
		return "", 0, false
	}
	decoded, err := hex.DecodeString(raw)
	if err != nil {
		a.warnf("record %d: %s is not valid hexadecimal: %v", v.record, where, err)
		return "", 0, false
	}
	return string(decoded), len(raw), true
}

// isQuotedValue returns true if the raw value is enclosed in matching
// single or double quotes.
func isQuotedValue(raw string) bool {
	if len(raw) < 2 {
		return false
	}
	first, last := raw[0], raw[len(raw)-1]
	return first == last && (first == '"' || first == '\'')
}
