// Copyright 2026 The mqtt-go authors.
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

// This example shows how receiver-side QoS 2 session state is carried across
// connections. A QoS 2 PUBLISH that was answered with PUBREC but whose PUBREL
// has not arrived yet stays owned by an mqtt.InboundSession. When the
// transport is replaced, that session is handed to the new BaseClient so the
// broker's PUBREL still delivers the reading exactly once and gets PUBCOMP.
//
// With NewReconnectClient the handover below happens automatically inside
// RetryClient.SetClient; this example shows the explicit, manual path.
package main

import (
	"context"
	"fmt"
	"log"
	"time"

	"github.com/at-wat/mqtt-go"
)

func main() {
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()

	dialer := &mqtt.URLDialer{URL: "mqtt://localhost:1883"}

	handler := mqtt.HandlerFunc(func(msg *mqtt.Message) {
		// Delivery of a QoS 2 message happens exactly once, at PUBREL time,
		// even when the connection was replaced between PUBREC and PUBREL.
		fmt.Printf("%s (QoS%d): %s\n", msg.Topic, msg.QoS, msg.Payload)
	})

	var (
		prevCli *mqtt.BaseClient
		session *mqtt.InboundSession
	)
	for attempt := 0; ; attempt++ {
		baseCli, err := dialer.DialContext(ctx)
		if err != nil {
			log.Fatalf("dial: %v", err)
		}

		if session != nil {
			// Continue the same MQTT session on the new connection. Pending
			// QoS 2 handshakes move with the session; the application does
			// not need to remember the original PUBLISH itself.
			baseCli.TransferInboundSession(prevCli)
		}
		baseCli.Handle(handler)

		// CleanSession(false) asks the broker to keep the session. The
		// library only retains receiver-side state when CONNACK also reports
		// SessionPresent=true; a clean start drops stale pending messages.
		sessionPresent, err := baseCli.Connect(ctx, "persistent-device",
			mqtt.WithKeepAlive(30),
			mqtt.WithCleanSession(false),
		)
		if err != nil {
			log.Printf("connect attempt %d failed: %v", attempt, err)
			time.Sleep(time.Second)
			continue
		}
		log.Printf("connected (sessionPresent=%v, pending QoS2=%d)",
			sessionPresent, baseCli.InboundSession().Pending())

		// Keep the owning session and the previous client for the handover.
		session = baseCli.InboundSession()
		prevCli = baseCli

		// Block until the connection ends, then dial again and resume.
		<-baseCli.Done()
		log.Printf("connection closed: %v; reconnecting", baseCli.Err())
	}
}
