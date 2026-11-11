// Copyright 2014 Quoc-Viet Nguyen. All rights reserved.
// This software may be modified and distributed under the terms
// of the BSD license. See the LICENSE file for details.

package modbus

import (
	"bytes"
	"encoding/binary"
	"errors"
	"io"
	"log"
	"net"
	"sync"
	"testing"
	"time"
)

// ExampleClient_ReadFileRecords shows a call with two files and a repeated
// request. The returned outer slice is ordered exactly like the requests.
func ExampleClient_ReadFileRecords() {
	client := TCPClient("127.0.0.1:502")
	records, err := client.ReadFileRecords([]FileRecordRequest{
		{File: 1, Record: 0, Length: 2},
		{File: 2, Record: 100, Length: 3},
		{File: 2, Record: 100, Length: 3}, // duplicates are not merged
	})
	if err != nil {
		log.Fatal(err)
	}
	// records[0] holds 2 registers from file 1, record 0.
	// records[1] and records[2] each hold 3 registers from file 2, record 100.
	_ = records
}

func TestReadFileRecords(t *testing.T) {
	t.Run("TCPTwoFilesAndDuplicate", testReadFileRecordsTCP)
	t.Run("TCPResponseValidation", testReadFileRecordsTCPResponseValidation)
	t.Run("TCPException", testReadFileRecordsTCPException)
	t.Run("TCPLastRecord", testReadFileRecordsTCPLastRecord)
	t.Run("RTUDoubleFileByteByByte", testReadFileRecordsRTU)
	t.Run("RTUException", testReadFileRecordsRTUException)
	t.Run("ASCIITwoFiles", testReadFileRecordsASCII)
	t.Run("Validation", testReadFileRecordsValidation)
}

func testReadFileRecordsTCP(t *testing.T) {
	requests := []FileRecordRequest{
		{File: 1, Record: 0, Length: 2},
		{File: 1, Record: 0, Length: 2}, // identical duplicate, must stay separate
		{File: 2, Record: 100, Length: 3},
	}
	records := [][]uint16{
		{0x1111, 0x2222},
		{0x1111, 0x2222},
		{0x3333, 0x4444, 0x5555},
	}
	client, responder := newTCPFileClient(t, func(pdu []byte) []byte {
		response := fileRecordResponseData(records...)
		return append([]byte{FuncCodeReadFileRecord}, response...)
	})

	results, err := client.ReadFileRecords(requests)
	if err != nil {
		t.Fatal(err)
	}
	if len(results) != len(requests) {
		t.Fatalf("result count: expected %d, actual %d", len(requests), len(results))
	}
	for i := range requests {
		if len(results[i]) != int(requests[i].Length) {
			t.Fatalf("result %d length: expected %d, actual %d", i, requests[i].Length, len(results[i]))
		}
		if !equalUint16(results[i], records[i]) {
			t.Fatalf("result %d: expected %v, actual %v", i, records[i], results[i])
		}
	}
	// Repeated requests must not share a result slice.
	if &results[0][0] == &results[1][0] {
		t.Fatal("duplicate request results share the same backing array")
	}
	results[0][0] = 0
	if results[1][0] != 0x1111 {
		t.Fatal("mutating one result changed a duplicate request result")
	}

	pdus := responder.requestPDUs()
	if len(pdus) != 1 {
		t.Fatalf("request count: expected 1, actual %d", len(pdus))
	}
	if !bytes.Equal(pdus[0], fileRecordRequestPDU(requests)) {
		t.Fatalf("request pdu: expected % x, actual % x", fileRecordRequestPDU(requests), pdus[0])
	}
}

