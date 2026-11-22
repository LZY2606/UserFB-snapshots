Opus timing and container references (offline contract)

- https://opus-codec.org/docs/opus_in_isobmff.html sections 4.3.2, 4.3.4, 4.4: dOps.PreSkip is informative; elst specifies actual presentation trimming. The final MP4 sample duration may exclude padding. This task supports one rate-1 edit only.
- https://www.rfc-editor.org/rfc/rfc6716.html section 3: TOC configuration = packet[0] >> 3. Configurations 0..11 use frame durations {480,960,1920,2880}[configuration % 4]; 12..15 use {480,960}[configuration % 2]; 16..31 use {120,240,480,960}[configuration % 4]. Units are per-channel frames at 48000 Hz.
- TOC code = packet[0] & 3. Codes 0/1/2 mean 1/2/2 frames. Code 3 needs a second byte; frame count is its low 6 bits. Reject zero count or total over 5760. This task delegates compressed frame validity to the caller and checks only these duration fields, plus table/range contracts in the prompt.
- https://www.rfc-editor.org/rfc/rfc7845.html sections 4 and 5: Ogg Opus uses 48k granules, a 19-byte version-1 OpusHead for family 0, and OpusTags. Header integer fields are little-endian. OpusHead pre_skip is subtracted once from the final presentation coordinate, not from each page granule.
- https://www.xiph.org/ogg/doc/framing.html: Ogg page version is 0; BOS=2, EOS=4; page number, serial, granule and checksum fields are little-endian. CRC uses polynomial 0x04c11db7, non-reflected, initial zero, no final xor, with the checksum field zeroed during calculation. A packet length divisible by 255 still needs a zero final lacing entry. This task deliberately writes one packet per page, without continuation pages.

These are specification constraints, not an implementation. The requested exporter and its tests are absent from the seed.
