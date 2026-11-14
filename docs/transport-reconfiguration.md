# In-place RTSP transport reconfiguration (`enableTransportReconfiguration`)

## What it does

`RTSPServer::enableTransportReconfiguration(Boolean)` adds an opt-in path for re-issuing
`SETUP` on a track that is already set up. With the feature **disabled** (the default), nothing
changes: a second `SETUP` still runs the historical `pauseStream()` → `deleteStream()` →
`getStreamParameters()` sequence, which tears down and rebuilds the media source, RTP sink and
RTCP instance.

With it **enabled** and the track served by a plain, unicast
`OnDemandServerMediaSubsession`, a re-`SETUP` instead moves the *existing* stream's single
destination in place.

```cpp
RTSPServer* rtspServer = RTSPServer::createNew(*env, 8554, authDB);
rtspServer->enableTransportReconfiguration(True); // before accepting clients
```

### What is preserved

Because the `FramedSource`, `RTPSink` and `RTCPInstance` are never recreated:

- the RTP **SSRC** is unchanged;
- RTP **sequence numbers** continue from the next value (no jump/reset);
- RTP **timestamps** keep advancing on the same clock;
- the **RTCP** instance and its accumulated sender/transmission statistics keep accumulating;
- a playing track keeps sending without another `PLAY`; a paused track stays paused.

A second `PLAY` is **not** part of the switch. After a successful re-`SETUP`, media and the
periodic RTCP reports go to the new destination, and the old destination stops receiving both.

## Accepted transports and validation

The reconfiguration path accepts only the two complete, explicit unicast transports:

- **RTP/AVP UDP** — `Transport: RTP/AVP;unicast;client_port=<rtp>-<rtcp>` where the two ports
  are distinct, non-zero 16-bit values;
- **RTP/AVP/TCP** — `Transport: RTP/AVP/TCP;unicast;interleaved=<rtp>-<rtcp>` with an explicit
  pair of distinct channel ids in `0..255`.

The requested TCP channels must not collide with a channel already assigned to another
established track on the same RTSP control connection.

Anything else — missing/implicit `interleaving`, equal ports or channels, zero UDP ports,
`RAW_UDP`, multicast, an unknown transport — is rejected with **`461 Unsupported Transport`**
and the existing transport is left fully working. Re-sending the identical `SETUP` is a no-op:
no duplicate destination and no duplicate RR callback are installed.

## Scope / limitations

- **Opt-in, set before serving clients.** It defaults to off so existing deployments are
  unaffected.
- **Plaintext, non-tunnelled connections only.** TLS servers and RTSP-over-HTTP tunnels keep the
  legacy path (the code requires the RTSP input and output socket to be the same and rejects when
  the server uses TLS).
- **Unicast `OnDemandServerMediaSubsession` only.** The base
  `ServerMediaSubsession::supportsTransportReconfiguration()` returns `False`. Specialized
  subclasses can opt out with the protected
  `OnDemandServerMediaSubsession::setTransportReconfigurationEnabled(False)`. The bundled
  back-end `ProxyServerMediaSubsession` opts out because its transport is tied to a separate
  upstream client connection.
- **Same control connection.** Reconfiguration works only over the RTSP connection that
  established the session; cross-connection migration is intentionally not implemented.
- **Per-track, per-subscriber isolation.** Each track carries its own destination. With
  `reuseFirstSource`, one subscriber's re-`SETUP` changes only that subscriber's destination and
  RTCP RR handler; the shared source and every other subscriber are not paused, recreated or
  moved.
- **RR handler migration.** The per-destination RTCP RR callback (which drives session liveness)
  is detached from the old destination and re-registered on the new one. An RR arriving on the
  old destination no longer refreshes the track; an RR on the new destination does.
- **Cleanup.** `TEARDOWN` and an abrupt control-connection disconnect remove the interleaved
  channels and RR callbacks through the same `endPlaying()`/`stopTCPStreamingOnSocket()` paths as
  a normal teardown, leaving no stale channel registration or dangling callback.

## Code layout

| File | Responsibility |
| --- | --- |
| `liveMedia/TransportReconfiguration.{cpp,hh}` | Stateless UDP-port / TCP-channel validation rules. |
| `liveMedia/RTSPServer.{cpp,hh}` | The `enableTransportReconfiguration()` switch, re-`SETUP` gating, 461 validation, channel-collision checks, RTSP socket/track bookkeeping and the success response. |
| `liveMedia/OnDemandServerMediaSubsession.{cpp,hh}` | `reconfigureStreamTransport()` and the `StreamState` destination attach/detach/switch machinery; per-destination handler storage. |
| `liveMedia/ServerMediaSession.{cpp,hh}` | The base-class virtual entry points (default: unsupported). |
| `liveMedia/RTPInterface.{cpp,hh}` | Re-arms datagram-socket reading after the last interleaved TCP channel is removed (TCP→UDP). |

## Tests

`task_tests/transport_switch.cpp` drives a real loopback `RTSPServer` with hand-written RTSP
requests and real TCP-interleaved/UDP packets, across three groups:

- `migration` — UDP→TCP→UDP continuity (SSRC, sequence, timestamp), and RR-handler migration;
- `isolation` — independent per-track destinations, channel-collision 461, and
  `reuseFirstSource` subscriber isolation;
- `lifecycle` — 461 validation, paused-track behavior, identical re-`SETUP` no-op, the
  feature-disabled legacy recreate path, and TEARDOWN/disconnect cleanup.

The media is a manually-advanced (`GatedFramedSource`) source, so completion is decided from
observed packets and counters rather than wall-clock sleeps.

> The integration tests open real loopback `AF_INET` sockets (the server `listen()`s and the
> clients `bind()` UDP ports). They must run where local port binding is permitted. In a sandbox
> that denies binding, `RTSPServer::createNew()` returns `NULL` and the cases report the missing
> capability rather than running; enable local binding (e.g.
> `sandbox.network.allowLocalBinding: true`) to execute them. The validation rules themselves also
> have a binding-free unit check.

## Example

See `testProgs/testTransportReconfiguration.cpp` for a small standalone server.
