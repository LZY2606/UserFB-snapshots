package srt

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"net"
	"os"
	"sync"
	"time"

	"github.com/datarhei/gosrt/circular"
	"github.com/datarhei/gosrt/packet"
	"github.com/datarhei/gosrt/rand"
)

// rendezvous roles, decided by the signed 32-bit cookie contest.
const (
	rzRoleUndecided = 0
	rzRoleInitiator = 1
	rzRoleResponder = 2
)

// rendezvous handshake phases. Each phase knows the single packet that is
// currently due, so it can be retransmitted without replaying a fixed call
// sequence.
const (
	rzPhaseWavehand   = 0 // WAVEHAND, no extensions
	rzPhaseConclusion = 1 // initiator: CONCLUSION+HSREQ, responder: plain CONCLUSION
	rzPhaseAgreement  = 2 // initiator: AGREEMENT; responder waits for AGREEMENT
)

// rendezvousRetransmitInterval is the fixed retransmission interval for the
// currently due WAVEHAND/CONCLUSION message.
const rendezvousRetransmitInterval = 250 * time.Millisecond

// rendezvousConfigError rejects configurations outside the supported scope
// before any UDP resource is created.
func rendezvousConfigError(config *Config) error {
	if len(config.Passphrase) != 0 {
		return fmt.Errorf("rendezvous: encrypted connections are not supported")
	}

	if len(config.PacketFilter) != 0 {
		return fmt.Errorf("rendezvous: packet filter/FEC is not supported")
	}

	if len(config.StreamId) != 0 {
		return fmt.Errorf("rendezvous: StreamId is not supported")
	}

	if config.MSS < MIN_MSS_SIZE || config.MSS > MAX_MSS_SIZE {
		return fmt.Errorf("rendezvous: MSS must be between %d and %d (both inclusive)", MIN_MSS_SIZE, MAX_MSS_SIZE)
	}

	if config.FC == 0 {
		return fmt.Errorf("rendezvous: FC must not be zero")
	}

	return nil
}

// parseRendezvousAddress validates that address is an IPv4 host:port pair with
// a non-zero port, as required for both endpoints of a rendezvous attempt.
func parseRendezvousAddress(address, label string) (*net.UDPAddr, error) {
	host, port, err := net.SplitHostPort(address)
	if err != nil {
		return nil, fmt.Errorf("rendezvous: invalid %s address: %w", label, err)
	}

	ip := net.ParseIP(host)
	if ip == nil || ip.To4() == nil {
		return nil, fmt.Errorf("rendezvous: %s address must be an IPv4 address", label)
	}

	if port == "0" {
		return nil, fmt.Errorf("rendezvous: %s address must use a non-zero port", label)
	}

	addr, err := net.ResolveUDPAddr("udp4", net.JoinHostPort(ip.To4().String(), port))
	if err != nil {
		return nil, fmt.Errorf("rendezvous: invalid %s address: %w", label, err)
	}

	if addr.Port == 0 {
		return nil, fmt.Errorf("rendezvous: %s address must use a non-zero port", label)
	}

	return addr, nil
}

// rendezvouser implements the Conn interface for a live SRT v5 rendezvous
// connection. Both sides bind a local UDP address and talk only to the single
// configured peer; no listener is involved.
type rendezvouser struct {
	pc *net.UDPConn

	localAddr  *net.UDPAddr
	remoteAddr *net.UDPAddr

	config Config

	start time.Time

	socketId                    uint32
	cookie                      uint32
	initialPacketSequenceNumber circular.Number

	lock sync.Mutex // guards the handshake negotiation state

	role  int
	phase int

	// hsrspSent records that the responder has already answered an HSREQ with
	// HSRSP. From that point its retransmitted stage packet is the HSRSP
	// CONCLUSION, and a media packet is allowed to confirm the connection.
	hsrspSent bool

	peerSocketId uint32
	peerCookie   uint32
	havePeer     bool

	// Negotiated parameters, fixed once the peer's first message is accepted.
	negotiated         bool
	peerISN            circular.Number
	peerMSS            uint32
	peerFC             uint32
	peerRecvLatency    uint16
	peerSendLatency    uint16
	effectiveMSS       uint32
	effectiveFC        uint32
	effectiveRecvDelay uint16 // local receive direction, milliseconds
	effectiveSendDelay uint16 // local send direction, milliseconds

	conn     *srtConn
	connLock sync.RWMutex

	rcvQueue chan packet.Packet
	doneChan chan error

	sndMutex sync.Mutex
	sndData  bytes.Buffer

	shutdown     bool
	shutdownLock sync.RWMutex
	shutdownOnce sync.Once
	stopReader   context.CancelFunc

	// arrival signals that a packet from the peer was accepted into the state
	// machine. The handshake loop is driven by this signal and a retransmit
	// ticker; it never sleeps to guess event ordering.
	arrival chan struct{}

	// stateSignal is broadcast (non-blocking) whenever the handshake state
	// changes, so tests can wait on concrete events instead of polling.
	stateSignal chan struct{}

	// wireWrite, when set, replaces the UDP socket write. It is a seam for
	// driving the state machine deterministically in tests.
	wireWrite func(b []byte) error

	failOnce sync.Once
	failed   bool
}

