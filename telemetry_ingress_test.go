package gomavlib

// This test pins down the telemetry ingress contract of Node.Events():
//
//   - stream channels accept frames split across Read calls and coalesced in
//     one Read call, preserving arrival order;
//   - datagram channels deliver exactly one frame per packet, report the
//     unconsumed bytes when the next packet arrives, and never splice a
//     truncated packet with the following one;
//   - known messages are decoded with the dialect (V2 zero/extension padding,
//     V1 base-only layout), while unknown IDs stay MessageRaw;
//   - a bad CRC on a known message yields EventParseError without publishing
//     the frame, and the next good frame is still ingested;
//   - every event is attributed to the channel that produced it.
//
// Wire bytes are built independently of Node/Writer/Reader: the test contains
// its own X25 reference implementation and its own hand-assembled payloads.

import (
	"bytes"
	"context"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net"
	"strings"
	"sync"
	"syscall"
	"testing"
	"time"

	"github.com/bluenviron/gomavlib/v4/pkg/dialects/common"
	"github.com/bluenviron/gomavlib/v4/pkg/frame"
	"github.com/bluenviron/gomavlib/v4/pkg/message"
)

const (
	telemetryStreamSystemID    = 0x51
	telemetryStreamComponentID = 0x52
	telemetryUDPSystemID       = 0x61
	telemetryUDPComponentID    = 0x62

	// CRC extras published by the MAVLink specification for common.xml.
	telemetryGpsRawIntID       = 24
	telemetryGpsRawIntCRCExtra = 24
	telemetryVfrHudCRCExtra    = 20

	telemetryGpsBaseSize     = 30
	telemetryGpsExtendedSize = 52
	// 8001 is unused by common.xml: its highest regular id is 440.
	telemetryUnknownID    = 8001
	telemetryFragmentSize = 3
	telemetryEventBarrier = 5 * time.Second
)

// telemetrySample is the independently expected value set for GPS_RAW_INT.
type telemetrySample struct {
	timeUsec     uint64
	lat          int32
	lon          int32
	alt          int32
	eph          uint16
	epv          uint16
	vel          uint16
	cog          uint16
	fixType      byte
	satellites   byte
	altEllipsoid int32
	hAcc         uint32
	vAcc         uint32
	velAcc       uint32
	hdgAcc       uint32
	yaw          uint16
}

var (
	telemetryExtendedSample = telemetrySample{
		timeUsec: 0x0102030405060708, lat: 0x11223344, lon: -0x01234567, alt: 0x0A0B0C0D,
		eph: 0x1234, epv: 0x2345, vel: 0x3456, cog: 0x4567, fixType: 0x03, satellites: 0x0B,
		altEllipsoid: -0x0A0B0C0D, hAcc: 0xCAFEBABE, vAcc: 0x0BADF00D,
		velAcc: 0xDEADBEEF, hdgAcc: 0xFEEDFACE, yaw: 0xBEEF,
	}

	telemetryNoExtSample = telemetrySample{
		timeUsec: 0x1112131415161718, lat: 0x21222324, lon: -0x31323334, alt: 0x41424344,
		eph: 0x5152, epv: 0x6162, vel: 0x7172, cog: 0x8182, fixType: 0x04, satellites: 0x0C,
	}

	telemetryTrailingZeroSample = telemetrySample{
		timeUsec: 0x191A1B1C1D1E1F20, lat: 0x292A2B2C, lon: -0x393A3B3C, alt: 0x494A4B4C,
		eph: 0x595A, epv: 0x696A, vel: 0x797A, cog: 0x898A, fixType: 0x05, satellites: 0x00,
	}

	telemetryOtherSample = telemetrySample{
		timeUsec: 0xA1A2A3A4A5A6A7A8, lat: 0x31415162, lon: -0x21314151, alt: 0x51617182,
		eph: 0xE1E2, epv: 0xF1F2, vel: 0x0102, cog: 0x0304, fixType: 0x06, satellites: 0x0D,
	}

	telemetryAfterTruncSample = telemetrySample{
		timeUsec: 0x123456789ABCDEF0, lat: 0x2468ACE0, lon: -0x36925814, alt: 0x0F1E2D3C,
		eph: 0xAB12, epv: 0xCD34, vel: 0xEF56, cog: 0x789A, fixType: 0x07, satellites: 0x0E,
		altEllipsoid: -0x0F1E2D3C, hAcc: 0x0BADC0DE, vAcc: 0x0DEC0DE0,
		velAcc: 0x0BADCAFE, hdgAcc: 0x0FEEDBEE, yaw: 0x0FADE,
	}
)

type telemetrySource struct {
	systemID    byte
	componentID byte
}

type telemetryPayloads struct {
	extended     []byte
	noExt        []byte
	trailingZero []byte
	v1Base       []byte
	other        []byte
	afterTrunc   []byte
	unknown      []byte
}