func testReadFileRecordsTCPResponseValidation(t *testing.T) {
	requests := []FileRecordRequest{
		{File: 1, Record: 0, Length: 1},
		{File: 2, Record: 0, Length: 1},
	}
	good := fileRecordResponseData([]uint16{0x1111}, []uint16{0x2222})

	// The second sub-response is deliberately broken in most cases.
	oddData := append([]byte(nil), good...)
	oddData[5] = 2 // length byte of second sub-response: 2 (one odd data byte)

	badReference := append([]byte(nil), good...)
	badReference[6] = 0x07 // reference type of second sub-response

	missingSubResponse := []byte{4, 3, fileRecordReferenceType, 0x11, 0x11}
	mismatchedLength := []byte{10, 3, fileRecordReferenceType, 0x11, 0x11,
		5, fileRecordReferenceType, 0x22, 0x22, 0x22, 0x22}
	shortSubResponse := []byte{7, 3, fileRecordReferenceType, 0x11, 0x11,
		5, fileRecordReferenceType, 0x22}
	outerCountMismatch := append([]byte(nil), good...)
	outerCountMismatch[0]++
	extraSubResponse := append(append([]byte(nil), good...), 3, fileRecordReferenceType, 0x33, 0x33)
	extraSubResponse[0] = byte(len(extraSubResponse) - 1)
	trailingByte := append(append([]byte(nil), good...), 0xAA)
	trailingByte[0] = byte(len(trailingByte) - 1)

	tests := []struct {
		name string
		data []byte
	}{
		{name: "outer count mismatch", data: outerCountMismatch},
		{name: "missing sub-response", data: missingSubResponse},
		{name: "extra sub-response", data: extraSubResponse},
		{name: "trailing byte", data: trailingByte},
		{name: "odd data", data: oddData},
		{name: "invalid reference type", data: badReference},
		{name: "register length mismatch", data: mismatchedLength},
		{name: "sub-response data too short", data: shortSubResponse},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			client, responder := newTCPFileClient(t, func(pdu []byte) []byte {
				return append([]byte{FuncCodeReadFileRecord}, test.data...)
			})
			results, err := client.ReadFileRecords(requests)
			if err == nil {
				t.Fatal("expected an error")
			}
			if results != nil {
				t.Fatalf("partial results exposed: %v", results)
			}
			if got := responder.requestCount(); got != 1 {
				t.Fatalf("request count: expected 1, actual %d", got)
			}
		})
	}
}

func testReadFileRecordsTCPException(t *testing.T) {
	requests := []FileRecordRequest{{File: 1, Record: 0, Length: 1}}
	client, responder := newTCPFileClient(t, func(pdu []byte) []byte {
		return []byte{0x94, ExceptionCodeIllegalDataAddress}
	})
	results, err := client.ReadFileRecords(requests)
	if results != nil {
		t.Fatalf("partial results exposed: %v", results)
	}
	var mbError *ModbusError
	if !errors.As(err, &mbError) {
		t.Fatalf("expected ModbusError, actual %T: %v", err, err)
	}
	if mbError.ExceptionCode != ExceptionCodeIllegalDataAddress {
		t.Fatalf("exception code: expected %d, actual %d", ExceptionCodeIllegalDataAddress, mbError.ExceptionCode)
	}
	if mbError.FunctionCode != 0x94 {
		t.Fatalf("exception function code: expected 0x94, actual 0x%x", mbError.FunctionCode)
	}
	if got := responder.requestCount(); got != 1 {
		t.Fatalf("request count: expected 1, actual %d", got)
	}
}

func testReadFileRecordsTCPLastRecord(t *testing.T) {
	requests := []FileRecordRequest{{File: 65535, Record: 9999, Length: 1}}
	client, responder := newTCPFileClient(t, func(pdu []byte) []byte {
		return append([]byte{FuncCodeReadFileRecord}, fileRecordResponseData([]uint16{0xBEEF})...)
	})
	results, err := client.ReadFileRecords(requests)
	if err != nil {
		t.Fatal(err)
	}
	if len(results) != 1 || len(results[0]) != 1 || results[0][0] != 0xBEEF {
		t.Fatalf("unexpected results: %v", results)
	}
	if got := responder.requestCount(); got != 1 {
		t.Fatalf("request count: expected 1, actual %d", got)
	}
}

