# Telemetry ingress contract

`telemetry_ingress_test.go` exercises the production `Node.Events()` pipeline
(not only the low-level readers), so `Channel` datagram handling, source
attribution and message decoding are covered together.

## Scenarios the unmodified library passes

- **stream_fragmented_and_coalesced**
  - `net.Pipe` plus a reader capped at 3 bytes/Read, so headers, payloads and
    checksums are split across many Reads.
  - Two complete frames written as one contiguous byte stream arrive in order
    as two `EventFrame`s.
  - A known message with a corrupted CRC yields `EventParseError` and is not
    published; a following valid frame is still ingested.
  - V2 payloads that omit trailing zero base bytes and/or extension fields
    decode those omitted bytes as zero.
  - A V1 frame is decoded using only the base field layout; extension fields
    stay zero.
  - A V2 payload with a non-zero `uint16` extension and base fields laid out in
    MAVLink width order decodes to the independently expected values.
  - An unknown message id (8001, unused by common.xml) is delivered as
    `MessageRaw` with its id and raw payload preserved.
  - Every `EventFrame` and `EventParseError` is checked against the channel
    that produced it and against the source system/component id.
- **datagram_one_frame_per_packet** (real loopback UDP where local binding is
  allowed; in-memory packet pair only when the runner returns EPERM)
  - A packet containing two frames delivers only the first frame.
  - The next packet first reports the previous packet's unconsumed byte count
    as `EventParseError`, then delivers the new packet's frame; stale bytes are
    not decoded as the new frame.
  - A truncated datagram produces a parse error, its buffered leftovers are
    reported as skipped bytes when the next packet arrives, and the next
    complete packet is decoded on its own.
  - The stream and datagram sources use different system/component ids and
    every event is checked for the correct channel and source.
- **public_write_path_compatibility**
  - One `Node.WriteMessageTo` write is read back with the test's independent
    V2 decoder; id, source, checksum and field values are verified.
- Wire bytes and expected field values come from hand-assembled payloads in
  the test plus an independent X25/CRC-extra reference implementation; the
  test does not generate fixtures with the library writer and read them back
  through the library reader.

## Degradation patches and the assertions they actually trigger

All logs are saved under `checks/logs/`.

| Patch | Scenario | Failing test | Assertion text |
| --- | --- | --- | --- |
| `01-datagram-buffer-retained.patch` | leftover datagram bytes are not discarded | `TestTelemetryIngressContract/datagram_one_frame_per_packet` | `datagram leftover bytes leaked into next packet` |
| `02-extension-padding-nonzero.patch` | missing extension bytes padded with `0xAA` | `TestTelemetryIngressContract/stream_fragmented_and_coalesced` (also the datagram subtest) | `omitted extension field decoded as non-zero` |
| `03-crc-check-skipped.patch` | known-message checksum validation removed | `TestTelemetryIngressContract/stream_fragmented_and_coalesced` | `known message with bad CRC was published` |

`checks/verify-telemetry.sh` applies exactly one patch to its own temporary
source copy, compiles the target package first, then runs
`^TestTelemetryIngressContract$`. A scenario is counted as caught only when the
expected subtest fails with the expected behavior assertion. Compile failures,
timeouts, missing files and zero test cases are reported as not caught.

## Not run here

- The full `go test ./...` suite was not run: the sandbox has local port binding
  disabled, so existing UDP/TCP tests such as `TestEndpointUDPBroadcast`,
  `TestNodeRoute`, `TestNodeFixFrame`, `TestNodeSignature` and
  `TestNodeWriteSameToMultiple` fail or hang with `bind: operation not
  permitted`. This is an environment restriction, not a code regression.
- The real loopback UDP branch was not exercised in this sandbox for the same
  reason; the test logged the EPERM fallback and used an in-memory packet pair
  with the same one-Write/one-Read datagram semantics. On a runner with
  `sandbox.network.allowLocalBinding` enabled, the real UDP branch runs.
- `baseline_test.go` (build tag `baselinecheck`) was not run; it is outside the
  acceptance command, and its assertions were left untouched.
- No SITL/flight controller, serial device or device-control command was used;
  only telemetry frames (`GPS_RAW_INT`, `VFR_HUD`) are involved.
- Packages outside the root package (`pkg/frame`, `pkg/message`, dialects, ...)
  were not re-run; their code is unchanged.

Sandbox note for the acceptance command: plain `mktemp` writes to the macOS
per-user temp directory, which this sandbox blocks. With a writable `out` path
(the rest of the command unchanged), the chain passes; in a normal shell the
original `out=$(mktemp)` command works.
