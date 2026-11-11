[![Go Reference](https://pkg.go.dev/badge/github.com/Comcast/gots/v3.svg)](https://pkg.go.dev/github.com/Comcast/gots/v3)
[![Build Status](https://travis-ci.org/Comcast/gots.svg?branch=master)](https://travis-ci.org/Comcast/gots)
[![Go Report Card](https://goreportcard.com/badge/github.com/Comcast/gots)](https://goreportcard.com/report/github.com/Comcast/gots)
[![Coverage Status](https://coveralls.io/repos/github/Comcast/gots/badge.svg?branch=master)](https://coveralls.io/github/Comcast/gots?branch=master)


# goTS (Go Transport Streams)

gots (Go Transport Streams) is a library for working with MPEG transport streams. It provides abstractions for reading packet information and program specific information (psi)

## Bug / Feature Reporting
Add requests to Github issues. To submit a PR see [CONTRIBUTING](./CONTRIBUTING)
## Tests
```bash
go test -race ./...
```
Travis-CI will run these tests:

```bash
go test -v ./...
```
## License 
This software is licensed under the MIT license. For full text see [LICENSE](./LICENSE)

## Code of Conduct
We take our [code of conduct](CODE_OF_CONDUCT.md) very seriously. Please abide by it.

## Examples
This is a simple example that extracts all PIDs from a ts file and prints them. [CLI example parser can be found here](cli/parsefile.go)
```go
func main() {
	pidSet := make(map[uint16]bool, 5)
	filename := "./scenario1.ts"
	file, err := os.Open(filename)
	if err == nil {
		pkt := make([]byte, packet.PacketSize)
		for read, err := file.Read(pkt); read > 0 && err == nil; read, err = file.Read(pkt) {
                        if err != nil {
                                println(err)
                                return
                        }
			pid, err := packet.Pid(pkt)
			if err != nil {
				println(err)
				continue
			}
			pidSet[pid] = true
		}

        	for v := range pidSet {
	        	fmt.Printf("Found pid %d\n", v)
	        }
	} else {
		fmt.Printf("Unable to open file [%s] due to [%s]\n", filename, err.Error())
}
```

## Retiming a fragment
The `retime` package shifts every media, clock and cue timestamp in a finite,
in-order TS fragment so it can be spliced onto an existing playback timeline.
Call `retime.Shift` with the PIDs that carry PES, PCR and SCTE-35 data and the
delta in 90 kHz units. Packet count, order, continuity counters, payload
boundaries and every non-time byte are preserved, and the input packets are
never modified:
```go
shifted, err := retime.Shift(packets, retime.Config{
    PESPIDs:  []int{videoPID, audioPID}, // rewrite PES PTS/DTS
    PCRPIDs:  []int{pcrPID},             // rewrite adaptation field PCR/OPCR
    CuePIDs:  []int{cuePID},             // rewrite SCTE-35 pts_adjustment + CRC
    Delta90k: 90000,                     // shift all clocks by one second
})
if err != nil {
    // err names the packet index and PID of the malformed selected stream
    return err
}
```