type telemetryFrames struct {
	v2Extended     []byte
	v2NoExt        []byte
	v2TrailingZero []byte
	v1Base         []byte
	badCRC         []byte
	v2Other        []byte
	v2AfterTrunc   []byte
	truncated      []byte
	unknown        []byte
}

// telemetryRefX25 is a standalone X25/CRC-16 implementation, kept separate
// from the library implementation used by the production code.
type telemetryRefX25 struct {
	crc uint16
}

func newTelemetryRefX25() *telemetryRefX25 {
	return &telemetryRefX25{crc: 0xffff}
}

func (x *telemetryRefX25) write(p []byte) {
	for _, b := range p {
		tmp := uint16(b) ^ (x.crc & 0xff)
		tmp ^= (tmp << 4) & 0xff
		x.crc = (x.crc >> 8) ^ (tmp << 8) ^ (tmp << 3) ^ (tmp >> 4)
	}
}

func (x *telemetryRefX25) sum16() uint16 {
	return x.crc
}

type telemetryRefField struct {
	typ      string
	name     string
	arrayLen byte
}

func telemetryRefCRCExtra(name string, fields []telemetryRefField) byte {
	h := newTelemetryRefX25()
	h.write([]byte(name + " "))
	for _, f := range fields {
		h.write([]byte(f.typ + " "))
		h.write([]byte(f.name + " "))
		if f.arrayLen > 0 {
			h.write([]byte{f.arrayLen})
		}
	}
	sum := h.sum16()
	return byte((sum & 0xff) ^ (sum >> 8))
}

var telemetryGpsRawIntReferenceFields = []telemetryRefField{
	{typ: "uint64_t", name: "time_usec"},
	{typ: "int32_t", name: "lat"},
	{typ: "int32_t", name: "lon"},
	{typ: "int32_t", name: "alt"},
	{typ: "uint16_t", name: "eph"},
	{typ: "uint16_t", name: "epv"},
	{typ: "uint16_t", name: "vel"},
	{typ: "uint16_t", name: "cog"},
	{typ: "uint8_t", name: "fix_type"},
	{typ: "uint8_t", name: "satellites_visible"},
}

var telemetryVfrHudReferenceFields = []telemetryRefField{
	{typ: "float", name: "airspeed"},
	{typ: "float", name: "groundspeed"},
	{typ: "float", name: "alt"},
	{typ: "float", name: "climb"},
	{typ: "int16_t", name: "heading"},
	{typ: "uint16_t", name: "throttle"},
}

func telemetryUint24(buf []byte, v uint32) {
	buf[0] = byte(v)
	buf[1] = byte(v >> 8)
	buf[2] = byte(v >> 16)
}

func telemetryRefChecksum(headerAndPayload []byte, crcExtra byte) uint16 {
	h := newTelemetryRefX25()
	h.write(headerAndPayload)
	h.write([]byte{crcExtra})
	return h.sum16()
}

func telemetryV2Frame(src telemetrySource, seq byte, id uint32, payload []byte, crcExtra byte) []byte {
	header := make([]byte, 10)
	header[0] = frame.V2MagicByte
	header[1] = byte(len(payload))
	header[4] = seq
	header[5] = src.systemID
	header[6] = src.componentID
	telemetryUint24(header[7:], id)

	crcInput := make([]byte, 0, len(header)+len(payload))
	crcInput = append(crcInput, header[1:]...)
	crcInput = append(crcInput, payload...)
	sum := telemetryRefChecksum(crcInput, crcExtra)

	wire := append(append([]byte{}, header...), payload...)
	return append(wire, byte(sum), byte(sum>>8))
}

func telemetryV1Frame(src telemetrySource, seq byte, id byte, payload []byte, crcExtra byte) []byte {
	header := []byte{frame.V1MagicByte, byte(len(payload)), seq, src.systemID, src.componentID, id}

	crcInput := make([]byte, 0, len(header)+len(payload))
	crcInput = append(crcInput, header[1:]...)
	crcInput = append(crcInput, payload...)
	sum := telemetryRefChecksum(crcInput, crcExtra)

	wire := append(append([]byte{}, header...), payload...)
	return append(wire, byte(sum), byte(sum>>8))
}

// telemetryGpsBasePayload assembles the base (non-extension) fields in the
// MAVLink wire order: widest fields first, regardless of struct declaration
// order. This is intentionally independent from the library writer.
func telemetryGpsBasePayload(s telemetrySample) []byte {
	buf := make([]byte, 0, telemetryGpsBaseSize)
	buf = binary.LittleEndian.AppendUint64(buf, s.timeUsec)
	buf = binary.LittleEndian.AppendUint32(buf, uint32(s.lat))
	buf = binary.LittleEndian.AppendUint32(buf, uint32(s.lon))
	buf = binary.LittleEndian.AppendUint32(buf, uint32(s.alt))
	buf = binary.LittleEndian.AppendUint16(buf, s.eph)
	buf = binary.LittleEndian.AppendUint16(buf, s.epv)
	buf = binary.LittleEndian.AppendUint16(buf, s.vel)
	buf = binary.LittleEndian.AppendUint16(buf, s.cog)
	buf = append(buf, s.fixType, s.satellites)
	return buf
}