// RendezvousWithContext establishes an SRT v5 live rendezvous connection. Both
// endpoints bind their own local IPv4 UDP address and connect to each other;
// neither side acts as a listener.
//
// localAddress and remoteAddress are IPv4 "host:port" pairs, both with a
// non-zero port. The attempt is constrained by ctx and Config.ConnectionTimeout.
// Only unencrypted live connections without FEC and StreamId are supported.
// On failure the locally created UDP socket is released; on success the
// returned Conn owns it and is closed with Conn.Close.
func RendezvousWithContext(ctx context.Context, localAddress, remoteAddress string, config Config) (Conn, error) {
	if ctx == nil {
		return nil, fmt.Errorf("context must not be nil")
	}

	if err := rendezvousConfigError(&config); err != nil {
		return nil, err
	}

	if err := config.Validate(); err != nil {
		return nil, fmt.Errorf("invalid config: %w", err)
	}

	if config.Logger == nil {
		config.Logger = NewLogger(nil)
	}

	localUDPAddr, err := parseRendezvousAddress(localAddress, "local")
	if err != nil {
		return nil, err
	}

	remoteUDPAddr, err := parseRendezvousAddress(remoteAddress, "remote")
	if err != nil {
		return nil, err
	}

	rz := &rendezvouser{
		localAddr:   localUDPAddr,
		remoteAddr:  remoteUDPAddr,
		config:      config,
		role:        rzRoleUndecided,
		phase:       rzPhaseWavehand,
		arrival:     make(chan struct{}, 1),
		stateSignal: make(chan struct{}, 16),
	}

	pc, err := net.DialUDP("udp4", localUDPAddr, remoteUDPAddr)
	if err != nil {
		return nil, fmt.Errorf("rendezvous: failed to bind %s: %w", localAddress, err)
	}

	rz.pc = pc

	if rz.socketId, err = rand.Uint32(); err != nil {
		rz.closeSocket()
		return nil, err
	}

	if rz.cookie, err = rand.Uint32(); err != nil {
		rz.closeSocket()
		return nil, err
	}

	seqNum, err := rand.Uint32()
	if err != nil {
		rz.closeSocket()
		return nil, err
	}

	rz.initialPacketSequenceNumber = circular.New(seqNum&packet.MAX_SEQUENCENUMBER, packet.MAX_SEQUENCENUMBER)

	rz.rcvQueue = make(chan packet.Packet, 2048)
	rz.doneChan = make(chan error, 1)
	rz.start = time.Now()

	var readerCtx context.Context
	readerCtx, rz.stopReader = context.WithCancel(context.Background())

	go rz.readWire(readerCtx)
	go rz.reader(readerCtx)

	conn, err := rz.handshake(ctx)
	if err != nil {
		rz.Close()
		return nil, err
	}

	rz.connLock.Lock()
	rz.conn = conn
	rz.connLock.Unlock()

	return rz, nil
}

// closeSocket releases the UDP socket without touching an established srtConn.
func (rz *rendezvouser) closeSocket() {
	if rz.pc != nil {
		rz.pc.Close()
	}
}

// newRendezvousForTest builds a rendezvouser without binding a UDP socket. The
// caller drives the state machine directly and provides a wireWrite sink. It is
// intended for the in-process handshake tests only.
func newRendezvousForTest(config Config, localAddr, remoteAddr *net.UDPAddr, socketId, cookie uint32, isn circular.Number, wireWrite func([]byte) error) *rendezvouser {
	if config.Logger == nil {
		config.Logger = NewLogger(nil)
	}

	rz := &rendezvouser{
		localAddr:                   localAddr,
		remoteAddr:                  remoteAddr,
		config:                      config,
		start:                       time.Now(),
		socketId:                    socketId,
		cookie:                      cookie,
		initialPacketSequenceNumber: isn,
		role:                        rzRoleUndecided,
		phase:                       rzPhaseWavehand,
		arrival:                     make(chan struct{}, 1),
		rcvQueue:                    make(chan packet.Packet, 2048),
		doneChan:                    make(chan error, 1),
		stateSignal:                 make(chan struct{}, 16),
		wireWrite:                   wireWrite,
	}

	var readerCtx context.Context
	readerCtx, rz.stopReader = context.WithCancel(context.Background())
	go rz.reader(readerCtx)

	return rz
}

func (rz *rendezvouser) connNow() *srtConn {
	rz.connLock.RLock()
	defer rz.connLock.RUnlock()
	return rz.conn
}

// readWire reads datagrams from the socket. The socket is connected to the
// single configured peer, so every datagram necessarily originates from it;
// packets from any other source cannot arrive on this socket.
func (rz *rendezvouser) readWire(ctx context.Context) {
	buffer := make([]byte, MAX_MSS_SIZE)

	for {
		if rz.isShutdown() {
			select {
			case rz.doneChan <- ErrClientClosed:
			default:
			}
			return
		}

		rz.pc.SetReadDeadline(time.Now().Add(3 * time.Second))
		n, err := rz.pc.Read(buffer)
		if err != nil {
			if errors.Is(err, os.ErrDeadlineExceeded) {
				if ctx.Err() != nil {
					return
				}
				continue
			}

			if rz.isShutdown() {
				select {
				case rz.doneChan <- ErrClientClosed:
				default:
				}
				return
			}

			select {
			case rz.doneChan <- err:
			default:
			}
			return
		}

		p, err := packet.NewPacketFromData(rz.remoteAddr, buffer[:n])
		if err != nil {
			continue
		}

		select {
		case rz.rcvQueue <- p:
		default:
			rz.log("rendezvous", func() string { return "receive queue is full" })
		}
	}
}

