// Copyright 2026 The GoSNMP Authors. All rights reserved.  Use of this
// source code is governed by a BSD-style license that can be found in the
// LICENSE file.

package gosnmp

import (
	crand "crypto/rand"
	"encoding/binary"
	"errors"
	"fmt"
	"net"
	"sync"
	"sync/atomic"
	"time"
)

const (
	// authoritativeMaxEngineTime is the largest valid
	// msgAuthoritativeEngineTime value; the next second wraps the local
	// clock to zero and increments the boots counter.
	authoritativeMaxEngineTime = 2147483647

	// authoritativeMaxBoots is the msgAuthoritativeEngineBoots value at
	// which the engine stops accepting authenticated messages (RFC3414
	// 3.2); a listener must be configured below it.
	authoritativeMaxBoots = 2147483647

	// authoritativeTimeWindow is the RFC3414 3.2 step 7(a) timeliness
	// window in seconds. A difference of exactly the window is accepted.
	authoritativeTimeWindow = 150
)

// authoritativeEngine holds the local state of a TrapListener running in
// authoritative engine mode. The clock starts at zero seconds when Listen
// begins and advances only with monotonically elapsed time; received
// packets never adjust it. The salt counters give every encrypted
// response a fresh IV (RFC3826 3.1.1.1) independent of the credentials
// copies made per packet.
type authoritativeEngine struct {
	mu       sync.Mutex
	engineID string
	boots    uint32
	started  bool
	start    time.Time
	nowFn    func() time.Time // test hook; nil means time.Now

	aesSalt atomic.Uint64
	desSalt atomic.Uint32
}

func (a *authoritativeEngine) now() time.Time {
	if a.nowFn != nil {
		return a.nowFn()
	}
	return time.Now()
}

// clock returns the current authoritative engine boots/time. When the
// seconds value passes authoritativeMaxEngineTime it wraps to zero and
// boots is incremented.
func (a *authoritativeEngine) clock() (boots, tim uint32) {
	a.mu.Lock()
	defer a.mu.Unlock()
	elapsed := a.now().Sub(a.start)
	if elapsed < 0 {
		elapsed = 0
	}
	total := uint64(elapsed / time.Second)
	return a.boots + uint32(total>>31), uint32(total & authoritativeMaxEngineTime)
}

// startClock begins the authoritative clock at zero seconds and seeds the
// response salt generators with a random start, per RFC3826.
func (a *authoritativeEngine) startClock() {
	a.mu.Lock()
	defer a.mu.Unlock()
	a.start = a.now()
	a.started = true
	var random [8]byte
	if _, err := crand.Read(random[:]); err == nil {
		a.aesSalt.Store(binary.BigEndian.Uint64(random[:]))
		a.desSalt.Store(binary.BigEndian.Uint32(random[:4]))
	}
}

// allocateSalt returns fresh privacy parameters for one outgoing
// encrypted message. The counter lives on the listener, so per-packet
// credential copies, table replacements and repeated notifications can
// never restart it and reuse an IV under the same key.
func (a *authoritativeEngine) allocateSalt(privProtocol SnmpV3PrivProtocol, boots uint32) ([]byte, error) {
	salt := make([]byte, 8)
	switch privProtocol {
	case AES, AES192, AES256, AES192C, AES256C:
		binary.BigEndian.PutUint64(salt, a.aesSalt.Add(1))
	case DES:
		binary.BigEndian.PutUint32(salt, boots)
		binary.BigEndian.PutUint32(salt[4:], a.desSalt.Add(1))
	default:
		return nil, fmt.Errorf("authoritative engine: unsupported privacy protocol %s", privProtocol)
	}
	return salt, nil
}

