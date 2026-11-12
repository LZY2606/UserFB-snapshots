// Baseline witness only: preserves and checks the upstream reassembly policy.
package main

import (
	"bytes"
	"fmt"
	"github.com/gopacket/gopacket"
	"github.com/gopacket/gopacket/layers"
	"github.com/gopacket/gopacket/reassembly"
	"net"
)

type recorder struct {
	data  []byte
	calls int
}

func (r *recorder) New(_, _ gopacket.Flow, _ *layers.TCP, _ reassembly.AssemblerContext) reassembly.Stream {
	return r
}
func (r *recorder) Accept(_ *layers.TCP, _ gopacket.CaptureInfo, _ reassembly.TCPFlowDirection, _ reassembly.Sequence, _ *bool, _ reassembly.AssemblerContext) bool {
	return true
}
func (r *recorder) ReassembledSG(sg reassembly.ScatterGather, _ reassembly.AssemblerContext) {
	n, _ := sg.Lengths()
	r.data = append(r.data, sg.Fetch(n)...)
	r.calls++
}
func (r *recorder) ReassemblyComplete(_ reassembly.AssemblerContext) bool { return true }
func fixture() (*recorder, *reassembly.Assembler, gopacket.Flow) {
	r := new(recorder)
	f, _ := gopacket.FlowFromEndpoints(layers.NewIPEndpoint(net.IP{10, 0, 0, 1}), layers.NewIPEndpoint(net.IP{10, 0, 0, 2}))
	return r, reassembly.NewAssembler(reassembly.NewStreamPool(r)), f
}
func send(a *reassembly.Assembler, f gopacket.Flow, seq uint32, syn bool, data []byte) {
	t := &layers.TCP{SrcPort: 12345, DstPort: 80, Seq: seq, SYN: syn, BaseLayer: layers.BaseLayer{Payload: data}}
	t.SetInternalPortsForTesting()
	a.Assemble(f, t)
}
func require(ok bool, message string) {
	if !ok {
		panic(message)
	}
}
func main() {
	for _, replacement := range []string{"ABCD", "AXCZ"} {
		r, a, f := fixture()
		send(a, f, 1000, true, nil)
		send(a, f, 1005, false, []byte("ABCD"))
		send(a, f, 1005, false, []byte(replacement))
		require(len(r.data) == 0, "out-of-order delivered too early")
		send(a, f, 1001, false, []byte("gap!"))
		require(string(r.data) == "gap!"+replacement, "queued overlap policy changed")
		a.FlushAll()
		fmt.Printf("QUEUED original=ABCD incoming=%s delivered=%s\n", replacement, r.data)
	}
	r, a, f := fixture()
	start := uint32(0xfffffffc)
	send(a, f, start-1, true, nil)
	send(a, f, start, false, []byte("ABCDEFGH"))
	calls := r.calls
	send(a, f, start+2, false, []byte("cDEf"))
	require(string(r.data) == "ABCDEFGH" && r.calls == calls, "delivered overlap was not discarded")
	a.FlushAll()
	fmt.Printf("DELIVERED wrap-start=%08x retransmit=cDEf output=%s extra-callbacks=%d\n", start, r.data, r.calls-calls)
	r, a, f = fixture()
	send(a, f, 1000, true, nil)
	payload := bytes.Repeat([]byte{'A'}, 3900)
	send(a, f, 1005, false, payload)
	send(a, f, 2904, false, []byte("XYZ"))
	copy(payload[1899:1902], "XYZ")
	send(a, f, 1001, false, []byte("gap!"))
	require(bytes.Equal(r.data, append([]byte("gap!"), payload...)), "page boundary replacement mismatch")
	a.FlushAll()
	fmt.Printf("PAGES bytes=%d replacement-offset=1899 actual=%s\n", len(r.data), r.data[1903:1906])
	fmt.Println("BASELINE_OK identical-retransmit queued-overwrite delivered-ignore wrap multipage")
}