// reader dispatches packets: handshake packets drive the rendezvous state
// machine, while every other packet goes to the established connection. Once
// the connection exists, late handshake packets are still delivered to the
// state machine and media packets are never consumed by the handshake path.
func (rz *rendezvouser) reader(ctx context.Context) {
	for {
		select {
		case <-ctx.Done():
			return
		case p := <-rz.rcvQueue:
			if rz.isShutdown() {
				continue
			}

			rz.log("packet:recv:dump", func() string { return p.Dump() })

			if p.Header().IsControlPacket && p.Header().ControlType == packet.CTRLTYPE_HANDSHAKE {
				rz.handleHandshake(p)
				continue
			}

			if conn := rz.connNow(); conn != nil {
				conn.push(p)
			} else {
				// Before the connection exists, a data or connection-stage
				// control packet can only confirm a waiting responder.
				rz.handleEarlyPacket(p)
			}
		}
	}
}

// handshake runs the retransmitting state machine until either a Conn is
// established or the attempt fails. Retransmission is ticker driven; progress
// is driven by packet arrival signals, not by sleeping.
func (rz *rendezvouser) handshake(ctx context.Context) (*srtConn, error) {
	timeout := time.NewTimer(rz.config.ConnectionTimeout)
	defer timeout.Stop()

	retransmit := time.NewTicker(rendezvousRetransmitInterval)
	defer retransmit.Stop()

	// Send the first WAVEHAND immediately.
	rz.sendCurrentPacket()

	for {
		select {
		case <-ctx.Done():
			return nil, ctx.Err()

		case err := <-rz.doneChan:
			return nil, err

		case <-timeout.C:
			if err := ctx.Err(); err != nil {
				return nil, err
			}
			return nil, fmt.Errorf("rendezvous: connection timeout, peer didn't respond")

		case <-retransmit.C:
			// Retransmit the packet due for the current phase.
			rz.sendCurrentPacket()

		case <-rz.arrival:
			if conn := rz.connNow(); conn != nil {
				return conn, nil
			}
		}
	}
}

// signalArrival wakes the handshake loop after a packet from the peer was
// accepted into the state machine.
func (rz *rendezvouser) signalArrival() {
	select {
	case rz.arrival <- struct{}{}:
	default:
	}
}

// signalState announces a handshake state change to anyone watching
// stateSignal. It never blocks and does not require every change to be drained.
func (rz *rendezvouser) signalState() {
	if rz.stateSignal == nil {
		return
	}
	select {
	case rz.stateSignal <- struct{}{}:
	default:
	}
}

// sendCurrentPacket sends the message due for the current phase. Repeated calls
// are safe; each builds a fresh packet from the stable local identity.
func (rz *rendezvouser) sendCurrentPacket() {
	rz.lock.Lock()
	role := rz.role
	phase := rz.phase
	negotiated := rz.negotiated
	rz.lock.Unlock()

	if !negotiated {
		// Before the peer is known there is nothing but WAVEHAND to send.
		rz.sendWavehand()
		return
	}

	switch phase {
	case rzPhaseWavehand:
		rz.sendWavehand()
	case rzPhaseConclusion:
		if role == rzRoleInitiator {
			rz.sendConclusion(true)
		} else if rz.hsrspSent {
			// Once HSREQ has been answered, keep retransmitting the HSRSP
			// CONCLUSION rather than reverting to a plain CONCLUSION.
			rz.sendConclusionWithHSRSP()
		} else {
			rz.sendConclusion(false)
		}
	case rzPhaseAgreement:
		// The initiator sends AGREEMENT; the responder keeps its plain
		// CONCLUSION flowing until the agreement (or a confirming media packet)
		// arrives.
		if role == rzRoleInitiator {
			rz.sendAgreement()
		} else {
			rz.sendConclusion(false)
		}
	}
}

// baseHandshakePacket creates a handshake packet addressed to the peer, using
// the destination socket ID already learned (0 before that).
func (rz *rendezvouser) baseHandshakePacket() packet.Packet {
	p := packet.NewPacket(rz.remoteAddr)
	p.Header().IsControlPacket = true
	p.Header().ControlType = packet.CTRLTYPE_HANDSHAKE
	p.Header().SubType = 0
	p.Header().TypeSpecific = 0
	p.Header().Timestamp = uint32(time.Since(rz.start).Microseconds())

	rz.lock.Lock()
	dest := rz.peerSocketId
	rz.lock.Unlock()

	p.Header().DestinationSocketId = dest

	return p
}