func telemetryGpsExtendedPayload(s telemetrySample) []byte {
	buf := telemetryGpsBasePayload(s)
	buf = binary.LittleEndian.AppendUint32(buf, uint32(s.altEllipsoid))
	buf = binary.LittleEndian.AppendUint32(buf, s.hAcc)
	buf = binary.LittleEndian.AppendUint32(buf, s.vAcc)
	buf = binary.LittleEndian.AppendUint32(buf, s.velAcc)
	buf = binary.LittleEndian.AppendUint32(buf, s.hdgAcc)
	buf = binary.LittleEndian.AppendUint16(buf, s.yaw)
	return buf
}

func telemetryBuildPayloads() telemetryPayloads {
	trailing := telemetryGpsBasePayload(telemetryTrailingZeroSample)
	// The last base byte (satellites_visible) is zero and is omitted by the
	// V2 empty-byte truncation rules.
	trailing = trailing[:len(trailing)-1]

	return telemetryPayloads{
		extended:     telemetryGpsExtendedPayload(telemetryExtendedSample),
		noExt:        telemetryGpsBasePayload(telemetryNoExtSample),
		trailingZero: trailing,
		v1Base:       telemetryGpsBasePayload(telemetryNoExtSample),
		other:        telemetryGpsBasePayload(telemetryOtherSample),
		afterTrunc:   telemetryGpsExtendedPayload(telemetryAfterTruncSample),
		unknown:      []byte{0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x7F, 0x42},
	}
}

func telemetryBuildFrames(src telemetrySource, seq byte, p telemetryPayloads) telemetryFrames {
	badCRC := append([]byte{}, telemetryV2Frame(src, seq, telemetryGpsRawIntID, p.extended, telemetryGpsRawIntCRCExtra)...)
	badCRC[len(badCRC)-1] ^= 0x5A

	afterTrunc := telemetryV2Frame(src, seq+5, telemetryGpsRawIntID, p.afterTrunc, telemetryGpsRawIntCRCExtra)

	return telemetryFrames{
		v2Extended:     telemetryV2Frame(src, seq, telemetryGpsRawIntID, p.extended, telemetryGpsRawIntCRCExtra),
		v2NoExt:        telemetryV2Frame(src, seq+1, telemetryGpsRawIntID, p.noExt, telemetryGpsRawIntCRCExtra),
		v2TrailingZero: telemetryV2Frame(src, seq+2, telemetryGpsRawIntID, p.trailingZero, telemetryGpsRawIntCRCExtra),
		v1Base:         telemetryV1Frame(src, seq+3, byte(telemetryGpsRawIntID), p.v1Base, telemetryGpsRawIntCRCExtra),
		badCRC:         badCRC,
		v2Other:        telemetryV2Frame(src, seq+4, telemetryGpsRawIntID, p.other, telemetryGpsRawIntCRCExtra),
		v2AfterTrunc:   afterTrunc,
		truncated:      afterTrunc[:7],
		unknown:        telemetryV2Frame(src, seq+6, telemetryUnknownID, p.unknown, 0),
	}
}

// telemetryFragmentConn limits each Read to fragSize bytes, so a header,
// payload and checksum written in one piece are necessarily spread over
// multiple Read calls. It records the sizes for post-conditions.
type telemetryFragmentConn struct {
	net.Conn
	fragSize int

	mu    sync.Mutex
	reads []int
}

func (c *telemetryFragmentConn) Read(p []byte) (int, error) {
	if len(p) > c.fragSize {
		p = p[:c.fragSize]
	}
	n, err := c.Conn.Read(p)
	if n > 0 {
		c.mu.Lock()
		c.reads = append(c.reads, n)
		c.mu.Unlock()
	}
	return n, err
}

func (c *telemetryFragmentConn) readSizes() []int {
	c.mu.Lock()
	defer c.mu.Unlock()
	out := make([]int, len(c.reads))
	copy(out, c.reads)
	return out
}

// The primary datagram chain is real loopback UDP. Some sandboxed test runners
// disable local port binding; in that case only, fall back to an in-memory
// packet pair that preserves one-Write/one-Read datagram semantics so the
// Channel logic can still be exercised. The fallback is announced in the log.
type telemetryFakeAddr string

func (a telemetryFakeAddr) Network() string { return "telemetry-mem" }
func (a telemetryFakeAddr) String() string  { return string(a) }

type telemetryPacketConn struct {
	recv      chan []byte
	send      chan []byte
	closed    chan struct{}
	closeOnce *sync.Once
}

func newTelemetryPacketPair() (*telemetryPacketConn, *telemetryPacketConn) {
	toB := make(chan []byte)
	toA := make(chan []byte)
	closed := make(chan struct{})
	once := &sync.Once{}

	a := &telemetryPacketConn{recv: toA, send: toB, closed: closed, closeOnce: once}
	b := &telemetryPacketConn{recv: toB, send: toA, closed: closed, closeOnce: once}
	return a, b
}

