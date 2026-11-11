/*
MIT License

Copyright 2016 Comcast Cable Communications Management, LLC

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

package retime_test

import (
	"fmt"

	"github.com/Comcast/gots/v3"
	"github.com/Comcast/gots/v3/packet"
	"github.com/Comcast/gots/v3/retime"
)

// ExampleShift moves every media, clock and cue timestamp on the selected PIDs
// forward by one second (90,000 90 kHz ticks) while leaving packet order,
// continuity counters and all other bytes untouched.
func ExampleShift() {
	// A video PES packet carrying a PTS of 1,000 ticks.
	pes := []byte{
		0x00, 0x00, 0x01, 0xE0, // start code prefix and video stream id
		0x00, 0x00, // PES_packet_length: unspecified (legal for video)
		0x80, 0x80, 0x05, // flags and PES header data length
		0x20, 0x00, 0x00, 0x00, 0x00, // PTS placeholder, filled in below
	}
	gots.InsertPTS(pes[9:14], 1000)
	var pkt packet.Packet
	pkt[0] = packet.SyncByte
	pkt[1], pkt[2] = 0x41, 0x00 // PUSI set, PID 256
	pkt[3] = 0x10               // payload only, continuity counter 0
	copy(pkt[4:], pes)

	out, err := retime.Shift([]*packet.Packet{&pkt}, retime.Config{
		PESPIDs:  []int{256},
		PCRPIDs:  []int{256},
		CuePIDs:  nil,
		Delta90k: 90000,
	})
	if err != nil {
		fmt.Println("error:", err)
		return
	}

	pay, _ := packet.Payload(out[0])
	fmt.Println("packets:", len(out))
	fmt.Println("shifted PTS:", gots.ExtractTime(pay[9:14]))
	// Output:
	// packets: 1
	// shifted PTS: 91000
}