// newWavehandCIF builds the stable WAVEHAND content. The local identity,
// cookie and ISN never change for the whole attempt.
func (rz *rendezvouser) newWavehandCIF() *packet.CIFHandshake {
	cif := &packet.CIFHandshake{
		IsRequest:                   false,
		Version:                     5,
		EncryptionField:             0,
		ExtensionField:              0,
		InitialPacketSequenceNumber: rz.initialPacketSequenceNumber,
		MaxTransmissionUnitSize:     rz.config.MSS,
		MaxFlowWindowSize:           rz.config.FC,
		HandshakeType:               packet.HSTYPE_WAVEHAND,
		SRTSocketId:                 rz.socketId,
		SynCookie:                   rz.cookie,
	}

	cif.PeerIP.FromNetAddr(rz.localAddr)

	return cif
}

func (rz *rendezvouser) sendWavehand() {
	p := rz.baseHandshakePacket()
	p.MarshalCIF(rz.newWavehandCIF())
	rz.log("handshake:send:dump", func() string { return p.Dump() })
	rz.send(p)
}

// sendConclusion sends a CONCLUSION. The initiator attaches HSREQ; the
// responder sends a plain CONCLUSION without extensions.
func (rz *rendezvouser) sendConclusion(withHSREQ bool) {
	rz.lock.Lock()
	if !rz.negotiated {
		rz.lock.Unlock()
		return
	}
	cif := rz.buildConclusionCIF(withHSREQ)
	rz.lock.Unlock()

	p := rz.baseHandshakePacket()
	p.MarshalCIF(cif)
	rz.log("handshake:send:dump", func() string { return p.Dump() })
	rz.send(p)
}

// buildConclusionCIF must be called with rz.lock held.
func (rz *rendezvouser) buildConclusionCIF(withHSREQ bool) *packet.CIFHandshake {
	cif := &packet.CIFHandshake{
		Version:                     5,
		EncryptionField:             0,
		InitialPacketSequenceNumber: rz.initialPacketSequenceNumber,
		MaxTransmissionUnitSize:     rz.config.MSS,
		MaxFlowWindowSize:           rz.config.FC,
		HandshakeType:               packet.HSTYPE_CONCLUSION,
		SRTSocketId:                 rz.socketId,
		SynCookie:                   rz.cookie,
	}

	cif.PeerIP.FromNetAddr(rz.localAddr)

	if withHSREQ {
		cif.IsRequest = true
		cif.HasHS = true
		cif.SRTHS = rz.localHSExtension()
	}

	return cif
}

// localHSExtension must be called with rz.lock held.
func (rz *rendezvouser) localHSExtension() *packet.CIFHandshakeExtension {
	return &packet.CIFHandshakeExtension{
		SRTVersion: SRT_VERSION,
		SRTFlags: packet.CIFHandshakeExtensionFlags{
			TSBPDSND:    true,
			TSBPDRCV:    true,
			CRYPT:       true,
			TLPKTDROP:   true,
			PERIODICNAK: true,
			REXMITFLG:   true,
			STREAM:      false,
		},
		RecvTSBPDDelay: uint16(rz.config.ReceiverLatency.Milliseconds()),
		SendTSBPDDelay: uint16(rz.config.PeerLatency.Milliseconds()),
	}
}

func (rz *rendezvouser) sendAgreement() {
	rz.lock.Lock()
	cif := rz.buildConclusionCIF(false)
	rz.lock.Unlock()

	cif.HandshakeType = packet.HSTYPE_AGREEMENT
	cif.HasHS = false
	cif.IsRequest = false

	p := rz.baseHandshakePacket()
	p.MarshalCIF(cif)
	rz.log("handshake:send:dump", func() string { return p.Dump() })
	rz.send(p)
}

