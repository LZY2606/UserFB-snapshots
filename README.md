# rtpfreeze — offline RTP/RTCP freeze review tool

An offline, single-binary review tool for reconstructing what happened during
a video freeze when the RTCP feedback cannot be trusted at face value: NACKs,
PLIs, receiver reports and transport-wide feedback may carry SSRC values that
were reused across a collision, a BYE/rebirth, a simulcast layer switch or an
ICE restart, while the encoder keeps reusing the same payload type.

It is written in Go, stores everything in a local SQLite file, serves one
HTML page over loopback HTTP, and never contacts any online service.

## Run

```sh
go mod download
go build ./...
go test ./... -count=1
go run ./cmd/server --addr 127.0.0.1:5532
# open http://127.0.0.1:5532
```

Dependencies are vendored (`./vendor`), so the build also works with
`GOPROXY=off`. The server creates `data/rtpfreeze.db` and, on an empty
database, imports the bundled sample automatically. Re-import it any time with
`--reseed`, or upload your own JSONL capture from the page.

## The identity rule (the core of the tool)

An SSRC value is **not** a stream identity. The tool partitions every capture
into three nested scopes, and evidence never crosses a boundary:

| Scope | Boundary that opens a new one |
| --- | --- |
| **Transport epoch** | ICE restart — the transport-wide sequence namespace resets |
| **Track** | MID/RID identity from RTP header extensions. Payload type is deliberately ignored because the encoder reuses it across layers. Captures without MID/RID get a synthetic, SSRC-scoped track. |
| **SSRC generation** | SSRC collision, BYE then rebirth, simulcast layer switch, ICE restart |

Each generation gets its own 64-bit *extended sequence* space (the 16-bit
counter unwrapped per generation) so a reused SSRC restarting at sequence 1 is
never confused with the previous flow that also had a sequence 1.

The four boundary kinds are recorded on every generation (`open_reason` /
`close_reason`): `ssrc_collision`, `bye_rebirth`, `layer_switch`,
`ice_restart`.

## Feedback causality is kept multi-candidate

When one feedback item could refer to more than one send event, **all**
candidates are retained. The tool never collapses ambiguity to a single
answer using wall-clock time:

- a NACK for an SSRC reused across N generations produces one candidate per
  generation; generations that only start *after* the feedback packet are
  excluded by capture ordering, not by clock guessing;
- a sequence that was never observed on a generation (a real loss) still gets a
  candidate whose extended sequence is projected from the generation's
  observed anchor;
- overlapping TWCC windows keep every matching send packet, and lookups are
  scoped to the feedback's transport epoch so a post-restart TWCC cannot map
  onto a pre-restart packet that happens to reuse the same transport sequence;
- receiver-report RTT is only emitted when the LSR matches an observed sender
  report; `LSR=0` and unmatched LSR stay explicitly untimed;
- every candidate carries a `cross_epoch` flag and every feedback event a
  status: `resolved`, `ambiguous`, `unresolved` or `cross_epoch`.

## What is stored

Every imported datagram keeps its immutable original capture **index**. The
SQLite schema (see `internal/store/schema.go`) holds, per capture:

- raw packet JSON plus a normalized `rtp_packets` row (SSRC, payload type, raw
  and extended sequence, RTP timestamp, MID/RID, TWCC transport sequence,
  track/generation/epoch assignment);
- RTCP messages with NACK ranges, report blocks and sender info;
- feedback events with their full candidate sets, NACK/TWCC sequence lists and
  RTT candidates;
- sender-report NTP↔RTP mappings;
- freeze intervals (`loss_gap` from extended-sequence jumps, `silence` from
  long quiet gaps) linked to the feedback that references their generation.

## The UI

- **Overview & identity** — tracks, transport epochs and every SSRC generation
  with the boundary that opened/closed it.
- **Freeze intervals** — select a freeze to jump to the feedback that
  triggered it, the sender mapping on either side of the gap, and a separate
  *Still undetermined* list for the references that cannot be settled.
- **Feedback causality** — the section literally titled `Feedback causality`:
  every feedback message, its status, and all candidate send events; a
  candidate row jumps to the originating packet.
- **Packet browser** — per-generation extended sequence and the original
  record for any packet index.

## Bundled sample scenarios

`internal/capture/sample.go` generates a deterministic capture containing:
RTP sequence wraparound (65530→9) with a loss gap, two TWCC messages with
overlapping windows, a late NACK for an SSRC generation that no longer exists,
a PLI storm, a BYE/rebirth, an ICE restart with transport-sequence reset, an
SSRC collision, and receiver reports covering a matched LSR, `LSR=0` and an
unmatched LSR (the low layer and the second media line have no sender report).

## Capture format (JSON Lines)

One datagram per line, in capture order. RTP example:

```json
{"index":0,"time_us":1000000,"direction":"send","kind":"rtp",
 "rtp":{"version":2,"marker":false,"payload_type":96,"sequence":65530,
        "timestamp":1000,"ssrc":286326785},
 "mid":"0","rid":"h","transport_seq":100}
```

RTCP example (NACK):

```json
{"index":12,"time_us":1240000,"direction":"recv","kind":"rtcp",
 "rtcp":[{"fmt":1,"packet_type":205,"ssrc":2576982017,"media_ssrc":286326785,
         "nack":[{"start":4,"end":5}]}]}
```

`packet_type`/`fmt` follow the RTCP numbering (RTPFB=205, PSFB=206, SR=200,
RR=201, BYE=203, APP=204). Generic NACK is `(205,1)`; transport-wide feedback
uses the synthetic `fmt=15` in this format to distinguish it from on-wire
`(205,1)`; PLI is `(206,1)`, FIR `(206,4)`. An APP packet with
`"reason":"ice_restart"` marks an ICE restart boundary.

## Layout

```
cmd/server           HTTP entry point (loopback only)
internal/rtproto     capture model, JSONL decoder, modular sequence math, NACK/TWCC decode
internal/identity    tracks, SSRC generations, transport epochs, boundary detection
internal/analyze     multi-candidate feedback causality, RTT, SR mapping, freeze detection
internal/capture     bundled synthetic sample
internal/store       SQLite schema, import transaction, query DTOs
internal/web         JSON API and the embedded single-page UI
vendor               vendored pure-Go SQLite driver (modernc.org/sqlite), no CGO
```

## Tests

`go test ./... -count=1` covers the four required areas:

- modular 16-bit sequence comparison and unwrapping (`rtproto`);
- NACK expansion and overlapping-set merging (`rtproto`);
- RTT candidates, including `LSR=0` and unmatched LSR (`analyze`);
- layer-switch, collision, BYE/rebirth and ICE-restart generation isolation,
  plus cross-generation/cross-epoch feedback and freeze linkage (`identity`,
  `analyze`, `store`, `web`).