// SetAuthoritativeEngine enables authoritative engine mode on the
// TrapListener: the listener then acts as the authoritative SNMP engine
// for SNMPv3 messages addressed to engineID, answering engine discovery
// and time-sync probes and confirming Informs with its own clock. It must
// be called before Listen; the mode applies to UDP only and cannot be
// reconfigured once listening has started.
//
// engineID is matched against raw bytes and must be 5 to 32 octets
// (RFC3411 section 5). engineBoots must be between 1 and 2147483646. The
// engine clock starts at zero seconds when Listen begins; persisting
// boots across process restarts is the caller's responsibility — pass an
// increased value on every restart so the clock never moves backwards.
// gosnmp deliberately does not store it on disk. An invalid configuration
// returns an error and keeps the previous one.
func (t *TrapListener) SetAuthoritativeEngine(engineID string, engineBoots uint32) error {
	t.authoritativeMu.Lock()
	defer t.authoritativeMu.Unlock()
	if t.authoritative != nil && t.authoritative.started {
		return errors.New("SetAuthoritativeEngine: the authoritative engine cannot be reconfigured after Listen has started")
	}
	if len(engineID) < 5 || len(engineID) > 32 {
		return fmt.Errorf("SetAuthoritativeEngine: engine ID must be 5 to 32 octets, got %d", len(engineID))
	}
	if engineBoots < 1 || engineBoots > authoritativeMaxBoots-1 {
		return fmt.Errorf("SetAuthoritativeEngine: engine boots must be between 1 and %d, got %d", authoritativeMaxBoots-1, engineBoots)
	}
	t.authoritative = &authoritativeEngine{engineID: engineID, boots: engineBoots}
	return nil
}

func (t *TrapListener) authoritativeState() *authoritativeEngine {
	t.authoritativeMu.Lock()
	defer t.authoritativeMu.Unlock()
	return t.authoritative
}

// startAuthoritativeEngine activates authoritative engine mode when the
// UDP listener starts. It is a no-op when the mode is not configured.
func (t *TrapListener) startAuthoritativeEngine() {
	t.authoritativeMu.Lock()
	defer t.authoritativeMu.Unlock()
	if t.authoritative != nil {
		t.authoritative.startClock()
	}
}

// authoritativeHeader is the USM header of an incoming v3 message, parsed
// off a copy so the original datagram stays untouched for the path that
// ends up handling it.
type authoritativeHeader struct {
	msgFlags SnmpV3MsgFlags
	engineID string
	userName string
}

func (t *TrapListener) peekAuthoritativeHeader(msg []byte) (authoritativeHeader, bool) {
	var hdr authoritativeHeader
	probe := &SnmpPacket{
		SecurityParameters: &UsmSecurityParameters{
			// NoAuth/NoPriv keeps the header parser from rekeying on the
			// packet's engine ID; it then stops at the authentication
			// parameters of an authenticated message, which is fine: every
			// field needed here is parsed before that point and the error
			// is deliberately ignored.
			AuthenticationProtocol: NoAuth,
			PrivacyProtocol:        NoPriv,
			Logger:                 t.Params.Logger,
		},
		Logger: t.Params.Logger,
	}
	// The parse may stop at the authentication parameters (e.g. a MAC
	// longer than SHA's); every field needed here is parsed before that
	// point, so the error is deliberately ignored.
	_, _ = t.Params.unmarshalHeader(msgCopy(msg), probe)
	if probe.Version != Version3 || probe.SecurityModel != UserSecurityModel {
		return hdr, false
	}
	usm, ok := probe.SecurityParameters.(*UsmSecurityParameters)
	if !ok {
		return hdr, false
	}
	hdr.msgFlags = probe.MsgFlags
	hdr.engineID = usm.AuthoritativeEngineID
	hdr.userName = usm.UserName
	return hdr, true
}

// handleAuthoritative processes one received datagram in authoritative
// engine mode. It reports whether the datagram was consumed; a false
// return means the caller should run the legacy receive path.
func (t *TrapListener) handleAuthoritative(msg []byte, remote *net.UDPAddr) bool {
	a := t.authoritativeState()
	if a == nil {
		return false
	}
	hdr, ok := t.peekAuthoritativeHeader(msg)
	if !ok {
		return false
	}
	switch {
	case hdr.engineID == "" && hdr.userName == "" &&
		hdr.msgFlags&AuthPriv == NoAuthNoPriv && hdr.msgFlags&Reportable != 0:
		// Engine discovery (RFC3414 section 4): answered even when a
		// security parameters table is configured, and never delivered as
		// a notification.
		t.handleAuthoritativeDiscovery(a, msg, remote)
		return true
	case hdr.engineID == a.engineID:
		t.handleAuthoritativeLocal(a, msg, remote, hdr.userName)
		return true
	default:
		// A message for another engine: traps follow the legacy receive
		// path, but an Inform must not be confirmed as if it were
		// addressed to this engine, so it is dropped here.
		trap, err := t.Params.UnmarshalTrap(msgCopy(msg), false)
		if err == nil && trap.PDUType == InformRequest {
			t.Params.Logger.Printf("TrapListener: dropping Inform for remote authoritative engine ID %0x", []byte(hdr.engineID))
			return true
		}
		return false
	}
}