// handleHandshake is the core of the state machine. It validates identity and
// extension direction, runs the cookie contest, and advances the phase only on
// consistent input. Repeated or late packets never reset established state.
func (rz *rendezvouser) handleHandshake(p packet.Packet) {
	cif := &packet.CIFHandshake{}
	if err := p.UnmarshalCIF(cif); err != nil {
		rz.log("handshake:recv:error", func() string { return err.Error() })
		return
	}

	rz.log("handshake:recv:dump", func() string { return p.Dump() })
	rz.log("handshake:recv:cif", func() string { return cif.String() })

	if cif.HandshakeType.IsRejection() {
		rz.fail(fmt.Errorf("rendezvous: connection rejected: %s", cif.HandshakeType.String()))
		return
	}

	if cif.Version != 5 {
		rz.fail(fmt.Errorf("rendezvous: only SRT v5 is supported, peer sent v%d", cif.Version))
		return
	}

	if cif.EncryptionField != 0 || cif.HasKM || cif.HasSID || len(cif.StreamId) != 0 || cif.HasFilter || len(cif.PacketFilter) != 0 {
		rz.fail(fmt.Errorf("rendezvous: peer advertised unsupported encryption, StreamId or packet filter extensions"))
		return
	}

	switch cif.HandshakeType {
	case packet.HSTYPE_WAVEHAND:
		// WAVEHAND carries no extensions.
		if cif.ExtensionField != 0 || cif.HasHS {
			rz.fail(fmt.Errorf("rendezvous: malformed WAVEHAND with extensions"))
			return
		}
	case packet.HSTYPE_CONCLUSION, packet.HSTYPE_AGREEMENT:
		// validated below
	default:
		rz.fail(fmt.Errorf("rendezvous: unsupported handshake type: %s", cif.HandshakeType.String()))
		return
	}

	rz.lock.Lock()

	// Identity consistency: the peer keeps one socket ID and one cookie for the
	// whole attempt. A changed identity never moves the state forward.
	if rz.havePeer {
		if cif.SRTSocketId != rz.peerSocketId || cif.SynCookie != rz.peerCookie {
			rz.lock.Unlock()
			rz.log("handshake:recv:error", func() string { return "peer identity changed mid-attempt; ignoring" })
			rz.signalArrival()
			return
		}
	} else {
		if cif.SRTSocketId == 0 {
			rz.lock.Unlock()
			rz.fail(fmt.Errorf("rendezvous: peer sent an empty socket ID"))
			return
		}
		rz.peerSocketId = cif.SRTSocketId
		rz.peerCookie = cif.SynCookie
		rz.havePeer = true
	}

	// A handshake packet arriving on a live connection is tolerated: it must
	// never re-run negotiation or the cookie contest and never reset the
	// connection or its media time base. Only the final-confirmation behavior
	// (initiator re-sending AGREEMENT) is applied.
	if rz.conn != nil {
		role := rz.role
		rz.lock.Unlock()
		rz.handleHandshakeLive(cif, role)
		rz.signalState()
		rz.signalArrival()
		return
	}

	if err := rz.recordNegotiation(cif); err != nil {
		rz.lock.Unlock()
		rz.fail(err)
		return
	}

	// Cookie contest on signed 32-bit values: larger cookie is the initiator,
	// smaller is the responder, equality is a definitive failure.
	localCookie := int32(rz.cookie)
	peerCookie := int32(cif.SynCookie)

	switch {
	case localCookie == peerCookie:
		rz.lock.Unlock()
		rz.fail(fmt.Errorf("rendezvous: cookie contest ended in a tie (%#08x)", rz.cookie))
		return
	case localCookie > peerCookie:
		rz.role = rzRoleInitiator
	default:
		rz.role = rzRoleResponder
	}

	role := rz.role
	phase := rz.phase
	hsDirectionOK := rz.validateHSDirection(cif)
	rz.lock.Unlock()

	if !hsDirectionOK {
		rz.log("handshake:recv:error", func() string { return "handshake extension direction inconsistent with role; ignoring" })
		rz.signalArrival()
		return
	}

	switch cif.HandshakeType {
	case packet.HSTYPE_WAVEHAND:
		rz.handleWavehand()
	case packet.HSTYPE_CONCLUSION:
		rz.handleConclusion(cif, role, phase)
	case packet.HSTYPE_AGREEMENT:
		rz.handleAgreement(role)
	}

	rz.signalState()
	rz.signalArrival()
}

// handleHandshakeLive absorbs handshake packets that arrive after the
// connection exists. They never reset the connection or its media time base.
func (rz *rendezvouser) handleHandshakeLive(cif *packet.CIFHandshake, role int) {
	switch cif.HandshakeType {
	case packet.HSTYPE_CONCLUSION:
		// A late CONCLUSION/HSRSP on a connected initiator gets another
		// AGREEMENT so a peer that lost the final confirmation can finish.
		if role == rzRoleInitiator {
			rz.sendAgreement()
		}
	case packet.HSTYPE_AGREEMENT:
		// Duplicate AGREEMENT on a connected responder: nothing to do.
	default:
		// Repeated WAVEHAND on a live connection is harmless.
	}
}

