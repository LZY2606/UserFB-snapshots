// Copyright 2026 The GoSNMP Authors. All rights reserved.  Use of this
// source code is governed by a BSD-style license that can be found in the
// LICENSE file.

//go:build all || trap

package gosnmp

import (
	"encoding/binary"
	"io"
	"log"
	"net"
	"strconv"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/stretchr/testify/require"
)

// The tests below exercise the TrapListener authoritative engine mode
// (SetAuthoritativeEngine) over real loopback UDP with real BER: engine
// discovery, timeliness enforcement and privacy (salt) handling for
// SNMPv3 Inform reception.

const (
	authorityEngineID  = "\x80\x00\x1f\x88\x04collector"
	authorityEngineID2 = "\x80\x00\x1f\x88\x04listenerB"
	authorityRemoteID  = "\x80\x00\x1f\x88\x04remote"
	authorityTestUser  = "informUser"
	authorityAuthPass  = "informAuthPass1"
	authorityPrivPass  = "informPrivPass1"
	authorityTimeout   = 2 * time.Second
)

func authorityLogger() Logger {
	return NewLogger(log.New(io.Discard, "", 0))
}

// authorityCreds returns the shared informUser credentials (SHA/AES128)
// without an engine ID; listener tables and device senders localize them
// per engine.
func authorityCreds() *UsmSecurityParameters {
	return &UsmSecurityParameters{
		UserName:                 authorityTestUser,
		AuthenticationProtocol:   SHA,
		AuthenticationPassphrase: authorityAuthPass,
		PrivacyProtocol:          AES,
		PrivacyPassphrase:        authorityPrivPass,
		Logger:                   authorityLogger(),
	}
}

// authorityDevice returns device-side credentials localized to engineID
// with the given engine boots/time.
func authorityDevice(engineID string, boots, tim uint32) *UsmSecurityParameters {
	sp := authorityCreds()
	sp.AuthoritativeEngineID = engineID
	sp.AuthoritativeEngineBoots = boots
	sp.AuthoritativeEngineTime = tim
	return sp
}

var authorityDeviceSaltCounter uint64

func authorityDeviceSalt() []byte {
	salt := make([]byte, 8)
	binary.BigEndian.PutUint64(salt, atomic.AddUint64(&authorityDeviceSaltCounter, 1))
	return salt
}

// marshalAuthorityMessage builds a real BER message the way a device
// would, using the library's own marshal/encrypt/authenticate path.
func marshalAuthorityMessage(t *testing.T, sp *UsmSecurityParameters, flags SnmpV3MsgFlags, pdu PDUType, msgID, reqID uint32, vars []SnmpPDU) []byte {
	t.Helper()
	require.NoError(t, sp.InitSecurityKeys())
	if flags&AuthPriv > AuthNoPriv {
		sp.PrivacyParameters = authorityDeviceSalt()
	}
	pkt := &SnmpPacket{
		Version:            Version3,
		MsgFlags:           flags,
		SecurityModel:      UserSecurityModel,
		SecurityParameters: sp,
		PDUType:            pdu,
		MsgMaxSize:         65507,
		MsgID:              msgID,
		RequestID:          reqID,
		ContextEngineID:    sp.AuthoritativeEngineID,
		Variables:          vars,
		Logger:             authorityLogger(),
	}
	raw, err := pkt.MarshalMsg()
	require.NoError(t, err)
	return raw
}

func authorityInformVars() []SnmpPDU {
	return []SnmpPDU{{Name: trapTestOid, Type: OctetString, Value: trapTestPayload}}
}

// authorityDiscoveryProbe builds a blank RFC3414 section 4 discovery
// request with the given identifiers.
func authorityDiscoveryProbe(t *testing.T, msgID, reqID uint32) []byte {
	t.Helper()
	probe := (&UsmSecurityParameters{Logger: authorityLogger()}).discoveryRequired()
	probe.MsgID, probe.RequestID = msgID, reqID
	raw, err := probe.MarshalMsg()
	require.NoError(t, err)
	return raw
}

// decodeAuthorityResponse parses a listener response, verifying its MAC
// and decrypting it with the given credentials.
func decodeAuthorityResponse(t *testing.T, raw []byte, sp *UsmSecurityParameters) *SnmpPacket {
	t.Helper()
	gs := newTestGoSNMPv3(AuthPriv, sp)
	pkt, err := gs.UnmarshalTrap(raw, true)
	require.NoError(t, err)
	return pkt
}

func decodeNoAuthReport(t *testing.T, raw []byte) *SnmpPacket {
	t.Helper()
	return decodeAuthorityResponse(t, raw, &UsmSecurityParameters{
		AuthenticationProtocol: NoAuth,
		PrivacyProtocol:        NoPriv,
		Logger:                 authorityLogger(),
	})
}