func (c *telemetryPacketConn) Read(p []byte) (int, error) {
	select {
	case pkt := <-c.recv:
		return copy(p, pkt), nil
	case <-c.closed:
		return 0, io.EOF
	}
}

func (c *telemetryPacketConn) Write(p []byte) (int, error) {
	pkt := append([]byte{}, p...)
	select {
	case c.send <- pkt:
		return len(p), nil
	case <-c.closed:
		return 0, io.EOF
	}
}

func (c *telemetryPacketConn) Close() error {
	c.closeOnce.Do(func() { close(c.closed) })
	return nil
}

func (c *telemetryPacketConn) LocalAddr() net.Addr              { return telemetryFakeAddr("mem-a") }
func (c *telemetryPacketConn) RemoteAddr() net.Addr             { return telemetryFakeAddr("mem-b") }
func (c *telemetryPacketConn) SetDeadline(time.Time) error      { return nil }
func (c *telemetryPacketConn) SetReadDeadline(time.Time) error  { return nil }
func (c *telemetryPacketConn) SetWriteDeadline(time.Time) error { return nil }

func telemetrySetupDatagram(t *testing.T) (*EndpointCustomClient, func([]byte) error) {
	t.Helper()

	peer, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err == nil {
		local, err2 := net.DialUDP("udp4", nil, peer.LocalAddr().(*net.UDPAddr))
		if err2 != nil {
			peer.Close()
			t.Fatalf("UDP local dial: %v", err2)
		}
		dst := local.LocalAddr().(*net.UDPAddr)
		t.Cleanup(func() {
			local.Close()
			peer.Close()
		})

		return &EndpointCustomClient{
				Label:      "telemetry-udp",
				IsDatagram: true,
				Connect: func(context.Context) (net.Conn, error) {
					return local, nil
				},
			}, func(data []byte) error {
				_, err := peer.WriteToUDP(data, dst)
				return err
			}
	}

	if !errors.Is(err, syscall.EPERM) {
		t.Fatalf("UDP peer listen: %v", err)
	}

	t.Logf("loopback UDP bind unavailable (%v); using in-memory datagram pair", err)
	client, peerConn := newTelemetryPacketPair()
	t.Cleanup(func() { peerConn.Close() })

	return &EndpointCustomClient{
			Label:      "telemetry-udp-memory",
			IsDatagram: true,
			Connect: func(context.Context) (net.Conn, error) {
				return client, nil
			},
		}, func(data []byte) error {
			_, err := peerConn.Write(data)
			return err
		}
}

func telemetryNextEvents(t *testing.T, node *Node, count int, what string) []Event {
	t.Helper()

	events := make([]Event, 0, count)
	timer := time.NewTimer(telemetryEventBarrier)
	defer timer.Stop()

	for len(events) < count {
		select {
		case ev := <-node.Events():
			if ev == nil {
				t.Fatalf("%s: event stream closed after %d/%d events", what, len(events), count)
			}
			events = append(events, ev)
		case <-timer.C:
			t.Fatalf("%s: timed out waiting for %d/%d events", what, len(events), count)
		}
	}
	return events
}

func telemetryRequireNoEvent(t *testing.T, node *Node, what string) {
	t.Helper()
	select {
	case ev := <-node.Events():
		t.Fatalf("%s: unexpected extra event %T after the expected sequence", what, ev)
	default:
	}
}

func telemetryAssertFrame(t *testing.T, ev Event, wantCh *Channel, src telemetrySource, wantID uint32, what string) *EventFrame {
	t.Helper()

	frameEvent, ok := ev.(*EventFrame)
	if !ok {
		t.Fatalf("%s: got %T, want *EventFrame", what, ev)
	}
	if frameEvent.Channel != wantCh {
		t.Fatalf("%s: event was attributed to the wrong channel", what)
	}
	if frameEvent.SystemID() != src.systemID || frameEvent.ComponentID() != src.componentID {
		t.Fatalf("%s: event crossed sources: got system/component %d/%d, want %d/%d",
			what, frameEvent.SystemID(), frameEvent.ComponentID(), src.systemID, src.componentID)
	}
	if frameEvent.Message().GetID() != wantID {
		t.Fatalf("%s: got message id %d, want %d", what, frameEvent.Message().GetID(), wantID)
	}
	return frameEvent
}

func telemetryAssertParseError(t *testing.T, ev Event, wantCh *Channel, what string) *EventParseError {
	t.Helper()

	parseError, ok := ev.(*EventParseError)
	if !ok {
		t.Fatalf("%s: got %T, want *EventParseError", what, ev)
	}
	if parseError.Channel != wantCh {
		t.Fatalf("%s: parse error was attributed to the wrong channel", what)
	}
	return parseError
}