// handleAuthoritativeDiscovery answers an engine discovery request with a
// usmStatsUnknownEngineIDs Report carrying the local engine ID and clock.
// Requests that only look like discovery (varbinds, notifications) are
// dropped instead of entering this unauthenticated path.
func (t *TrapListener) handleAuthoritativeDiscovery(a *authoritativeEngine, msg []byte, remote *net.UDPAddr) {
	probe, err := t.Params.unmarshalTrapBase(msgCopy(msg), nil, true)
	if err != nil || !isEngineDiscoveryRequest(probe) {
		t.Params.Logger.Printf("TrapListener: dropping invalid engine discovery request: %v", err)
		return
	}
	count := atomic.AddUint32(&t.usmStatsUnknownEngineIDsCount, 1)
	if err := t.SendUDP(t.discoveryReport(a, probe, count), remote); err != nil {
		t.Params.Logger.Printf("TrapListener: %s", err)
	}
}

// discoveryReport builds the usmStatsUnknownEngineIDs Report answering an
// engine discovery request: the local engine ID and clock, the cumulative
// counter, and the request's message and request IDs.
func (t *TrapListener) discoveryReport(a *authoritativeEngine, probe *SnmpPacket, count uint32) *SnmpPacket {
	boots, tim := a.clock()
	return &SnmpPacket{
		Version:       Version3,
		MsgFlags:      NoAuthNoPriv,
		SecurityModel: UserSecurityModel,
		SecurityParameters: &UsmSecurityParameters{
			AuthoritativeEngineID:    a.engineID,
			AuthoritativeEngineBoots: boots,
			AuthoritativeEngineTime:  tim,
			Logger:                   t.Params.Logger,
		},
		ContextEngineID: a.engineID,
		PDUType:         Report,
		MsgID:           probe.MsgID,
		RequestID:       probe.RequestID,
		MsgMaxSize:      probe.MsgMaxSize,
		Variables: []SnmpPDU{{
			Name:  usmStatsUnknownEngineIDs,
			Type:  Counter32,
			Value: count,
		}},
		Logger: t.Params.Logger,
	}
}

// handleAuthoritativeLocal processes a v3 message addressed to the local
// authoritative engine ID: discovery is handled elsewhere, so this covers
// authenticated time-sync probes and Informs. Anything else is dropped;
// the mode is not a Get/Set proxy.
func (t *TrapListener) handleAuthoritativeLocal(a *authoritativeEngine, msg []byte, remote *net.UDPAddr, userName string) {
	result, creds, raw, cursor, err := t.authenticateAuthoritative(a, msg, userName)
	if err != nil {
		// Unknown user, bad digest or no usable credentials: drop without
		// a response, and without touching the timeliness counters or the
		// clock.
		t.Params.Logger.Printf("TrapListener: authoritative engine: %s", err)
		return
	}
	if result.MsgFlags&AuthNoPriv != 0 {
		// Timeliness is checked after authentication and before the
		// scoped PDU is decrypted (RFC3414 3.2 step 7).
		boots, tim := a.clock()
		if boots >= authoritativeMaxBoots ||
			creds.AuthoritativeEngineBoots != boots ||
			authoritativeOutsideWindow(creds.AuthoritativeEngineTime, tim) {
			count := atomic.AddUint32(&t.usmStatsNotInTimeWindowsCount, 1)
			if result.MsgFlags&Reportable != 0 {
				t.reportNotInTimeWindow(a, result, creds, count, remote)
			}
			return
		}
	}
	raw, cursor, err = t.Params.decryptPacket(raw, cursor, result)
	if err != nil {
		t.Params.Logger.Printf("TrapListener: authoritative engine decrypt: %s", err)
		return
	}
	if err = t.Params.unmarshalPayload(raw, cursor, result); err != nil {
		t.Params.Logger.Printf("TrapListener: authoritative engine payload: %s", err)
		return
	}
	switch {
	case result.PDUType == InformRequest:
		t.OnNewTrap(result, remote)
		t.respondToInform(a, result, creds, remote)
	case result.PDUType == GetRequest && len(result.Variables) == 0:
		// Authenticated time-sync probe with a valid clock: silently
		// discarded, it is not a notification.
	default:
		t.Params.Logger.Printf("TrapListener: authoritative engine: dropping %s for the local engine ID", result.PDUType)
	}
}

