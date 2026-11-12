// Copyright 2024 Google LLC.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package zip

import "encoding/binary"

// extraHeaderSize is the size of the tag/length prefix of an extra field record:
// a 16-bit little-endian tag followed by a 16-bit little-endian payload length.
const extraHeaderSize = 4

// unixIdentityExtraTags are the five extra field tag classes that carry Unix owner identity.
// All records with one of these tags are removed when DropUserIdentity is enabled, duplicate
// records of the same tag included. No other extra records are touched, and file names,
// comments and other extensions are not inspected for their content.
//
// The cleaned range is exactly these five tags, as required:
//   - 0x000d
//   - 0x5855
//   - 0x7855
//   - 0x7875
//   - 0x756e (the Info-ZIP "ux" UID/GID record, which may also carry timestamps)
var unixIdentityExtraTags = map[uint16]bool{
	0x000d: true,
	0x5855: true,
	0x7855: true,
	0x7875: true,
	0x756e: true,
}

// dropIdentityExtra removes every extra field record carrying Unix owner identity from extra.
//
// The remaining, complete records are kept byte for byte and in their original order; in
// particular ZIP64 (0x0001), extended timestamp (0x5455) and Unicode path (0x7075) records are
// preserved. If the field contains a trailing header shorter than four bytes, or a record whose
// declared length runs past the end, the boundaries of the remaining records can no longer be
// trusted and the whole field is cleared instead. Removing a record may also drop timestamps that
// were embedded in it; this is acceptable, FileHeader.Modified itself is never changed.
//
// The returned slice never shares its backing storage with extra, so mutating the sanitized view
// cannot damage the original bytes the view was computed from.
func dropIdentityExtra(extra []byte) []byte {
	cleaned := make([]byte, 0, len(extra))
	for pos := 0; pos < len(extra); {
		if len(extra)-pos < extraHeaderSize {
			// A truncated trailing header: the rest of the field cannot be parsed safely.
			return []byte{}
		}
		tag := binary.LittleEndian.Uint16(extra[pos:])
		size := int(binary.LittleEndian.Uint16(extra[pos+2:]))
		recordEnd := pos + extraHeaderSize + size
		if recordEnd > len(extra) {
			// The declared payload overruns the field: boundaries are untrustworthy.
			return []byte{}
		}
		if !unixIdentityExtraTags[tag] {
			cleaned = append(cleaned, extra[pos:recordEnd]...)
		}
		pos = recordEnd
	}
	return cleaned
}