func telemetrySampleFromMessage(msg *common.MessageGpsRawInt) telemetrySample {
	return telemetrySample{
		timeUsec:     msg.TimeUsec,
		lat:          msg.Lat,
		lon:          msg.Lon,
		alt:          msg.Alt,
		eph:          msg.Eph,
		epv:          msg.Epv,
		vel:          msg.Vel,
		cog:          msg.Cog,
		fixType:      byte(msg.FixType),
		satellites:   msg.SatellitesVisible,
		altEllipsoid: msg.AltEllipsoid,
		hAcc:         msg.HAcc,
		vAcc:         msg.VAcc,
		velAcc:       msg.VelAcc,
		hdgAcc:       msg.HdgAcc,
		yaw:          msg.Yaw,
	}
}

func telemetryBaseFields(s telemetrySample) telemetrySample {
	s.altEllipsoid = 0
	s.hAcc = 0
	s.vAcc = 0
	s.velAcc = 0
	s.hdgAcc = 0
	s.yaw = 0
	return s
}

func telemetryAssertGPS(t *testing.T, ev Event, wantCh *Channel, src telemetrySource, want telemetrySample, extensionsPresent bool, what string) {
	t.Helper()

	frameEvent := telemetryAssertFrame(t, ev, wantCh, src, telemetryGpsRawIntID, what)
	msg, ok := frameEvent.Message().(*common.MessageGpsRawInt)
	if !ok {
		t.Fatalf("%s: message decoded as %T, want *common.MessageGpsRawInt", what, frameEvent.Message())
	}

	got := telemetrySampleFromMessage(msg)

	if !extensionsPresent &&
		(got.altEllipsoid != 0 || got.hAcc != 0 || got.vAcc != 0 ||
			got.velAcc != 0 || got.hdgAcc != 0 || got.yaw != 0) {
		t.Fatalf("%s: omitted extension field decoded as non-zero: "+
			"altEllipsoid=%d hAcc=%d vAcc=%d velAcc=%d hdgAcc=%d yaw=%d",
			what, got.altEllipsoid, got.hAcc, got.vAcc, got.velAcc, got.hdgAcc, got.yaw)
	}

	if got != want {
		t.Fatalf("%s: GPS_RAW_INT field mismatch: got %+v want %+v", what, got, want)
	}
}

func telemetryAssertRaw(t *testing.T, ev Event, wantCh *Channel, src telemetrySource, wantID uint32, wantPayload []byte, what string) {
	t.Helper()

	frameEvent := telemetryAssertFrame(t, ev, wantCh, src, wantID, what)
	raw, ok := frameEvent.Message().(*message.MessageRaw)
	if !ok {
		t.Fatalf("%s: unknown-id message decoded as %T, want *message.MessageRaw", what, frameEvent.Message())
	}
	if raw.ID != wantID {
		t.Fatalf("%s: raw message id is %d, want %d", what, raw.ID, wantID)
	}
	if !bytes.Equal(raw.Payload, wantPayload) {
		t.Fatalf("%s: raw payload is % x, want % x", what, raw.Payload, wantPayload)
	}
}

func telemetryStartStreamWrite(conn net.Conn, data []byte) <-chan error {
	errCh := make(chan error, 1)
	go func() {
		// net.Pipe writes block until the receiver consumes the bytes; the
		// Node may pause to deliver an event between coalesced frames, so the
		// write has to run concurrently with the event barrier.
		_ = conn.SetWriteDeadline(time.Now().Add(telemetryEventBarrier))
		_, err := conn.Write(data)
		errCh <- err
	}()
	return errCh
}

func telemetryFinishStreamWrite(t *testing.T, errCh <-chan error, what string) {
	t.Helper()
	select {
	case err := <-errCh:
		if err != nil {
			t.Fatalf("%s: stream write failed: %v", what, err)
		}
	case <-time.After(telemetryEventBarrier):
		t.Fatalf("%s: stream write did not complete", what)
	}
}

func telemetrySendDatagram(t *testing.T, send func([]byte) error, data []byte, what string) {
	t.Helper()
	if err := send(data); err != nil {
		t.Fatalf("%s: datagram write failed: %v", what, err)
	}
}