// recordNegotiation fixes the negotiated parameters from the first peer
// message and refuses to move the MSS/FC/ISN baseline on later packets. The
// latency negotiation is refreshed whenever an HS extension is present because
// the first message may be a WAVEHAND without one. Must be called with rz.lock
// held.
func (rz *rendezvouser) recordNegotiation(cif *packet.CIFHandshake) error {
	if cif.MaxTransmissionUnitSize < MIN_MSS_SIZE || cif.MaxTransmissionUnitSize > MAX_MSS_SIZE {
		return fmt.Errorf("rendezvous: peer MSS %d is outside the allowed range %d..%d", cif.MaxTransmissionUnitSize, MIN_MSS_SIZE, MAX_MSS_SIZE)
	}

	if cif.MaxFlowWindowSize == 0 {
		return fmt.Errorf("rendezvous: peer advertised a zero flow window")
	}

	if cif.HasHS {
		if cif.SRTHS == nil {
			return fmt.Errorf("rendezvous: missing handshake extension content")
		}

		hs := cif.SRTHS

		if hs.SRTVersion < rz.config.MinVersion {
			return fmt.Errorf("rendezvous: peer SRT version is not sufficient")
		}

		if !hs.SRTFlags.TSBPDSND || !hs.SRTFlags.TSBPDRCV || !hs.SRTFlags.TLPKTDROP ||
			!hs.SRTFlags.PERIODICNAK || !hs.SRTFlags.REXMITFLG || !hs.SRTFlags.CRYPT {
			return fmt.Errorf("rendezvous: peer doesn't agree on the required SRT flags")
		}

		if hs.SRTFlags.STREAM {
			return fmt.Errorf("rendezvous: peer doesn't support live streaming")
		}
	}

	if rz.negotiated {
		// The baseline is fixed; a peer that moves it is ignored.
		if !cif.InitialPacketSequenceNumber.Equals(rz.peerISN) ||
			cif.MaxTransmissionUnitSize != rz.peerMSS ||
			cif.MaxFlowWindowSize != rz.peerFC {
			return fmt.Errorf("rendezvous: peer changed negotiated parameters mid-attempt")
		}
	} else {
		rz.peerISN = cif.InitialPacketSequenceNumber
		rz.peerMSS = cif.MaxTransmissionUnitSize
		rz.peerFC = cif.MaxFlowWindowSize

		// Effective MSS/FC are the smaller of both announcements. The MSS change
		// is applied to the effective payload size used for fragmenting writes.
		if rz.config.MSS < cif.MaxTransmissionUnitSize {
			rz.effectiveMSS = rz.config.MSS
		} else {
			rz.effectiveMSS = cif.MaxTransmissionUnitSize
		}

		if rz.config.FC < cif.MaxFlowWindowSize {
			rz.effectiveFC = rz.config.FC
		} else {
			rz.effectiveFC = cif.MaxFlowWindowSize
		}

		payloadSize := rz.effectiveMSS - SRT_HEADER_SIZE - UDP_HEADER_SIZE
		if payloadSize < MIN_PAYLOAD_SIZE {
			return fmt.Errorf("rendezvous: effective MSS too small (%d bytes) to fit the minimal payload size (%d bytes)", rz.effectiveMSS, MIN_PAYLOAD_SIZE)
		}

		rz.negotiated = true
	}

	// Latencies are carried by HS extensions, which may arrive after a first
	// WAVEHAND. Recompute deterministically whenever one is present.
	if cif.HasHS {
		rz.peerRecvLatency = cif.SRTHS.RecvTSBPDDelay
		rz.peerSendLatency = cif.SRTHS.SendTSBPDDelay

		// Local receive direction: max(local ReceiverLatency, peer send latency).
		// Local send direction: max(local PeerLatency, peer receive latency).
		rz.effectiveRecvDelay = uint16(rz.config.ReceiverLatency.Milliseconds())
		if rz.peerSendLatency > rz.effectiveRecvDelay {
			rz.effectiveRecvDelay = rz.peerSendLatency
		}

		rz.effectiveSendDelay = uint16(rz.config.PeerLatency.Milliseconds())
		if rz.peerRecvLatency > rz.effectiveSendDelay {
			rz.effectiveSendDelay = rz.peerRecvLatency
		}
	}

	return nil
}

// validateHSDirection checks that an HS extension, when present, points the way
// the role expects: HSREQ is sent by the initiator, HSRSP by the responder.
// A wrong-direction extension cannot advance the state. Must be called with
// rz.lock held.
func (rz *rendezvouser) validateHSDirection(cif *packet.CIFHandshake) bool {
	if !cif.HasHS {
		return true
	}

	if cif.SRTHS == nil {
		return false
	}

	switch rz.role {
	case rzRoleInitiator:
		// The initiator accepts only the responder's HSRSP.
		return !cif.IsRequest
	case rzRoleResponder:
		// The responder accepts only the initiator's HSREQ.
		return cif.IsRequest
	default:
		return false
	}
}

func (rz *rendezvouser) handleWavehand() {
	rz.lock.Lock()
	defer rz.lock.Unlock()

	// Crossed WAVEHANDs: both sides have now seen each other and the role is
	// decided; move to the conclusion phase. A repeated WAVEHAND changes nothing.
	if rz.phase == rzPhaseWavehand {
		rz.phase = rzPhaseConclusion
	}
}

func (rz *rendezvouser) handleConclusion(cif *packet.CIFHandshake, role, phase int) {
	rz.lock.Lock()
	if phase == rzPhaseWavehand {
		// The first message was a CONCLUSION, not a WAVEHAND. The role was
		// decided from the cookie; enter the conclusion phase directly.
		rz.phase = rzPhaseConclusion
	}
	hasHS := cif.HasHS
	rz.lock.Unlock()

	if role == rzRoleInitiator {
		// A plain CONCLUSION (crossed or early) carries no answer: the
		// initiator keeps retransmitting HSREQ until HSRSP arrives.
		if !hasHS {
			return
		}

		// HSRSP: establish and confirm with AGREEMENT.
		rz.lock.Lock()
		rz.phase = rzPhaseAgreement
		rz.lock.Unlock()

		conn, err := rz.establish()
		if err != nil {
			rz.fail(err)
			return
		}

		rz.attachConn(conn)
		rz.sendAgreement()
		return
	}

	// Responder
	if hasHS {
		// HSREQ: answer with CONCLUSION+HSRSP and keep waiting for AGREEMENT.
		rz.sendConclusionWithHSRSP()
	}
	// A plain CONCLUSION changes nothing; the responder's own plain CONCLUSION
	// keeps retransmitting until HSREQ or AGREEMENT arrives.
}