// authenticateAuthoritative selects the credentials that actually verify
// the message digest, from the single-user configuration or the security
// parameters table. Keys are localized to the local engine ID on a copy;
// the shared entries never see the packet's engine ID or clock. A failing
// candidate (e.g. a rotated-out key) does not fail the packet while
// another candidate verifies.
func (t *TrapListener) authenticateAuthoritative(a *authoritativeEngine, msg []byte, userName string) (result *SnmpPacket, creds *UsmSecurityParameters, raw []byte, cursor int, err error) {
	candidates, err := t.authoritativeCandidates(userName)
	if err != nil {
		return nil, nil, nil, 0, err
	}
	for _, candidate := range candidates {
		usm, ok := candidate.(*UsmSecurityParameters)
		if !ok {
			continue
		}
		candidate := usm.Copy().(*UsmSecurityParameters)
		if candidate.AuthoritativeEngineID != a.engineID {
			candidate.AuthoritativeEngineID = a.engineID
			candidate.SecretKey = nil
			candidate.PrivacyKey = nil
		}
		if err = candidate.InitSecurityKeys(); err != nil {
			continue
		}
		raw = msgCopy(msg)
		result = &SnmpPacket{SecurityParameters: candidate, Logger: t.Params.Logger}
		cursor, err = t.Params.unmarshalHeader(raw, result)
		if err != nil {
			continue
		}
		if err = candidate.checkAuthenticated(result.MsgFlags); err != nil {
			continue
		}
		if result.MsgFlags&AuthNoPriv != 0 {
			authentic, authErr := candidate.isAuthentic(raw, result)
			if authErr != nil || !authentic {
				continue
			}
		}
		return result, candidate, raw, cursor, nil
	}
	return nil, nil, nil, 0, fmt.Errorf("authoritative engine: no credentials authenticated message for user %q", userName)
}

func (t *TrapListener) authoritativeCandidates(userName string) ([]SnmpV3SecurityParameters, error) {
	if table := t.Params.GetTrapSecurityParametersTable(); table != nil {
		list, err := table.Get(userName)
		if err != nil {
			return nil, fmt.Errorf("authoritative engine: unknown user %q", userName)
		}
		return list, nil
	}
	usm, ok := t.Params.SecurityParameters.(*UsmSecurityParameters)
	if !ok || usm == nil {
		return nil, errors.New("authoritative engine: no security parameters configured")
	}
	if usm.UserName != userName {
		return nil, fmt.Errorf("authoritative engine: unknown user %q", userName)
	}
	return []SnmpV3SecurityParameters{usm}, nil
}

// respondToInform confirms a delivered Inform with a GetResponse at the
// same security level, carrying the local engine ID and clock and, for
// encrypted responses, a freshly allocated salt. The packet handed to the
// callback is never modified.
func (t *TrapListener) respondToInform(a *authoritativeEngine, inform *SnmpPacket, creds *UsmSecurityParameters, remote *net.UDPAddr) {
	response, err := t.informResponse(a, inform, creds)
	if err != nil {
		t.Params.Logger.Printf("TrapListener: %s", err)
		return
	}
	if err := t.SendUDP(response, remote); err != nil {
		t.Params.Logger.Printf("TrapListener: %s", err)
	}
}