func testReadFileRecordsRTU(t *testing.T) {
	requests := []FileRecordRequest{
		{File: 1, Record: 0, Length: 2},
		{File: 1, Record: 0, Length: 2},
		{File: 2, Record: 9998, Length: 2},
	}
	records := [][]uint16{
		{0x1111, 0x2222},
		{0x1111, 0x2222},
		{0x3333, 0x4444},
	}
	responsePDU := append([]byte{FuncCodeReadFileRecord}, fileRecordResponseData(records...)...)
	responseFrame := makeRTUFrame(1, responsePDU)

	port := &fakeSerialPort{response: responseFrame, chunk: 1}
	client := newTestRTUClient(port)
	results, err := client.ReadFileRecords(requests)
	if err != nil {
		t.Fatal(err)
	}
	for i := range requests {
		if len(results[i]) != int(requests[i].Length) || !equalUint16(results[i], records[i]) {
			t.Fatalf("result %d: expected %v, actual %v", i, records[i], results[i])
		}
	}
	written := port.writtenBytes()
	expectedRequest := makeRTUFrame(1, fileRecordRequestPDU(requests))
	if !bytes.Equal(written, expectedRequest) {
		t.Fatalf("outbound frame: expected % x, actual % x", expectedRequest, written)
	}
	// The transporter must read the complete response frame, not only the
	// minimum RTU frame length.
	if got, want := calculateResponseLength(written), len(responseFrame); got != want {
		t.Fatalf("response length estimate: expected %d, actual %d", want, got)
	}
}

func testReadFileRecordsRTUException(t *testing.T) {
	requests := []FileRecordRequest{{File: 1, Record: 0, Length: 1}}
	responseFrame := makeRTUFrame(1, []byte{0x94, ExceptionCodeIllegalDataAddress})
	port := &fakeSerialPort{response: responseFrame}
	client := newTestRTUClient(port)

	results, err := client.ReadFileRecords(requests)
	if results != nil {
		t.Fatalf("partial results exposed: %v", results)
	}
	var mbError *ModbusError
	if !errors.As(err, &mbError) {
		t.Fatalf("expected ModbusError, actual %T: %v", err, err)
	}
	if mbError.ExceptionCode != ExceptionCodeIllegalDataAddress {
		t.Fatalf("exception code: expected %d, actual %d", ExceptionCodeIllegalDataAddress, mbError.ExceptionCode)
	}
	if got := port.writtenBytes(); !bytes.Equal(got, makeRTUFrame(1, fileRecordRequestPDU(requests))) {
		t.Fatalf("outbound frame: expected % x, actual % x", makeRTUFrame(1, fileRecordRequestPDU(requests)), got)
	}
}

func testReadFileRecordsASCII(t *testing.T) {
	requests := []FileRecordRequest{
		{File: 1, Record: 0, Length: 2},
		{File: 2, Record: 5, Length: 1},
	}
	records := [][]uint16{{0x1111, 0x2222}, {0x3333}}

	// The server response is produced by a real ASCII packager; the client
	// then encodes the request and decodes this response through the real
	// packager as well.
	serverPackager := &asciiPackager{SlaveId: 1}
	responseADU, err := serverPackager.Encode(&ProtocolDataUnit{
		FunctionCode: FuncCodeReadFileRecord,
		Data:         fileRecordResponseData(records...),
	})
	if err != nil {
		t.Fatal(err)
	}
	port := &fakeSerialPort{response: responseADU, chunk: 1}
	transporter := &asciiSerialTransporter{serialPort: serialPort{port: port}}
	client := NewClient2(&asciiPackager{SlaveId: 1}, transporter)

	results, err := client.ReadFileRecords(requests)
	if err != nil {
		t.Fatal(err)
	}
	for i := range requests {
		if len(results[i]) != int(requests[i].Length) || !equalUint16(results[i], records[i]) {
			t.Fatalf("result %d: expected %v, actual %v", i, records[i], results[i])
		}
	}

	// Decode the recorded outbound frame through the real packager.
	requestPDU, err := (&asciiPackager{}).Decode(port.writtenBytes())
	if err != nil {
		t.Fatalf("decode outbound ascii frame: %v", err)
	}
	if requestPDU.FunctionCode != FuncCodeReadFileRecord ||
		!bytes.Equal(requestPDU.Data, fileRecordRequestPDU(requests)[1:]) {
		t.Fatalf("outbound pdu: expected fc=%d data=% x, actual fc=%d data=% x",
			FuncCodeReadFileRecord, fileRecordRequestPDU(requests)[1:],
			requestPDU.FunctionCode, requestPDU.Data)
	}
}