// telemetryReadV2Wire reads one V2 frame from the peer side and verifies its
// checksum with the independent reference implementation.
func telemetryReadV2Wire(t *testing.T, conn net.Conn, crcExtra byte, what string) (sys, comp, seq byte, id uint32, payload []byte) {
	t.Helper()

	if err := conn.SetReadDeadline(time.Now().Add(telemetryEventBarrier)); err != nil {
		t.Fatalf("%s: setting read deadline: %v", what, err)
	}

	var magic [1]byte
	if _, err := io.ReadFull(conn, magic[:]); err != nil {
		t.Fatalf("%s: reading magic: %v", what, err)
	}
	if magic[0] != frame.V2MagicByte {
		t.Fatalf("%s: wire magic is %#x, want %#x", what, magic[0], frame.V2MagicByte)
	}

	header := make([]byte, 9)
	if _, err := io.ReadFull(conn, header); err != nil {
		t.Fatalf("%s: reading header: %v", what, err)
	}
	if int(header[0]) > telemetryGpsExtendedSize {
		t.Fatalf("%s: encoded payload length %d exceeds extended size %d",
			what, header[0], telemetryGpsExtendedSize)
	}

	seq = header[3]
	sys = header[4]
	comp = header[5]
	id = uint32(header[8])<<16 | uint32(header[7])<<8 | uint32(header[6])

	payload = make([]byte, int(header[0]))
	if _, err := io.ReadFull(conn, payload); err != nil {
		t.Fatalf("%s: reading payload: %v", what, err)
	}

	var checksumBytes [2]byte
	if _, err := io.ReadFull(conn, checksumBytes[:]); err != nil {
		t.Fatalf("%s: reading checksum: %v", what, err)
	}
	checksum := binary.LittleEndian.Uint16(checksumBytes[:])

	crcInput := append(append([]byte{}, header...), payload...)
	if want := telemetryRefChecksum(crcInput, crcExtra); checksum != want {
		t.Fatalf("%s: wire checksum is %.4x, reference checksum is %.4x", what, checksum, want)
	}

	return sys, comp, seq, id, payload
}

func telemetryReferenceDecodeGPS(t *testing.T, payload []byte, what string) telemetrySample {
	t.Helper()

	if len(payload) > telemetryGpsExtendedSize {
		t.Fatalf("%s: payload length %d exceeds GPS_RAW_INT extended size", what, len(payload))
	}
	if len(payload) < telemetryGpsExtendedSize {
		payload = append(payload, make([]byte, telemetryGpsExtendedSize-len(payload))...)
	}

	return telemetrySample{
		timeUsec:     binary.LittleEndian.Uint64(payload[0:8]),
		lat:          int32(binary.LittleEndian.Uint32(payload[8:12])),
		lon:          int32(binary.LittleEndian.Uint32(payload[12:16])),
		alt:          int32(binary.LittleEndian.Uint32(payload[16:20])),
		eph:          binary.LittleEndian.Uint16(payload[20:22]),
		epv:          binary.LittleEndian.Uint16(payload[22:24]),
		vel:          binary.LittleEndian.Uint16(payload[24:26]),
		cog:          binary.LittleEndian.Uint16(payload[26:28]),
		fixType:      payload[28],
		satellites:   payload[29],
		altEllipsoid: int32(binary.LittleEndian.Uint32(payload[30:34])),
		hAcc:         binary.LittleEndian.Uint32(payload[34:38]),
		vAcc:         binary.LittleEndian.Uint32(payload[38:42]),
		velAcc:       binary.LittleEndian.Uint32(payload[42:46]),
		hdgAcc:       binary.LittleEndian.Uint32(payload[46:50]),
		yaw:          binary.LittleEndian.Uint16(payload[50:52]),
	}
}