// authorityClock is a controllable engine clock for timeliness, wrap and
// saturation tests, so they do not wait for real time to pass.
type authorityClock struct {
	mu     sync.Mutex
	offset time.Duration
}

func (c *authorityClock) install(t *testing.T, tl *TrapListener) {
	t.Helper()
	base := time.Now()
	tl.authoritativeMu.Lock()
	defer tl.authoritativeMu.Unlock()
	require.NotNil(t, tl.authoritative)
	tl.authoritative.nowFn = func() time.Time {
		c.mu.Lock()
		defer c.mu.Unlock()
		return base.Add(c.offset)
	}
}

func (c *authorityClock) set(offset time.Duration) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.offset = offset
}

// startAuthorityListener creates an authoritative-mode listener on an
// ephemeral UDP port and returns it together with the delivery channel.
func startAuthorityListener(t *testing.T, params *GoSNMP, engineID string, boots uint32, clock *authorityClock) (*TrapListener, chan *SnmpPacket) {
	t.Helper()
	tl := NewTrapListener()
	tl.Params = params
	require.NoError(t, tl.SetAuthoritativeEngine(engineID, boots))
	if clock != nil {
		clock.install(t, tl)
	}
	received := make(chan *SnmpPacket, 16)
	tl.OnNewTrap = func(p *SnmpPacket, _ *net.UDPAddr) { received <- p }
	listenOnEphemeral(t, tl)
	t.Cleanup(tl.Close)
	return tl, received
}

func dialAuthority(t *testing.T, tl *TrapListener) net.Conn {
	t.Helper()
	udpAddr := tl.conn.LocalAddr().(*net.UDPAddr)
	conn, err := net.Dial("udp", net.JoinHostPort(udpAddr.IP.String(), strconv.Itoa(udpAddr.Port)))
	require.NoError(t, err)
	require.NoError(t, conn.SetDeadline(time.Now().Add(authorityTimeout)))
	t.Cleanup(func() { conn.Close() })
	return conn
}

func authorityRoundTrip(t *testing.T, conn net.Conn, raw []byte) []byte {
	t.Helper()
	_, err := conn.Write(raw)
	require.NoError(t, err)
	buf := make([]byte, 65535)
	n, err := conn.Read(buf)
	require.NoError(t, err)
	return buf[:n]
}

// authorityExpectSilence sends raw (unless nil) and requires that no
// response arrives, then restores the default deadline.
func authorityExpectSilence(t *testing.T, conn net.Conn, raw []byte) {
	t.Helper()
	if raw != nil {
		_, err := conn.Write(raw)
		require.NoError(t, err)
	}
	require.NoError(t, conn.SetReadDeadline(time.Now().Add(300*time.Millisecond)))
	_, err := conn.Read(make([]byte, 65535))
	require.Error(t, err, "expected no response")
	require.NoError(t, conn.SetReadDeadline(time.Now().Add(authorityTimeout)))
}

func requireTrap(t *testing.T, received chan *SnmpPacket) *SnmpPacket {
	t.Helper()
	select {
	case p := <-received:
		return p
	case <-time.After(authorityTimeout):
		t.Fatal("timed out waiting for notification")
		return nil
	}
}

func requireNoTrap(t *testing.T, received chan *SnmpPacket) {
	t.Helper()
	select {
	case p := <-received:
		t.Fatalf("unexpected notification delivered: %v", p.PDUType)
	default:
	}
}

// sendAuthorityInform sends one authPriv inform from a device at the
// given engine boots/time and returns the raw response.
func sendAuthorityInform(t *testing.T, conn net.Conn, engineID string, boots, tim, msgID, reqID uint32) []byte {
	t.Helper()
	device := authorityDevice(engineID, boots, tim)
	raw := marshalAuthorityMessage(t, device, AuthPriv|Reportable, InformRequest, msgID, reqID, authorityInformVars())
	return authorityRoundTrip(t, conn, raw)
}

func authorityTableParams(t *testing.T, entries ...*UsmSecurityParameters) *GoSNMP {
	t.Helper()
	gs := newTestGoSNMP()
	gs.Version = Version3
	gs.SecurityModel = UserSecurityModel
	table := NewSnmpV3SecurityParametersTable(authorityLogger())
	for _, e := range entries {
		require.NoError(t, table.Add(e.UserName, e))
	}
	gs.SetTrapSecurityParametersTable(table)
	return gs
}

func requireReportCounter(t *testing.T, report *SnmpPacket, oid string, want uint) {
	t.Helper()
	require.Equal(t, Report, report.PDUType)
	require.Len(t, report.Variables, 1)
	require.Equal(t, oid, report.Variables[0].Name)
	require.Equal(t, Counter32, report.Variables[0].Type)
	require.Equal(t, want, report.Variables[0].Value)
}

