# mqtt-go

[![Go Reference](https://pkg.go.dev/badge/github.com/at-wat/mqtt-go.svg)](https://pkg.go.dev/github.com/at-wat/mqtt-go) [![ci](https://github.com/at-wat/mqtt-go/actions/workflows/ci.yml/badge.svg)](https://github.com/at-wat/mqtt-go/actions/workflows/ci.yml) [![codecov](https://codecov.io/gh/at-wat/mqtt-go/branch/master/graph/badge.svg)](https://codecov.io/gh/at-wat/mqtt-go) [![License](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](https://opensource.org/licenses/Apache-2.0)

## Yet another Go MQTT 3.1.1 client library

- Go-ish interface
  > Fully context controlled and mockable interface.
- Extensible
  > Easy to implement a wrapper with unified interface. e.g. AWS IoT WebSocket dialer with automatic presign URL updater is available: [AWS IoT Device SDK for Go](https://github.com/seqsense/aws-iot-device-sdk-go)
- Thread-safe
  > All functions and structs are safe to be used from multiple goroutines.

## Migration guide

- [v0.14.0](MIGRATION.md#v0140)
- [v0.12.0](MIGRATION.md#v0120)

## Examples

- [MQTTs with client certificate](examples/mqtts-client-cert)
- [WebSockets with presigned URL](examples/wss-presign-url)
- [Inbound session handover across connections](examples/inbound-session)

## Inbound QoS 2 session state

A QoS 2 delivery started by the broker is a four-step handshake
(`PUBLISH -> PUBREC -> PUBREL -> PUBCOMP`). The client stores the full
message when the first `PUBLISH` arrives, answers `PUBREC`, and only delivers
the message to the handler when `PUBREL` arrives (answering `PUBCOMP`).

That receiver-side state is owned by an [`InboundSession`](https://pkg.go.dev/github.com/at-wat/mqtt-go#InboundSession),
not by the serving goroutine, so an in-flight handshake survives replacing
the underlying transport:

- Each `BaseClient` owns its own `InboundSession` by default; independent
  logical clients never share packet-identifier state and there is no
  package-global message table.
- `ReconnectClient`/`RetryClient` propagate the session to every replacement
  `BaseClient` automatically in `SetClient`. The application does not have to
  remember the original `PUBLISH`.
- For manually managed clients, hand the session over explicitly before
  connecting the new client:
  ```go
  newCli.TransferInboundSession(oldCli)
  // then Connect with WithCleanSession(false)
  ```
- Pending messages are kept only when `CleanSession=false` **and** the broker
  answers with `SessionPresent=true`. A clean session, or an explicit
  `SessionPresent=false`, discards pending messages. The verdict is applied
  while processing the `CONNACK`, before any later packet (including a
  `PUBREL` pipelined into the same read) is handled. A rejected `CONNACK`
  never clears the state.
- State is kept in memory only; it is not persisted across process restarts.

## Reference

- [MQTT Version 3.1.1 Plus Errata 01](http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/mqtt-v3.1.1.html)

## License

This package is licensed under [Apache License Version 2.0](./LICENSE).