func (rz *rendezvouser) handleAgreement(role int) {
	if role != rzRoleResponder {
		rz.log("handshake:recv:error", func() string { return "initiator received an unexpected AGREEMENT; ignoring" })
		return
	}

	rz.lock.Lock()
	rz.phase = rzPhaseAgreement
	rz.lock.Unlock()

	conn, err := rz.establish()
	if err != nil {
		rz.fail(err)
		return
	}

	rz.attachConn(conn)
}

// handleEarlyPacket deals with data or connection-stage control packets that
// arrive before the Conn has been delivered. Only a responder that has already
// answered HSREQ (i.e. is waiting for AGREEMENT) may use the first legitimate
// packet as confirmation; the packet is then preserved and handed to the
// normal receive path.
func (rz *rendezvouser) handleEarlyPacket(p packet.Packet) {
	rz.lock.Lock()
	role := rz.role
	phase := rz.phase
	negotiated := rz.negotiated
	hsrspSent := rz.hsrspSent
	rz.lock.Unlock()

	// The responder may confirm on a media packet only after it has processed
	// HSREQ (answered with HSRSP), i.e. while waiting for AGREEMENT.
	if role != rzRoleResponder || phase != rzPhaseConclusion || !negotiated || !hsrspSent {
		rz.log("rendezvous", func() string { return "dropping packet received before handshake completion" })
		p.Decommission()
		return
	}

	conn, err := rz.establish()
	if err != nil {
		rz.fail(err)
		p.Decommission()
		return
	}

	rz.attachConn(conn)

	// Hand the confirming packet to the connection as well; the handshake path
	// must not swallow it.
	conn.push(p)
	rz.signalState()
	rz.signalArrival()
}

// sendConclusionWithHSRSP sends the responder's CONCLUSION carrying HSRSP.
func (rz *rendezvouser) sendConclusionWithHSRSP() {
	rz.lock.Lock()
	if !rz.negotiated {
		rz.lock.Unlock()
		return
	}

	cif := rz.buildConclusionCIF(false)
	cif.HasHS = true
	cif.IsRequest = false
	cif.SRTHS = rz.localHSExtension()
	// Reflect the negotiated latencies, matching the caller/listener path.
	cif.SRTHS.RecvTSBPDDelay = rz.effectiveRecvDelay
	cif.SRTHS.SendTSBPDDelay = rz.effectiveSendDelay
	rz.hsrspSent = true
	rz.lock.Unlock()

	p := rz.baseHandshakePacket()
	p.MarshalCIF(cif)
	rz.log("handshake:send:dump", func() string { return p.Dump() })
	rz.send(p)
}

// establish creates the srtConn with independent local and peer ISNs: the
// local sender starts at this side's own ISN while the receiver sequence space
// starts at the peer's ISN.
func (rz *rendezvouser) establish() (*srtConn, error) {
	rz.lock.Lock()

	if !rz.negotiated {
		rz.lock.Unlock()
		return nil, fmt.Errorf("rendezvous: cannot establish before negotiation completed")
	}

	// Copy the config and apply the negotiated MSS/FC and latencies.
	connConfig := rz.config
	connConfig.MSS = rz.effectiveMSS
	connConfig.FC = rz.effectiveFC
	connConfig.PayloadSize = rz.effectiveMSS - SRT_HEADER_SIZE - UDP_HEADER_SIZE
	connConfig.ReceiverLatency = time.Duration(rz.effectiveRecvDelay) * time.Millisecond
	connConfig.PeerLatency = time.Duration(rz.effectiveSendDelay) * time.Millisecond

	localISN := rz.initialPacketSequenceNumber
	peerISN := rz.peerISN
	socketId := rz.socketId
	peerSocketId := rz.peerSocketId
	recvDelay := rz.effectiveRecvDelay
	sendDelay := rz.effectiveSendDelay
	rz.lock.Unlock()

	conn := newSRTConn(srtConnConfig{
		version:                     5,
		isCaller:                    true,
		localAddr:                   rz.localAddr,
		remoteAddr:                  rz.remoteAddr,
		config:                      connConfig,
		start:                       rz.start,
		socketId:                    socketId,
		peerSocketId:                peerSocketId,
		tsbpdTimeBase:               uint64(time.Since(rz.start).Microseconds()),
		tsbpdDelay:                  uint64(recvDelay) * 1000,
		peerTsbpdDelay:              uint64(sendDelay) * 1000,
		initialPacketSequenceNumber: localISN,
		// Independent peer ISN for the receive direction only.
		receiveInitialPacketSequenceNumber:    peerISN,
		hasReceiveInitialPacketSequenceNumber: true,
		keyBaseEncryption:                     packet.UnencryptedPacket,
		onSend:                                rz.send,
		onShutdown:                            func(*srtConn) { rz.Close() },
		logger:                                rz.config.Logger,
	})

	rz.log("connection:new", func() string {
		return fmt.Sprintf("%#08x rendezvous localISN=%d peerISN=%d", conn.SocketId(), localISN.Val(), peerISN.Val())
	})

	return conn, nil
}