func testReadFileRecordsValidation(t *testing.T) {
	tooManyRequests := make([]FileRecordRequest, 36)
	for i := range tooManyRequests {
		tooManyRequests[i] = FileRecordRequest{File: 1, Record: uint16(i), Length: 1}
	}
	tests := []struct {
		name     string
		requests []FileRecordRequest
	}{
		{name: "empty", requests: nil},
		{name: "file zero", requests: []FileRecordRequest{{File: 0, Record: 0, Length: 1}}},
		{name: "record too high", requests: []FileRecordRequest{{File: 1, Record: 10000, Length: 1}}},
		{name: "length zero", requests: []FileRecordRequest{{File: 1, Record: 0, Length: 0}}},
		{name: "length too high", requests: []FileRecordRequest{{File: 1, Record: 0, Length: 126}}},
		{name: "last record overflow", requests: []FileRecordRequest{{File: 1, Record: 9999, Length: 2}}},
		{name: "request pdu too long", requests: tooManyRequests},
		{name: "request fits response too long", requests: []FileRecordRequest{{File: 1, Record: 0, Length: 125}}},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			client, responder := newTCPFileClient(t, func(pdu []byte) []byte {
				// This handler must never be reached: validation happens
				// before any device access.
				t.Errorf("unexpected device request: % x", pdu)
				return nil
			})
			results, err := client.ReadFileRecords(test.requests)
			if err == nil {
				t.Fatal("expected an error")
			}
			if results != nil {
				t.Fatalf("partial results exposed: %v", results)
			}
			if got := responder.requestCount(); got != 0 {
				t.Fatalf("requests sent: expected 0, actual %d", got)
			}
		})
	}
}

// Helpers used by the file record tests.

func equalUint16(a, b []uint16) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

func fileRecordRequestPDU(requests []FileRecordRequest) []byte {
	data := make([]byte, 1+fileRecordSubRequestSize*len(requests))
	data[0] = byte(fileRecordSubRequestSize * len(requests))
	for i, request := range requests {
		offset := 1 + i*fileRecordSubRequestSize
		data[offset] = fileRecordReferenceType
		binary.BigEndian.PutUint16(data[offset+1:], request.File)
		binary.BigEndian.PutUint16(data[offset+3:], request.Record)
		binary.BigEndian.PutUint16(data[offset+5:], request.Length)
	}
	return append([]byte{FuncCodeReadFileRecord}, data...)
}

func fileRecordResponseData(records ...[]uint16) []byte {
	body := make([]byte, 0)
	for _, record := range records {
		sub := make([]byte, 2+2*len(record))
		sub[0] = byte(1 + 2*len(record))
		sub[1] = fileRecordReferenceType
		for i, value := range record {
			binary.BigEndian.PutUint16(sub[2+2*i:], value)
		}
		body = append(body, sub...)
	}
	data := make([]byte, 1+len(body))
	data[0] = byte(len(body))
	copy(data[1:], body)
	return data
}

func makeRTUFrame(slave byte, pdu []byte) []byte {
	frame := make([]byte, 0, len(pdu)+3)
	frame = append(frame, slave)
	frame = append(frame, pdu...)
	var sum crc
	sum.reset().pushBytes(frame)
	value := sum.value()
	frame = append(frame, byte(value), byte(value>>8))
	return frame
}

type fakeSerialPort struct {
	mu       sync.Mutex
	response []byte
	readPos  int
	chunk    int
	written  []byte
}

