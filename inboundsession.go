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

package mqtt

import (
	"bytes"
	"errors"
	"fmt"
	"sync"
)

// ErrInboundSessionConflict means that a retransmitted QoS 2 PUBLISH packet
// carries the same packet identifier as a PUBLISH still waiting for its
// PUBREL, but its topic or payload do not match the original packet.
// The original message is kept untouched and the connection is closed.
var ErrInboundSessionConflict = errors.New("conflicting QoS 2 PUBLISH retransmission")

// InboundSessionConflictError is returned by the serving loop when a
// retransmitted QoS 2 PUBLISH conflicts with the pending message stored
// under the same packet identifier.
type InboundSessionConflictError struct {
	// ID is the conflicting packet identifier.
	ID uint16
}

func (e *InboundSessionConflictError) Error() string {
	return fmt.Sprintf("mqtt: %s: packet ID %d", ErrInboundSessionConflict.Error(), e.ID)
}

// Unwrap returns ErrInboundSessionConflict.
func (e *InboundSessionConflictError) Unwrap() error {
	return ErrInboundSessionConflict
}

// InboundSession stores the receiver-side state of an MQTT session.
//
// It owns QoS 2 delivery handshakes started by the broker
// (PUBLISH -> PUBREC -> PUBREL -> PUBCOMP) so that the handshake can
// continue after the underlying transport is replaced. Messages are kept
// between the first PUBLISH (answered by PUBREC) and the matching PUBREL,
// which is the only point where the message is delivered to the handler.
//
// The zero value is a valid empty session. A single InboundSession must be
// shared by the BaseClients that continue the same MQTT session, while
// independent logical clients must use separate values. The library never
// keeps packet-identifier state in package-global tables.
//
// RetryClient (and therefore ReconnectClient) propagates the session to
// replacement BaseClients automatically. For manually managed BaseClients,
// use BaseClient.TransferInboundSession before connecting the new client.
//
// InboundSession is safe for concurrent use. Its state lives in memory only:
// it is not persisted to disk and is lost when the process exits.
type InboundSession struct {
	mu      sync.Mutex
	pending map[uint16]*Message
}

// store keeps a QoS 2 PUBLISH message until its PUBREL arrives.
//
// The message is copied so later modifications of the parsed packet never
// affect the protocol record. A retransmission with the same identifier,
// topic and payload is accepted without overwriting the record; a different
// topic or payload is reported as InboundSessionConflictError and the
// original record is kept.
func (s *InboundSession) store(msg *Message) error {
	s.mu.Lock()
	defer s.mu.Unlock()

	if s.pending == nil {
		s.pending = make(map[uint16]*Message)
	}
	if stored, ok := s.pending[msg.ID]; ok {
		if stored.Topic != msg.Topic || !bytes.Equal(stored.Payload, msg.Payload) {
			return &InboundSessionConflictError{ID: msg.ID}
		}
		// Retransmission of the same message: re-send PUBREC, keep the record.
		return nil
	}
	s.pending[msg.ID] = msg.clone()
	return nil
}

// take removes and returns the message stored under id.
// It returns nil if no PUBLISH with the identifier is pending, which happens
// for unknown or duplicate PUBREL packets.
func (s *InboundSession) take(id uint16) *Message {
	s.mu.Lock()
	defer s.mu.Unlock()

	msg, ok := s.pending[id]
	if !ok {
		return nil
	}
	delete(s.pending, id)
	return msg
}

// reset drops every pending QoS 2 message.
// It is used when a newly established connection starts a clean session,
// either because CleanSession was requested or the broker answered
// SessionPresent=false.
func (s *InboundSession) reset() {
	s.mu.Lock()
	defer s.mu.Unlock()

	s.pending = nil
}

// Reset drops all pending QoS 2 messages held by the session.
// Applications normally do not need to call it: the client resets the
// session automatically while processing a clean-session CONNACK, before any
// later packet of the same connection is handled.
func (s *InboundSession) Reset() {
	s.reset()
}

// Pending returns the number of QoS 2 messages waiting for PUBREL.
func (s *InboundSession) Pending() int {
	s.mu.Lock()
	defer s.mu.Unlock()

	return len(s.pending)
}