// attachConn publishes the established connection exactly once. A second
// confirmation closes its duplicate and keeps the original, so the media time
// base of the live connection is never reset.
func (rz *rendezvouser) attachConn(conn *srtConn) {
	rz.connLock.Lock()
	if rz.conn != nil {
		established := rz.conn
		rz.connLock.Unlock()
		if conn != established {
			conn.close()
		}
		return
	}
	rz.conn = conn
	rz.connLock.Unlock()

	rz.signalState()
	rz.signalArrival()
}

// fail aborts the attempt with the first recorded error.
func (rz *rendezvouser) fail(err error) {
	rz.failOnce.Do(func() {
		rz.failed = true
		select {
		case rz.doneChan <- err:
		default:
		}
		rz.signalState()
	})
}

// send writes a packet to the connected peer socket.
func (rz *rendezvouser) send(p packet.Packet) {
	rz.sndMutex.Lock()
	defer rz.sndMutex.Unlock()

	rz.sndData.Reset()

	if err := p.Marshal(&rz.sndData); err != nil {
		p.Decommission()
		rz.log("packet:send:error", func() string { return "marshalling packet failed" })
		return
	}

	rz.log("packet:send:dump", func() string { return p.Dump() })

	if rz.wireWrite != nil {
		if err := rz.wireWrite(rz.sndData.Bytes()); err != nil {
			rz.log("packet:send:error", func() string { return "writing packet to the wire failed: " + err.Error() })
		}
	} else if _, err := rz.pc.Write(rz.sndData.Bytes()); err != nil {
		rz.log("packet:send:error", func() string { return "writing packet to the wire failed: " + err.Error() })
	}

	if p.Header().IsControlPacket {
		p.Decommission()
	}
}

func (rz *rendezvouser) isShutdown() bool {
	rz.shutdownLock.RLock()
	defer rz.shutdownLock.RUnlock()
	return rz.shutdown
}

// Close closes the rendezvous connection and releases the UDP socket.
func (rz *rendezvouser) Close() error {
	rz.shutdownOnce.Do(func() {
		rz.shutdownLock.Lock()
		rz.shutdown = true
		rz.shutdownLock.Unlock()

		if conn := rz.connNow(); conn != nil {
			conn.close()
		}

		if rz.stopReader != nil {
			rz.stopReader()
		}

		rz.log("rendezvous", func() string { return "closing socket" })
		rz.closeSocket()
	})

	return nil
}

func (rz *rendezvouser) LocalAddr() net.Addr {
	if conn := rz.connNow(); conn == nil {
		return nil
	} else {
		return conn.LocalAddr()
	}
}

func (rz *rendezvouser) RemoteAddr() net.Addr {
	if conn := rz.connNow(); conn == nil {
		return nil
	} else {
		return conn.RemoteAddr()
	}
}

func (rz *rendezvouser) SocketId() uint32 {
	if conn := rz.connNow(); conn == nil {
		return 0
	} else {
		return conn.SocketId()
	}
}

func (rz *rendezvouser) PeerSocketId() uint32 {
	if conn := rz.connNow(); conn == nil {
		return 0
	} else {
		return conn.PeerSocketId()
	}
}

func (rz *rendezvouser) StreamId() string { return "" }

func (rz *rendezvouser) Version() uint32 {
	if conn := rz.connNow(); conn == nil {
		return 0
	} else {
		return conn.Version()
	}
}

func (rz *rendezvouser) Read(p []byte) (int, error) {
	if err := rz.checkConnection(); err != nil {
		return 0, err
	}
	conn := rz.connNow()
	if conn == nil {
		return 0, fmt.Errorf("no connection")
	}
	return conn.Read(p)
}

func (rz *rendezvouser) ReadPacket() (packet.Packet, error) {
	if err := rz.checkConnection(); err != nil {
		return nil, err
	}
	conn := rz.connNow()
	if conn == nil {
		return nil, fmt.Errorf("no connection")
	}
	return conn.ReadPacket()
}

func (rz *rendezvouser) Write(p []byte) (int, error) {
	if err := rz.checkConnection(); err != nil {
		return 0, err
	}
	conn := rz.connNow()
	if conn == nil {
		return 0, fmt.Errorf("no connection")
	}
	return conn.Write(p)
}

func (rz *rendezvouser) WritePacket(p packet.Packet) error {
	if err := rz.checkConnection(); err != nil {
		return err
	}
	conn := rz.connNow()
	if conn == nil {
		return fmt.Errorf("no connection")
	}
	return conn.WritePacket(p)
}

func (rz *rendezvouser) SetDeadline(t time.Time) error {
	return rz.connNow().SetDeadline(t)
}

func (rz *rendezvouser) SetReadDeadline(t time.Time) error {
	return rz.connNow().SetReadDeadline(t)
}

func (rz *rendezvouser) SetWriteDeadline(t time.Time) error {
	return rz.connNow().SetWriteDeadline(t)
}

func (rz *rendezvouser) Stats(s *Statistics) {
	if conn := rz.connNow(); conn != nil {
		conn.Stats(s)
	}
}

func (rz *rendezvouser) checkConnection() error {
	select {
	case err := <-rz.doneChan:
		rz.Close()
		return err
	default:
	}
	return nil
}

func (rz *rendezvouser) log(topic string, message func() string) {
	if rz.config.Logger == nil {
		return
	}
	rz.config.Logger.Print(topic, rz.socketId, 2, message)
}