func TestTelemetryIngressContract(t *testing.T) {
	if got := telemetryRefCRCExtra("GPS_RAW_INT", telemetryGpsRawIntReferenceFields); got != telemetryGpsRawIntCRCExtra {
		t.Fatalf("independent GPS_RAW_INT CRC extra is %d, MAVLink defines %d", got, telemetryGpsRawIntCRCExtra)
	}

	payloads := telemetryBuildPayloads()

	streamSource := telemetrySource{systemID: telemetryStreamSystemID, componentID: telemetryStreamComponentID}
	udpSource := telemetrySource{systemID: telemetryUDPSystemID, componentID: telemetryUDPComponentID}
	streamFrames := telemetryBuildFrames(streamSource, 0x40, payloads)
	udpFrames := telemetryBuildFrames(udpSource, 0x70, payloads)

	// Stream channel: real net.Pipe, with a wrapper that caps every Read so
	// headers/payloads/checksums are necessarily split and coalesced frames
	// stay adjacent on the wire.
	pipeNode, pipePeer := net.Pipe()
	defer pipePeer.Close()
	fragConn := &telemetryFragmentConn{Conn: pipeNode, fragSize: telemetryFragmentSize}
	streamEndpoint := &EndpointCustomClient{
		Label: "telemetry-stream",
		Connect: func(context.Context) (net.Conn, error) {
			return fragConn, nil
		},
	}

	// Datagram channel: actual loopback UDP where local binding is available,
	// one packet per Read.
	udpEndpoint, sendDatagramData := telemetrySetupDatagram(t)

	node := &Node{
		Dialect:          common.Dialect,
		OutVersion:       V2,
		OutSystemID:      1,
		HeartbeatDisable: true,
		Endpoints:        []Endpoint{streamEndpoint, udpEndpoint},
	}
	if err := node.Initialize(); err != nil {
		t.Fatalf("node initialize: %v", err)
	}
	defer node.Close()

	openEvents := telemetryNextEvents(t, node, 2, "channel-open barrier")
	var streamCh, udpCh *Channel
	for _, ev := range openEvents {
		openEvent, ok := ev.(*EventChannelOpen)
		if !ok {
			t.Fatalf("channel-open barrier: got %T, want *EventChannelOpen", ev)
		}
		switch openEvent.Channel.Endpoint() {
		case streamEndpoint:
			streamCh = openEvent.Channel
		case udpEndpoint:
			udpCh = openEvent.Channel
		default:
			t.Fatalf("channel-open barrier: unexpected endpoint %T", openEvent.Channel.Endpoint())
		}
	}
	if streamCh == nil || udpCh == nil {
		t.Fatalf("channel-open barrier: stream=%v udp=%v", streamCh, udpCh)
	}

	t.Run("stream_fragmented_and_coalesced", func(t *testing.T) {
		// Two complete frames in a single peer Write; the reader must return
		// them in order, even though its Reads are capped at 3 bytes.
		writeDone := telemetryStartStreamWrite(pipePeer,
			append(append([]byte{}, streamFrames.v2Extended...), streamFrames.v2NoExt...))

		events := telemetryNextEvents(t, node, 2, "stream coalesced frame barrier")
		telemetryFinishStreamWrite(t, writeDone, "stream coalesced frames")
		telemetryAssertGPS(t, events[0], streamCh, streamSource, telemetryExtendedSample, true,
			"stream first coalesced frame")
		telemetryAssertGPS(t, events[1], streamCh, streamSource, telemetryNoExtSample, false,
			"stream second coalesced frame")

		reads := fragConn.readSizes()
		var total int
		maxRead := 0
		for _, n := range reads {
			total += n
			if n > maxRead {
				maxRead = n
			}
		}
		if maxRead > telemetryFragmentSize {
			t.Fatalf("stream fragmentation: a Read returned %d bytes, cap is %d",
				maxRead, telemetryFragmentSize)
		}
		if len(reads) < 8 {
			t.Fatalf("stream fragmentation: only %d Read calls for %d bytes", len(reads), total)
		}
		if total != len(streamFrames.v2Extended)+len(streamFrames.v2NoExt) {
			t.Fatalf("stream fragmentation: read %d bytes, wire contained %d",
				total, len(streamFrames.v2Extended)+len(streamFrames.v2NoExt))
		}

		// A known message with a corrupted checksum must not be published.
		writeDone = telemetryStartStreamWrite(pipePeer, streamFrames.badCRC)
		events = telemetryNextEvents(t, node, 1, "stream bad CRC barrier")
		telemetryFinishStreamWrite(t, writeDone, "stream bad CRC")
		if frameEvent, ok := events[0].(*EventFrame); ok {
			t.Fatalf("known message with bad CRC was published instead of EventParseError "+
				"(id=%d source=%d/%d)", frameEvent.Message().GetID(), frameEvent.SystemID(), frameEvent.ComponentID())
		}
		telemetryAssertParseError(t, events[0], streamCh, "stream bad CRC")
		telemetryRequireNoEvent(t, node, "stream bad CRC")

		// The next valid frame is still ingested, and a V2 payload that omits
		// its trailing zero base byte decodes that byte as zero.
		writeDone = telemetryStartStreamWrite(pipePeer, streamFrames.v2TrailingZero)
		events = telemetryNextEvents(t, node, 1, "stream trailing-zero barrier")
		telemetryFinishStreamWrite(t, writeDone, "stream trailing-zero frame")
		telemetryAssertGPS(t, events[0], streamCh, streamSource, telemetryTrailingZeroSample, false,
			"stream trailing-zero frame")

		// V1 frames carry only the base field layout; extensions are absent.
		writeDone = telemetryStartStreamWrite(pipePeer, streamFrames.v1Base)
		events = telemetryNextEvents(t, node, 1, "stream V1 barrier")
		telemetryFinishStreamWrite(t, writeDone, "stream V1 frame")
		telemetryAssertGPS(t, events[0], streamCh, streamSource, telemetryNoExtSample, false,
			"stream V1 frame")
		if _, ok := events[0].(*EventFrame).Frame.(*frame.V1Frame); !ok {
			t.Fatalf("stream V1 frame: decoded as %T, want *frame.V1Frame",
				events[0].(*EventFrame).Frame)
		}

		// Unknown IDs have no CRC extra; they must be delivered as MessageRaw
		// with the original id and payload.
		writeDone = telemetryStartStreamWrite(pipePeer, streamFrames.unknown)
		events = telemetryNextEvents(t, node, 1, "stream unknown-id barrier")
		telemetryFinishStreamWrite(t, writeDone, "stream unknown id")
		telemetryAssertRaw(t, events[0], streamCh, streamSource, telemetryUnknownID, payloads.unknown,
			"stream unknown id")
	})

	t.Run("datagram_one_frame_per_packet", func(t *testing.T) {
		// A packet with two frames: only the first is delivered. The bytes of
		// the second stay unconsumed until the next packet arrives.
		twoFrames := append(append([]byte{}, udpFrames.v2Extended...), udpFrames.v2NoExt...)
		telemetrySendDatagram(t, sendDatagramData, twoFrames, "datagram two-frame packet")

		events := telemetryNextEvents(t, node, 1, "datagram first-frame barrier")
		telemetryAssertGPS(t, events[0], udpCh, udpSource, telemetryExtendedSample, true,
			"datagram first frame of packet")
		telemetryRequireNoEvent(t, node, "datagram first frame of packet")

		// Next packet: the reader reports the previous packet's unconsumed
		// bytes and then delivers the new packet's frame. The old second frame
		// must not leak into this delivery.
		telemetrySendDatagram(t, sendDatagramData, udpFrames.v2Other, "datagram second packet")

		events = telemetryNextEvents(t, node, 2, "datagram second-packet barrier")
		parseError := telemetryAssertParseError(t, events[0], udpCh, "datagram skipped-bytes report")
		wantSkipped := fmt.Sprintf("skipped %d bytes", len(udpFrames.v2NoExt))
		if !strings.Contains(parseError.Error.Error(), wantSkipped) {
			t.Fatalf("datagram skipped-bytes report: got %q, want %q", parseError.Error.Error(), wantSkipped)
		}
		if _, ok := events[1].(*EventFrame); !ok {
			t.Fatalf("datagram second packet: got %T, want *EventFrame", events[1])
		}
		if got := telemetrySampleFromMessage(events[1].(*EventFrame).Message().(*common.MessageGpsRawInt)); telemetryBaseFields(got) != telemetryBaseFields(telemetryOtherSample) {
			t.Fatalf("datagram leftover bytes leaked into next packet: decoded %+v, want %+v",
				got, telemetryOtherSample)
		}
		telemetryAssertGPS(t, events[1], udpCh, udpSource, telemetryOtherSample, false,
			"datagram second packet frame")

		// A truncated datagram must not be completed by the next packet.
		telemetrySendDatagram(t, sendDatagramData, udpFrames.truncated, "datagram truncated packet")
		events = telemetryNextEvents(t, node, 1, "datagram truncated barrier")
		telemetryAssertParseError(t, events[0], udpCh, "datagram truncated packet")
		telemetryRequireNoEvent(t, node, "datagram truncated packet")

		telemetrySendDatagram(t, sendDatagramData, udpFrames.v2AfterTrunc, "datagram recovery packet")
		// The six bytes still buffered from the truncated packet (all but its
		// magic byte) are discarded and reported before the new packet's frame
		// is delivered: truncation is never completed by the next packet.
		events = telemetryNextEvents(t, node, 2, "datagram recovery barrier")
		parseError = telemetryAssertParseError(t, events[0], udpCh, "datagram truncated leftovers")
		if !strings.HasPrefix(parseError.Error.Error(), "skipped ") ||
			!strings.HasSuffix(parseError.Error.Error(), " bytes") {
			t.Fatalf("datagram truncated leftovers: got %q, want a skipped-bytes report",
				parseError.Error.Error())
		}
		telemetryAssertGPS(t, events[1], udpCh, udpSource, telemetryAfterTruncSample, true,
			"datagram recovery frame")
	})

	t.Run("public_write_path_compatibility", func(t *testing.T) {
		if got := telemetryRefCRCExtra("VFR_HUD", telemetryVfrHudReferenceFields); got != telemetryVfrHudCRCExtra {
			t.Fatalf("independent VFR_HUD CRC extra is %d, MAVLink defines %d", got, telemetryVfrHudCRCExtra)
		}

		want := telemetryExtendedSample
		msg := &common.MessageGpsRawInt{
			TimeUsec:          want.timeUsec,
			FixType:           common.GPS_FIX_TYPE(want.fixType),
			Lat:               want.lat,
			Lon:               want.lon,
			Alt:               want.alt,
			Eph:               want.eph,
			Epv:               want.epv,
			Vel:               want.vel,
			Cog:               want.cog,
			SatellitesVisible: want.satellites,
			AltEllipsoid:      want.altEllipsoid,
			HAcc:              want.hAcc,
			VAcc:              want.vAcc,
			VelAcc:            want.velAcc,
			HdgAcc:            want.hdgAcc,
			Yaw:               want.yaw,
		}

		if err := node.WriteMessageTo(streamCh, msg); err != nil {
			t.Fatalf("public write path: WriteMessageTo: %v", err)
		}

		sys, comp, seq, id, payload := telemetryReadV2Wire(t, pipePeer, telemetryGpsRawIntCRCExtra,
			"public write path")
		if id != telemetryGpsRawIntID {
			t.Fatalf("public write path: message id is %d, want %d", id, telemetryGpsRawIntID)
		}
		if sys != node.OutSystemID || comp != node.OutComponentID {
			t.Fatalf("public write path: source %d/%d, want %d/%d",
				sys, comp, node.OutSystemID, node.OutComponentID)
		}
		_ = seq

		decoded := telemetryReferenceDecodeGPS(t, payload, "public write path")
		if decoded != want {
			t.Fatalf("public write path: reference decode got %+v, want %+v", decoded, want)
		}
	})
}
