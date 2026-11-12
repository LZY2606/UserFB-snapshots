// Copyright 2019 The mqtt-go authors.
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
	"errors"
	"io"
	"sync"
	"time"
)

// ErrNotConnected is returned if a function is called before Connect.
var ErrNotConnected = errors.New("not connected")

// BaseClient is a low layer MQTT client.
// Zero values with valid underlying Transport is a valid BaseClient.
type BaseClient struct {
	// Transport is an underlying connection. Typically net.Conn.
	Transport io.ReadWriteCloser
	// ConnState is called if the connection state is changed.
	ConnState func(ConnState, error)

	// MaxPayloadLen is a maximum allowed length of message payload.
	// 0 means unlimited. (It will panic if exceeds protocol maximum message length (256MB).)
	MaxPayloadLen int

	handler      Handler
	sig          *signaller
	inbound      *InboundSession
	cleanSession bool
	mu           sync.RWMutex
	connState    ConnState
	err          error
	muErr        sync.RWMutex
	connClosed   chan struct{}
	muConnecting sync.RWMutex
	muWrite      sync.Mutex
	idLast       uint32

	muStats sync.RWMutex
	stats   BaseStats
}

// BaseStats stores base client statistics.
type BaseStats struct {
	// Recent ping delay.
	PingDelayRecent time.Duration
	// Maximum ping delay.
	PingDelayMax time.Duration
	// Minimum ping delay.
	PingDelayMin time.Duration
	// Count of ping error.
	CountPingError int
}

func (c *BaseClient) storePingDelay(d time.Duration) {
	c.muStats.Lock()
	c.stats.PingDelayRecent = d
	if c.stats.PingDelayMax < d {
		c.stats.PingDelayMax = d
	}
	if c.stats.PingDelayMin > d || c.stats.PingDelayMin == 0 {
		c.stats.PingDelayMin = d
	}
	c.muStats.Unlock()
}

func (c *BaseClient) storePingError() {
	c.muStats.Lock()
	c.stats.PingDelayRecent = 0
	c.stats.CountPingError++
	c.muStats.Unlock()
}

// Stats returns base client stats.
func (c *BaseClient) Stats() BaseStats {
	c.muStats.RLock()
	defer c.muStats.RUnlock()
	return c.stats
}

// Handle registers the message handler.
func (c *BaseClient) Handle(handler Handler) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.handler = handler
}

// inboundSession returns the session owning receiver-side QoS 2 state,
// creating a new one on first use. Each BaseClient gets its own session
// unless one is explicitly shared via TransferInboundSession.
func (c *BaseClient) inboundSession() *InboundSession {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.inbound == nil {
		c.inbound = &InboundSession{}
	}
	return c.inbound
}

// InboundSession returns the receiver-side MQTT session state used by the
// client. A new empty session is created if none has been assigned yet.
//
// The returned value owns the QoS 2 handshakes started by the broker and is
// the object to hand over to another BaseClient with
// TransferInboundSession when the same MQTT session is continued on a new
// connection. Independent logical clients must not share it.
func (c *BaseClient) InboundSession() *InboundSession {
	return c.inboundSession()
}

// TransferInboundSession hands the receiver-side session state of src over
// to c. Pending QoS 2 messages (PUBLISH already answered with PUBREC but
// not yet released by PUBREL) are then continued on c, even if c uses a
// different transport.
//
// Call it before Connect on the client that takes over the session and
// connect with CleanSession(false). The caller is responsible for stopping
// the serving loop of src before reusing the session; the two clients must
// not process packets concurrently. If src has no session state, c keeps
// its own session.
func (c *BaseClient) TransferInboundSession(src *BaseClient) {
	session := src.InboundSession()
	c.mu.Lock()
	c.inbound = session
	c.mu.Unlock()
}

func (c *BaseClient) write(b []byte) error {
	l := len(b)
	c.muWrite.Lock()
	defer c.muWrite.Unlock()
	for i := 0; i < l; {
		n, err := c.Transport.Write(b[i : l-i])
		if err != nil {
			return err
		}
		i += n
	}
	return nil
}

func (c *BaseClient) signaller() (*signaller, error) {
	c.mu.Lock()
	defer c.mu.Unlock()

	if c.sig == nil {
		return nil, ErrNotConnected
	}
	return c.sig, nil
}

type signaller struct {
	chConnAck  chan *pktConnAck
	chPingResp chan *pktPingResp
	chPubAck   map[uint16]chan *pktPubAck
	chPubRec   map[uint16]chan *pktPubRec
	chPubComp  map[uint16]chan *pktPubComp
	chSubAck   map[uint16]chan *pktSubAck
	chUnsubAck map[uint16]chan *pktUnsubAck
	mu         sync.RWMutex
}

func (s *signaller) ConnAck() chan *pktConnAck {
	s.mu.RLock()
	defer s.mu.RUnlock()

	return s.chConnAck
}
func (s *signaller) PingResp() chan *pktPingResp {
	s.mu.RLock()
	defer s.mu.RUnlock()

	return s.chPingResp
}
func (s *signaller) PubAck(id uint16) (chan *pktPubAck, bool) {
	s.mu.RLock()
	defer s.mu.RUnlock()

	if s.chPubAck == nil {
		return nil, false
	}
	defer delete(s.chPubAck, id)
	ch, ok := s.chPubAck[id]
	return ch, ok
}
func (s *signaller) PubRec(id uint16) (chan *pktPubRec, bool) {
	s.mu.RLock()
	defer s.mu.RUnlock()

	if s.chPubRec == nil {
		return nil, false
	}
	defer delete(s.chPubRec, id)
	ch, ok := s.chPubRec[id]
	return ch, ok
}
func (s *signaller) PubComp(id uint16) (chan *pktPubComp, bool) {
	s.mu.RLock()
	defer s.mu.RUnlock()

	if s.chPubComp == nil {
		return nil, false
	}
	defer delete(s.chPubComp, id)
	ch, ok := s.chPubComp[id]
	return ch, ok
}
func (s *signaller) SubAck(id uint16) (chan *pktSubAck, bool) {
	s.mu.RLock()
	defer s.mu.RUnlock()

	if s.chSubAck == nil {
		return nil, false
	}
	defer delete(s.chSubAck, id)
	ch, ok := s.chSubAck[id]
	return ch, ok
}
func (s *signaller) UnsubAck(id uint16) (chan *pktUnsubAck, bool) {
	s.mu.RLock()
	defer s.mu.RUnlock()

	if s.chUnsubAck == nil {
		return nil, false
	}
	defer delete(s.chUnsubAck, id)
	ch, ok := s.chUnsubAck[id]
	return ch, ok
}