func TestInformAuthorityDiscovery(t *testing.T) {
	// The full device flow through the Listen entry point with a security
	// parameters table configured: discovery, time synchronization, then
	// a normal Inform.
	t.Run("discovery_sync_inform_flow", func(t *testing.T) {
		clock := &authorityClock{}
		tl, received := startAuthorityListener(t, authorityTableParams(t, authorityCreds()), authorityEngineID, 7, clock)
		conn := dialAuthority(t, tl)

		// 1. Engine discovery is answered even with a table configured,
		// and never delivered to OnNewTrap.
		raw := authorityRoundTrip(t, conn, authorityDiscoveryProbe(t, 101, 202))
		report := decodeNoAuthReport(t, raw)
		requireReportCounter(t, report, usmStatsUnknownEngineIDs, 1)
		require.Zero(t, report.MsgFlags&Reportable)
		require.Equal(t, uint32(101), report.MsgID)
		require.Equal(t, uint32(202), report.RequestID)
		rsp := report.SecurityParameters.(*UsmSecurityParameters)
		require.Equal(t, authorityEngineID, rsp.AuthoritativeEngineID)
		require.Equal(t, uint32(7), rsp.AuthoritativeEngineBoots)
		require.Equal(t, uint32(0), rsp.AuthoritativeEngineTime)
		requireNoTrap(t, received)

		// The counter is cumulative.
		report = decodeNoAuthReport(t, authorityRoundTrip(t, conn, authorityDiscoveryProbe(t, 102, 203)))
		requireReportCounter(t, report, usmStatsUnknownEngineIDs, 2)

		// 2. A time-sync probe (authenticated empty GetRequest) with a
		// stale clock gets an authNoPriv notInTimeWindows Report carrying
		// the local clock; the request ID is not yet known, so it is 0.
		stale := authorityDevice(authorityEngineID, 7, 900)
		raw = authorityRoundTrip(t, conn, marshalAuthorityMessage(t, stale, AuthNoPriv|Reportable, GetRequest, 103, 0, nil))
		syncReport := decodeAuthorityResponse(t, raw, authorityCreds())
		requireReportCounter(t, syncReport, usmStatsNotInTimeWindows, 1)
		require.Equal(t, AuthNoPriv, syncReport.MsgFlags&AuthPriv)
		require.Equal(t, uint32(103), syncReport.MsgID)
		require.Equal(t, uint32(0), syncReport.RequestID)
		srsp := syncReport.SecurityParameters.(*UsmSecurityParameters)
		require.Equal(t, authorityTestUser, srsp.UserName)
		require.Equal(t, uint32(7), srsp.AuthoritativeEngineBoots)
		require.Equal(t, uint32(0), srsp.AuthoritativeEngineTime)
		requireNoTrap(t, received)

		// 3. The same probe with a correct clock is silently dropped.
		syncOK := authorityDevice(authorityEngineID, 7, 0)
		authorityExpectSilence(t, conn, marshalAuthorityMessage(t, syncOK, AuthNoPriv|Reportable, GetRequest, 104, 0, nil))
		requireNoTrap(t, received)

		// 4. A proper Inform is delivered once and confirmed with a
		// GetResponse carrying the local engine ID and clock.
		device := authorityDevice(authorityEngineID, 7, 0)
		raw = authorityRoundTrip(t, conn, marshalAuthorityMessage(t, device, AuthPriv|Reportable, InformRequest, 105, 303, authorityInformVars()))
		requestSalt := device.PrivacyParameters
		response := decodeAuthorityResponse(t, raw, authorityCreds())
		require.Equal(t, GetResponse, response.PDUType)
		require.Equal(t, uint32(105), response.MsgID)
		require.Equal(t, uint32(303), response.RequestID)
		require.Equal(t, NoError, response.Error)
		require.Zero(t, response.ErrorIndex)
		require.Equal(t, AuthPriv, response.MsgFlags&AuthPriv)
		require.Zero(t, response.MsgFlags&Reportable)
		require.Equal(t, authorityEngineID, response.ContextEngineID)
		require.Len(t, response.Variables, 1)
		require.Equal(t, trapTestOid, response.Variables[0].Name)
		require.Equal(t, trapTestPayload, string(response.Variables[0].Value.([]byte)))
		ersp := response.SecurityParameters.(*UsmSecurityParameters)
		require.Equal(t, authorityEngineID, ersp.AuthoritativeEngineID)
		require.Equal(t, uint32(7), ersp.AuthoritativeEngineBoots)
		require.Equal(t, uint32(0), ersp.AuthoritativeEngineTime)
		require.NotEqual(t, requestSalt, ersp.PrivacyParameters)
		delivered := requireTrap(t, received)
		require.Equal(t, InformRequest, delivered.PDUType)
		require.Equal(t, uint32(303), delivered.RequestID)
	})

	// Requests that only look like discovery must not enter the
	// unauthenticated path.
	t.Run("invalid_discovery_shapes", func(t *testing.T) {
		clock := &authorityClock{}
		tl, received := startAuthorityListener(t, authorityTableParams(t, authorityCreds()), authorityEngineID, 7, clock)
		conn := dialAuthority(t, tl)
		blank := &UsmSecurityParameters{Logger: authorityLogger()}

		// A discovery-shaped GetRequest with varbinds gets no response.
		authorityExpectSilence(t, conn, marshalAuthorityMessage(t, blank, Reportable|NoAuthNoPriv, GetRequest, 201, 1, authorityInformVars()))
		// A notification disguised as a discovery request gets no
		// response and is not delivered.
		authorityExpectSilence(t, conn, marshalAuthorityMessage(t, blank, Reportable|NoAuthNoPriv, InformRequest, 202, 2, authorityInformVars()))
		requireNoTrap(t, received)
	})

	t.Run("config_validation", func(t *testing.T) {
		tl := NewTrapListener()
		defer tl.Close()
		for _, id := range []string{"", "1234", string(make([]byte, 33))} {
			require.Error(t, tl.SetAuthoritativeEngine(id, 7), "engine ID %q", id)
		}
		require.Error(t, tl.SetAuthoritativeEngine(authorityEngineID, 0))
		require.Error(t, tl.SetAuthoritativeEngine(authorityEngineID, authoritativeMaxBoots))
		require.NoError(t, tl.SetAuthoritativeEngine(authorityEngineID, 7))
		// An invalid reconfiguration keeps the previous config.
		require.Error(t, tl.SetAuthoritativeEngine("bad", 7))
		require.Equal(t, authorityEngineID, tl.authoritative.engineID)
		require.Equal(t, uint32(7), tl.authoritative.boots)
		require.NoError(t, tl.SetAuthoritativeEngine(authorityEngineID, authoritativeMaxBoots-1))

		// Once listening, the engine can no longer be reconfigured.
		tl.Params = newTestGoSNMP()
		tl.OnNewTrap = func(*SnmpPacket, *net.UDPAddr) {}
		listenOnEphemeral(t, tl)
		require.Error(t, tl.SetAuthoritativeEngine(authorityEngineID, 10))
	})

	// Without SetAuthoritativeEngine the legacy behavior is unchanged: a
	// discovery probe to a table-only listener gets no response.
	t.Run("mode_disabled_keeps_legacy_behavior", func(t *testing.T) {
		tl := NewTrapListener()
		tl.Params = authorityTableParams(t, authorityCreds())
		received := make(chan *SnmpPacket, 1)
		tl.OnNewTrap = func(p *SnmpPacket, _ *net.UDPAddr) { received <- p }
		listenOnEphemeral(t, tl)
		t.Cleanup(tl.Close)
		conn := dialAuthority(t, tl)

		authorityExpectSilence(t, conn, authorityDiscoveryProbe(t, 301, 302))
		requireNoTrap(t, received)
	})

	// Messages for a remote authoritative engine: traps are collected via
	// the legacy path, Informs are dropped rather than confirmed.
	t.Run("remote_engine", func(t *testing.T) {
		clock := &authorityClock{}
		tl, received := startAuthorityListener(t, authorityTableParams(t, authorityCreds()), authorityEngineID, 7, clock)
		conn := dialAuthority(t, tl)

		// A remote Inform is dropped: no callback, no response.
		device := authorityDevice(authorityRemoteID, 3, 50)
		authorityExpectSilence(t, conn, marshalAuthorityMessage(t, device, AuthPriv|Reportable, InformRequest, 401, 404, authorityInformVars()))
		requireNoTrap(t, received)

		// A remote trap is collected via the legacy path and not answered.
		trapDevice := authorityDevice(authorityRemoteID, 3, 50)
		raw := marshalAuthorityMessage(t, trapDevice, AuthPriv, SNMPv2Trap, 402, 405, authorityInformVars())
		_, err := conn.Write(raw)
		require.NoError(t, err)
		delivered := requireTrap(t, received)
		require.Equal(t, SNMPv2Trap, delivered.PDUType)
		require.Equal(t, uint32(405), delivered.RequestID)
		authorityExpectSilence(t, conn, nil)
	})
}

