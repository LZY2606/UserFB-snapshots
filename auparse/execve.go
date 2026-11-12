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

package auparse

import (
	"errors"
	"fmt"
)

// ExecveField is a single raw key=value field parsed from an AUDIT_EXECVE
// message. The value retains the original representation found in the
// message, including any surrounding quotes, so that hex-encoded and
// fragmented (aN_len / aN[K]) arguments can be validated and reassembled by
// the caller.
type ExecveField struct {
	Key   string // Field name (e.g. argc, a0, a0_len, a0[0]).
	Value string // Raw value as it appeared in the message (quotes preserved).
}

// ExecveFields returns the raw key=value fields carried by an AUDIT_EXECVE
// message in their order of appearance. Unlike Data it preserves duplicate
// keys, empty values, and the original value representation, and it does not
// require the record to carry argc or a complete argument set (the kernel
// may split a long execve event across multiple EXECVE records). An error is
// returned if the message is not an AUDIT_EXECVE message or carries no data.
func (m *AuditMessage) ExecveFields() ([]ExecveField, error) {
	if m.RecordType != AUDIT_EXECVE {
		return nil, fmt.Errorf("cannot extract execve fields from %v message", m.RecordType)
	}
	if m.offset < 0 {
		return nil, errors.New("message has no data content")
	}
	return parseExecveFields(m.RawData[m.offset:]), nil
}

// parseExecveFields tokenizes key=value pairs from the message payload. Keys
// may contain brackets (e.g. a1[0]) which the generic key=value regex used
// by Data does not support. Quoted values keep their quotes; no unescaping,
// hex decoding, or shell interpretation is performed.
func parseExecveFields(s string) []ExecveField {
	var fields []ExecveField
	i := 0
	for i < len(s) {
		// Skip whitespace between tokens.
		if s[i] == ' ' || s[i] == '\t' {
			i++
			continue
		}

		// Read the key up to '='.
		start := i
		for i < len(s) && s[i] != '=' && s[i] != ' ' && s[i] != '\t' {
			i++
		}
		if i == len(s) || s[i] != '=' || i == start {
			// Not a key=value token; skip to the next whitespace.
			for i < len(s) && s[i] != ' ' && s[i] != '\t' {
				i++
			}
			continue
		}
		key := s[start:i]
		i++ // Skip '='.

		var value string
		if i < len(s) && (s[i] == '"' || s[i] == '\'') {
			quote := s[i]
			end := i + 1
			for end < len(s) && s[end] != quote {
				end++
			}
			if end < len(s) {
				end++ // Include the closing quote.
			}
			value = s[i:end]
			i = end
		} else {
			end := i
			for end < len(s) && s[end] != ' ' && s[end] != '\t' {
				end++
			}
			value = s[i:end]
			i = end
		}

		fields = append(fields, ExecveField{Key: key, Value: value})
	}
	return fields
}