// informResponse builds the GetResponse for a delivered Inform.
func (t *TrapListener) informResponse(a *authoritativeEngine, inform *SnmpPacket, creds *UsmSecurityParameters) (*SnmpPacket, error) {
	boots, tim := a.clock()
	sp := creds.Copy().(*UsmSecurityParameters)
	sp.AuthoritativeEngineBoots = boots
	sp.AuthoritativeEngineTime = tim
	sp.AuthenticationParameters = ""
	sp.PrivacyParameters = nil

	// Same security level as the request, reportable flag cleared.
	msgFlags := inform.MsgFlags & AuthPriv
	if msgFlags&AuthPriv > AuthNoPriv {
		salt, err := a.allocateSalt(sp.PrivacyProtocol, boots)
		if err != nil {
			return nil, err
		}
		sp.PrivacyParameters = salt
	}

	return &SnmpPacket{
		Version:            Version3,
		MsgFlags:           msgFlags,
		SecurityModel:      UserSecurityModel,
		SecurityParameters: sp,
		ContextEngineID:    inform.ContextEngineID,
		ContextName:        inform.ContextName,
		PDUType:            GetResponse,
		MsgID:              inform.MsgID,
		RequestID:          inform.RequestID,
		MsgMaxSize:         inform.MsgMaxSize,
		Error:              NoError,
		ErrorIndex:         0,
		Variables:          cloneVariables(inform.Variables),
		Logger:             t.Params.Logger,
	}, nil
}

// reportNotInTimeWindow sends the RFC3414 3.2 step 7(a) authNoPriv Report
// for an authenticated message outside the time window. The scoped PDU is
// still encrypted at this point, so the request ID is reported as zero.
func (t *TrapListener) reportNotInTimeWindow(a *authoritativeEngine, request *SnmpPacket, creds *UsmSecurityParameters, count uint32, remote *net.UDPAddr) {
	if err := t.SendUDP(t.notInTimeWindowReport(a, request, creds, count), remote); err != nil {
		t.Params.Logger.Printf("TrapListener: %s", err)
	}
}

// notInTimeWindowReport builds the authNoPriv usmStatsNotInTimeWindows
// Report, signed with the credentials that authenticated the request and
// carrying the local clock.
func (t *TrapListener) notInTimeWindowReport(a *authoritativeEngine, request *SnmpPacket, creds *UsmSecurityParameters, count uint32) *SnmpPacket {
	boots, tim := a.clock()
	sp := creds.Copy().(*UsmSecurityParameters)
	sp.AuthoritativeEngineBoots = boots
	sp.AuthoritativeEngineTime = tim
	sp.AuthenticationParameters = ""
	sp.PrivacyParameters = nil
	return &SnmpPacket{
		Version:            Version3,
		MsgFlags:           AuthNoPriv,
		SecurityModel:      UserSecurityModel,
		SecurityParameters: sp,
		ContextEngineID:    a.engineID,
		PDUType:            Report,
		MsgID:              request.MsgID,
		RequestID:          0,
		MsgMaxSize:         request.MsgMaxSize,
		Variables: []SnmpPDU{{
			Name:  usmStatsNotInTimeWindows,
			Type:  Counter32,
			Value: count,
		}},
		Logger: t.Params.Logger,
	}
}

// authoritativeOutsideWindow reports whether packetTime differs from the
// local engine time by more than the RFC3414 timeliness window.
func authoritativeOutsideWindow(packetTime, localTime uint32) bool {
	diff := packetTime - localTime
	if packetTime < localTime {
		diff = localTime - packetTime
	}
	return diff > authoritativeTimeWindow
}

func msgCopy(msg []byte) []byte {
	cp := make([]byte, len(msg))
	copy(cp, msg)
	return cp
}

func cloneVariables(vars []SnmpPDU) []SnmpPDU {
	out := make([]SnmpPDU, len(vars))
	for i, v := range vars {
		out[i] = v
		if b, ok := v.Value.([]byte); ok {
			out[i].Value = append([]byte(nil), b...)
		}
	}
	return out
}