func TestInformAuthorityTimeliness(t *testing.T) {
	singleUserParams := func() *GoSNMP {
		return newTestGoSNMPv3(AuthPriv, authorityCreds())
	}

	// The RFC3414 window is ±150 seconds around the local engine time;
	// the boundary is accepted, one second beyond is rejected.
	t.Run("window_edges", func(t *testing.T) {
		clock := &authorityClock{}
		tl, received := startAuthorityListener(t, singleUserParams(), authorityEngineID, 7, clock)
		conn := dialAuthority(t, tl)
		clock.set(1000 * time.Second)

		for i, tim := range []uint32{850, 1150} {
			raw := sendAuthorityInform(t, conn, authorityEngineID, 7, tim, uint32(10+i), uint32(100+i))
			response := decodeAuthorityResponse(t, raw, authorityCreds())
			require.Equal(t, GetResponse, response.PDUType, "time %d", tim)
			require.Equal(t, uint32(100+i), requireTrap(t, received).RequestID)
		}
		for i, tim := range []uint32{849, 1151} {
			raw := sendAuthorityInform(t, conn, authorityEngineID, 7, tim, uint32(20+i), uint32(200+i))
			report := decodeAuthorityResponse(t, raw, authorityCreds())
			requireReportCounter(t, report, usmStatsNotInTimeWindows, uint(i+1))
			require.Equal(t, uint32(0), report.RequestID)
		}
		requireNoTrap(t, received)
		require.Equal(t, uint32(2), atomic.LoadUint32(&tl.usmStatsNotInTimeWindowsCount))
	})

	// The boots value must match the local one exactly.
	t.Run("boots_mismatch", func(t *testing.T) {
		clock := &authorityClock{}
		tl, received := startAuthorityListener(t, singleUserParams(), authorityEngineID, 7, clock)
		conn := dialAuthority(t, tl)
		clock.set(1000 * time.Second)

		report := decodeAuthorityResponse(t, sendAuthorityInform(t, conn, authorityEngineID, 8, 1000, 30, 300), authorityCreds())
		requireReportCounter(t, report, usmStatsNotInTimeWindows, 1)
		rsp := report.SecurityParameters.(*UsmSecurityParameters)
		require.Equal(t, uint32(7), rsp.AuthoritativeEngineBoots)
		require.Equal(t, uint32(1000), rsp.AuthoritativeEngineTime)
		requireNoTrap(t, received)
		require.Equal(t, uint32(1), atomic.LoadUint32(&tl.usmStatsNotInTimeWindowsCount))
	})

	// A bad digest is dropped before the timeliness check: no response,
	// no delivery, no counter, and the clock is untouched.
	t.Run("bad_digest", func(t *testing.T) {
		clock := &authorityClock{}
		tl, received := startAuthorityListener(t, singleUserParams(), authorityEngineID, 7, clock)
		conn := dialAuthority(t, tl)
		clock.set(1000 * time.Second)

		device := authorityDevice(authorityEngineID, 7, 1000)
		device.AuthenticationPassphrase = "wrongAuthPass1"
		raw := marshalAuthorityMessage(t, device, AuthPriv|Reportable, InformRequest, 40, 400, authorityInformVars())
		beforeBoots, beforeTime := tl.authoritative.clock()
		authorityExpectSilence(t, conn, raw)
		requireNoTrap(t, received)
		require.Equal(t, uint32(0), atomic.LoadUint32(&tl.usmStatsNotInTimeWindowsCount))
		afterBoots, afterTime := tl.authoritative.clock()
		require.Equal(t, beforeBoots, afterBoots)
		require.Equal(t, beforeTime, afterTime)
	})

	// An unknown user gets no response and is not delivered.
	t.Run("unknown_user", func(t *testing.T) {
		clock := &authorityClock{}
		tl, received := startAuthorityListener(t, singleUserParams(), authorityEngineID, 7, clock)
		conn := dialAuthority(t, tl)
		clock.set(1000 * time.Second)

		device := authorityDevice(authorityEngineID, 7, 1000)
		device.UserName = "nobody"
		raw := marshalAuthorityMessage(t, device, AuthPriv|Reportable, InformRequest, 41, 401, authorityInformVars())
		authorityExpectSilence(t, conn, raw)
		requireNoTrap(t, received)
		require.Equal(t, uint32(0), atomic.LoadUint32(&tl.usmStatsNotInTimeWindowsCount))
	})

	// A message that authenticates but whose body cannot be parsed is
	// dropped without a response or delivery.
	t.Run("corrupted_body", func(t *testing.T) {
		clock := &authorityClock{}
		tl, received := startAuthorityListener(t, singleUserParams(), authorityEngineID, 7, clock)
		conn := dialAuthority(t, tl)
		clock.set(1000 * time.Second)

		device := authorityDevice(authorityEngineID, 7, 1000)
		// 0xab is not a known PDU type, so the (correctly signed and
		// encrypted) payload fails to parse after decryption.
		raw := marshalAuthorityMessage(t, device, AuthPriv|Reportable, PDUType(0xab), 42, 402, authorityInformVars())
		authorityExpectSilence(t, conn, raw)
		requireNoTrap(t, received)
		require.Equal(t, uint32(0), atomic.LoadUint32(&tl.usmStatsNotInTimeWindowsCount))
	})

	// Duplicate Informs inside the window are delivered and answered
	// again; the listener does no business-level deduplication.
	t.Run("duplicate_inform_redelivered", func(t *testing.T) {
		clock := &authorityClock{}
		tl, received := startAuthorityListener(t, singleUserParams(), authorityEngineID, 7, clock)
		conn := dialAuthority(t, tl)
		clock.set(1000 * time.Second)

		device := authorityDevice(authorityEngineID, 7, 1000)
		raw := marshalAuthorityMessage(t, device, AuthPriv|Reportable, InformRequest, 50, 500, authorityInformVars())
		for range 2 {
			response := decodeAuthorityResponse(t, authorityRoundTrip(t, conn, raw), authorityCreds())
			require.Equal(t, GetResponse, response.PDUType)
			require.Equal(t, uint32(500), response.RequestID)
			require.Equal(t, uint32(500), requireTrap(t, received).RequestID)
		}
	})

	// When the seconds value passes 2^31-1 it wraps to zero and boots is
	// incremented; messages must carry the new tuple.
	t.Run("engine_time_wrap", func(t *testing.T) {
		clock := &authorityClock{}
		tl, received := startAuthorityListener(t, singleUserParams(), authorityEngineID, 7, clock)
		conn := dialAuthority(t, tl)
		clock.set((1<<31 + 42) * time.Second)

		response := decodeAuthorityResponse(t, sendAuthorityInform(t, conn, authorityEngineID, 8, 42, 60, 600), authorityCreds())
		require.Equal(t, GetResponse, response.PDUType)
		rsp := response.SecurityParameters.(*UsmSecurityParameters)
		require.Equal(t, uint32(8), rsp.AuthoritativeEngineBoots)
		require.Equal(t, uint32(42), rsp.AuthoritativeEngineTime)
		require.Equal(t, uint32(600), requireTrap(t, received).RequestID)

		// The pre-wrap tuple is now stale.
		report := decodeAuthorityResponse(t, sendAuthorityInform(t, conn, authorityEngineID, 7, 42, 61, 601), authorityCreds())
		requireReportCounter(t, report, usmStatsNotInTimeWindows, 1)
	})

	// Once boots reaches 2^31-1 the engine stops accepting authenticated
	// requests, but still answers discovery.
	t.Run("boots_saturation", func(t *testing.T) {
		clock := &authorityClock{}
		tl, received := startAuthorityListener(t, singleUserParams(), authorityEngineID, authoritativeMaxBoots-1, clock)
		conn := dialAuthority(t, tl)

		// Before the wrap the configured boots is accepted.
		response := decodeAuthorityResponse(t, sendAuthorityInform(t, conn, authorityEngineID, authoritativeMaxBoots-1, 0, 70, 700), authorityCreds())
		require.Equal(t, GetResponse, response.PDUType)
		require.Equal(t, uint32(700), requireTrap(t, received).RequestID)

		// The wrap pushes boots to 2^31-1: authenticated requests are no
		// longer accepted.
		clock.set((1 << 31) * time.Second)
		report := decodeAuthorityResponse(t, sendAuthorityInform(t, conn, authorityEngineID, authoritativeMaxBoots, 0, 71, 701), authorityCreds())
		requireReportCounter(t, report, usmStatsNotInTimeWindows, 1)
		requireNoTrap(t, received)

		// Discovery is still answered, with the saturated clock.
		discovery := decodeNoAuthReport(t, authorityRoundTrip(t, conn, authorityDiscoveryProbe(t, 72, 702)))
		requireReportCounter(t, discovery, usmStatsUnknownEngineIDs, 1)
		drsp := discovery.SecurityParameters.(*UsmSecurityParameters)
		require.Equal(t, uint32(authoritativeMaxBoots), drsp.AuthoritativeEngineBoots)
		require.Equal(t, uint32(0), drsp.AuthoritativeEngineTime)
	})
}