func (p *fakeSerialPort) Read(buffer []byte) (int, error) {
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.readPos >= len(p.response) {
		return 0, io.EOF
	}
	n := len(p.response) - p.readPos
	if p.chunk > 0 && n > p.chunk {
		n = p.chunk
	}
	if n > len(buffer) {
		n = len(buffer)
	}
	copy(buffer, p.response[p.readPos:p.readPos+n])
	p.readPos += n
	return n, nil
}

func (p *fakeSerialPort) Write(buffer []byte) (int, error) {
	p.mu.Lock()
	defer p.mu.Unlock()
	p.written = append(p.written, buffer...)
	return len(buffer), nil
}

func (p *fakeSerialPort) Close() error {
	return nil
}

func (p *fakeSerialPort) writtenBytes() []byte {
	p.mu.Lock()
	defer p.mu.Unlock()
	out := make([]byte, len(p.written))
	copy(out, p.written)
	return out
}

func newTestRTUClient(port io.ReadWriteCloser) Client {
	return NewClient2(&rtuPackager{SlaveId: 1}, &rtuSerialTransporter{
		serialPort: serialPort{port: port},
	})
}

type tcpTestResponder struct {
	listener net.Listener
	handler  func(pdu []byte) []byte

	mu       sync.Mutex
	requests [][]byte
}

func (r *tcpTestResponder) serve() {
	for {
		conn, err := r.listener.Accept()
		if err != nil {
			return
		}
		go r.handle(conn)
	}
}

func (r *tcpTestResponder) handle(conn net.Conn) {
	defer conn.Close()
	for {
		header := make([]byte, tcpHeaderSize)
		if _, err := io.ReadFull(conn, header); err != nil {
			return
		}
		length := int(binary.BigEndian.Uint16(header[4:6]))
		if length < 1 {
			return
		}
		pdu := make([]byte, length-1)
		if _, err := io.ReadFull(conn, pdu); err != nil {
			return
		}
		r.mu.Lock()
		r.requests = append(r.requests, append([]byte(nil), pdu...))
		r.mu.Unlock()

		response := r.handler(pdu)
		if response == nil {
			return
		}
		out := make([]byte, tcpHeaderSize+len(response))
		copy(out[0:4], header[0:4])
		binary.BigEndian.PutUint16(out[4:6], uint16(1+len(response)))
		out[6] = header[6]
		copy(out[tcpHeaderSize:], response)
		if _, err := conn.Write(out); err != nil {
			return
		}
	}
}

func (r *tcpTestResponder) requestCount() int {
	r.mu.Lock()
	defer r.mu.Unlock()
	return len(r.requests)
}

func (r *tcpTestResponder) requestPDUs() [][]byte {
	r.mu.Lock()
	defer r.mu.Unlock()
	out := make([][]byte, len(r.requests))
	for i := range r.requests {
		out[i] = append([]byte(nil), r.requests[i]...)
	}
	return out
}

func newTCPFileClient(t *testing.T, handler func(pdu []byte) []byte) (Client, *tcpTestResponder) {
	t.Helper()
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err == nil {
		responder := &tcpTestResponder{listener: listener, handler: handler}
		go responder.serve()
		t.Cleanup(func() { listener.Close() })
		handlerConfig := NewTCPClientHandler(listener.Addr().String())
		handlerConfig.SlaveId = 1
		handlerConfig.Timeout = time.Second
		handlerConfig.IdleTimeout = 0
		t.Cleanup(func() { handlerConfig.Close() })
		return NewClient(handlerConfig), responder
	}
	// Some sandboxes forbid binding local ports. Fall back to an in-process
	// connection pair while still driving the real TCP packager and
	// transporter over a net.Conn.
	t.Logf("local TCP listen unavailable (%v), using in-process responder", err)
	clientConn, serverConn := net.Pipe()
	responder := &tcpTestResponder{handler: handler}
	go responder.handle(serverConn)
	transporter := &tcpTransporter{conn: clientConn, Timeout: time.Second}
	t.Cleanup(func() {
		clientConn.Close()
		serverConn.Close()
	})
	return NewClient2(&tcpPackager{SlaveId: 1}, transporter), responder
}