func TestInformAuthorityPrivacy(t *testing.T) {
	// An authPriv (SHA/AES128) Inform gets an encrypted GetResponse that
	// the device can verify and decrypt, with a salt that is not copied
	// from the request.
	t.Run("authpriv_response", func(t *testing.T) {
		clock := &authorityClock{}
		tl, received := startAuthorityListener(t, newTestGoSNMPv3(AuthPriv, authorityCreds()), authorityEngineID, 7, clock)
		conn := dialAuthority(t, tl)

		device := authorityDevice(authorityEngineID, 7, 0)
		raw := marshalAuthorityMessage(t, device, AuthPriv|Reportable, InformRequest, 90, 900, authorityInformVars())
		response := decodeAuthorityResponse(t, authorityRoundTrip(t, conn, raw), authorityCreds())
		require.Equal(t, GetResponse, response.PDUType)
		require.Equal(t, AuthPriv, response.MsgFlags&AuthPriv)
		rsp := response.SecurityParameters.(*UsmSecurityParameters)
		require.Len(t, rsp.PrivacyParameters, 8)
		require.NotEqual(t, device.PrivacyParameters, rsp.PrivacyParameters)
		require.Equal(t, trapTestPayload, string(response.Variables[0].Value.([]byte)))
		require.Equal(t, uint32(900), requireTrap(t, received).RequestID)
	})

	// Every encrypted response allocates a fresh salt from the listener's
	// own generator: repeated same-second notifications and different
	// devices never see an IV reused.
	t.Run("salt_uniqueness", func(t *testing.T) {
		clock := &authorityClock{} // fixed at zero: the same second throughout
		tl, received := startAuthorityListener(t, authorityTableParams(t, authorityCreds()), authorityEngineID, 7, clock)
		conn := dialAuthority(t, tl)
		conn2 := dialAuthority(t, tl)

		salts := map[string]struct{}{}
		sendAndCheck := func(conn net.Conn, msgID, reqID uint32) {
			t.Helper()
			device := authorityDevice(authorityEngineID, 7, 0)
			raw := marshalAuthorityMessage(t, device, AuthPriv|Reportable, InformRequest, msgID, reqID, authorityInformVars())
			response := decodeAuthorityResponse(t, authorityRoundTrip(t, conn, raw), authorityCreds())
			require.Equal(t, GetResponse, response.PDUType)
			rsp := response.SecurityParameters.(*UsmSecurityParameters)
			require.Len(t, rsp.PrivacyParameters, 8)
			require.NotEqual(t, device.PrivacyParameters, rsp.PrivacyParameters)
			_, dup := salts[string(rsp.PrivacyParameters)]
			require.False(t, dup, "response salt reused")
			salts[string(rsp.PrivacyParameters)] = struct{}{}
			requireTrap(t, received)
		}
		// The identical datagram repeated in the same second.
		device := authorityDevice(authorityEngineID, 7, 0)
		raw := marshalAuthorityMessage(t, device, AuthPriv|Reportable, InformRequest, 95, 950, authorityInformVars())
		for range 2 {
			response := decodeAuthorityResponse(t, authorityRoundTrip(t, conn, raw), authorityCreds())
			rsp := response.SecurityParameters.(*UsmSecurityParameters)
			_, dup := salts[string(rsp.PrivacyParameters)]
			require.False(t, dup, "response salt reused for a repeated inform")
			salts[string(rsp.PrivacyParameters)] = struct{}{}
			requireTrap(t, received)
		}
		// Different devices in the same second.
		for i := range 3 {
			sendAndCheck(conn, uint32(100+i), uint32(1000+i))
			sendAndCheck(conn2, uint32(110+i), uint32(1100+i))
		}
	})

	// With rotated credentials in the table (a failing candidate first),
	// the packet is authenticated by the candidate whose digest verifies,
	// and both the Report and the normal response verify under that key.
	t.Run("rotated_credentials", func(t *testing.T) {
		clock := &authorityClock{}
		wrong := authorityCreds()
		wrong.AuthenticationPassphrase = "rotatedOldPass1"
		wrong.PrivacyPassphrase = "rotatedOldPriv1"
		tl, received := startAuthorityListener(t, authorityTableParams(t, wrong, authorityCreds()), authorityEngineID, 7, clock)
		conn := dialAuthority(t, tl)

		// Inform signed with the current (second) credentials.
		response := decodeAuthorityResponse(t, sendAuthorityInform(t, conn, authorityEngineID, 7, 0, 200, 2000), authorityCreds())
		require.Equal(t, GetResponse, response.PDUType)
		require.Equal(t, uint32(2000), requireTrap(t, received).RequestID)

		// A stale Inform gets a Report that verifies with the same key.
		stale := authorityDevice(authorityEngineID, 7, 900)
		raw := marshalAuthorityMessage(t, stale, AuthNoPriv|Reportable, InformRequest, 201, 2001, authorityInformVars())
		report := decodeAuthorityResponse(t, authorityRoundTrip(t, conn, raw), authorityCreds())
		requireReportCounter(t, report, usmStatsNotInTimeWindows, 1)
		requireNoTrap(t, received)
	})

	// Replacing the security parameters table from inside the callback
	// must not change which credentials sign this packet's response.
	t.Run("table_swap_during_callback", func(t *testing.T) {
		clock := &authorityClock{}
		params := authorityTableParams(t, authorityCreds())
		tl := NewTrapListener()
		tl.Params = params
		require.NoError(t, tl.SetAuthoritativeEngine(authorityEngineID, 7))
		clock.install(t, tl)
		received := make(chan *SnmpPacket, 4)
		var swapped atomic.Bool
		tl.OnNewTrap = func(p *SnmpPacket, _ *net.UDPAddr) {
			if swapped.CompareAndSwap(false, true) {
				bad := authorityCreds()
				bad.AuthenticationPassphrase = "swappedPass1"
				bad.PrivacyPassphrase = "swappedPriv1"
				badTable := NewSnmpV3SecurityParametersTable(authorityLogger())
				if err := badTable.Add(bad.UserName, bad); err == nil {
					params.SetTrapSecurityParametersTable(badTable)
				}
			}
			received <- p
		}
		listenOnEphemeral(t, tl)
		t.Cleanup(tl.Close)
		conn := dialAuthority(t, tl)

		// The response to the in-flight packet still verifies with the
		// credentials that authenticated it, not with the swapped table.
		response := decodeAuthorityResponse(t, sendAuthorityInform(t, conn, authorityEngineID, 7, 0, 300, 3000), authorityCreds())
		require.Equal(t, GetResponse, response.PDUType)
		require.True(t, swapped.Load())
		require.Equal(t, uint32(3000), requireTrap(t, received).RequestID)
	})

	// Building responses must not modify the packet handed to the
	// callback, even across later packets.
	t.Run("callback_packet_untouched", func(t *testing.T) {
		clock := &authorityClock{}
		tl := NewTrapListener()
		tl.Params = newTestGoSNMPv3(AuthPriv, authorityCreds())
		require.NoError(t, tl.SetAuthoritativeEngine(authorityEngineID, 7))
		clock.install(t, tl)
		captured := make(chan *SnmpPacket, 4)
		tl.OnNewTrap = func(p *SnmpPacket, _ *net.UDPAddr) { captured <- p }
		listenOnEphemeral(t, tl)
		t.Cleanup(tl.Close)
		conn := dialAuthority(t, tl)

		device := authorityDevice(authorityEngineID, 7, 0)
		raw := marshalAuthorityMessage(t, device, AuthPriv|Reportable, InformRequest, 400, 4000, authorityInformVars())
		response := decodeAuthorityResponse(t, authorityRoundTrip(t, conn, raw), authorityCreds())
		require.Equal(t, GetResponse, response.PDUType)
		first := requireTrap(t, captured)

		// A second inform must not disturb the first callback's packet.
		sendAuthorityInform(t, conn, authorityEngineID, 7, 0, 401, 4001)
		requireTrap(t, captured)

		require.Equal(t, InformRequest, first.PDUType)
		require.Equal(t, Reportable|AuthPriv, first.MsgFlags)
		require.Equal(t, uint32(4000), first.RequestID)
		fsp := first.SecurityParameters.(*UsmSecurityParameters)
		require.Equal(t, uint32(7), fsp.AuthoritativeEngineBoots)
		require.Equal(t, uint32(0), fsp.AuthoritativeEngineTime)
		require.Equal(t, device.PrivacyParameters, fsp.PrivacyParameters)
		require.Equal(t, trapTestPayload, string(first.Variables[0].Value.([]byte)))
	})

	// Two listeners are fully isolated: each answers with its own engine
	// ID, clock and salt generator, and an Inform addressed to the other
	// engine is dropped.
	t.Run("listeners_isolated", func(t *testing.T) {
		clockA, clockB := &authorityClock{}, &authorityClock{}
		tlA, receivedA := startAuthorityListener(t, newTestGoSNMPv3(AuthPriv, authorityCreds()), authorityEngineID, 7, clockA)
		tlB, receivedB := startAuthorityListener(t, newTestGoSNMPv3(AuthPriv, authorityCreds()), authorityEngineID2, 11, clockB)
		connA := dialAuthority(t, tlA)
		connB := dialAuthority(t, tlB)

		responseA := decodeAuthorityResponse(t, sendAuthorityInform(t, connA, authorityEngineID, 7, 0, 500, 5000), authorityCreds())
		rspA := responseA.SecurityParameters.(*UsmSecurityParameters)
		require.Equal(t, authorityEngineID, rspA.AuthoritativeEngineID)
		require.Equal(t, uint32(7), rspA.AuthoritativeEngineBoots)
		require.Equal(t, uint32(5000), requireTrap(t, receivedA).RequestID)

		// An Inform for A's engine sent to B is a remote Inform: dropped.
		deviceA := authorityDevice(authorityEngineID, 7, 0)
		rawA := marshalAuthorityMessage(t, deviceA, AuthPriv|Reportable, InformRequest, 501, 5001, authorityInformVars())
		authorityExpectSilence(t, connB, rawA)
		requireNoTrap(t, receivedB)

		// B still answers its own Informs with its own engine and clock.
		responseB := decodeAuthorityResponse(t, sendAuthorityInform(t, connB, authorityEngineID2, 11, 0, 502, 5002), authorityCreds())
		rspB := responseB.SecurityParameters.(*UsmSecurityParameters)
		require.Equal(t, authorityEngineID2, rspB.AuthoritativeEngineID)
		require.Equal(t, uint32(11), rspB.AuthoritativeEngineBoots)
		require.NotEqual(t, rspA.PrivacyParameters, rspB.PrivacyParameters)
		require.Equal(t, uint32(5002), requireTrap(t, receivedB).RequestID)
	})
}
