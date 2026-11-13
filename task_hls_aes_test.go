package gohlslib

import (
	"bytes"
	"crypto/aes"
	"crypto/cipher"
	"encoding/hex"
	"fmt"
	"io"
	"net/http"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/bluenviron/mediacommon/v2/pkg/formats/fmp4"
	"github.com/bluenviron/mediacommon/v2/pkg/formats/fmp4/seekablebuffer"
	mp4codecs "github.com/bluenviron/mediacommon/v2/pkg/formats/mp4/codecs"
	"github.com/bluenviron/mediacommon/v2/pkg/formats/mpegts"
	tscodecs "github.com/bluenviron/mediacommon/v2/pkg/formats/mpegts/codecs"
)

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

type hlsAESTransport func(*http.Request) (*http.Response, error)

func (f hlsAESTransport) RoundTrip(r *http.Request) (*http.Response, error) { return f(r) }

func newHLSResponse(r *http.Request, body []byte) *http.Response {
	return &http.Response{
		StatusCode: http.StatusOK,
		Header:     make(http.Header),
		Body:       io.NopCloser(bytes.NewReader(body)),
		Request:    r,
	}
}

// hlsAESEncryptor encrypts objects independently with AES-128-CBC + PKCS#7.
// It is deliberately implemented with the standard library, independently
// from the client decryption code.
type hlsAESEncryptor struct {
	key   []byte
	iv    []byte
	block cipher.Block
}

func newHLSEncryptor(key []byte, iv string) *hlsAESEncryptor {
	e := &hlsAESEncryptor{key: bytes.Clone(key)}

	var err error
	e.block, err = aes.NewCipher(key)
	if err != nil {
		panic(err)
	}

	if iv != "" {
		e.iv, err = hexDecodeIV(iv)
		if err != nil {
			panic(err)
		}
	}

	return e
}

func hexDecodeIV(iv string) ([]byte, error) {
	if !strings.HasPrefix(iv, "0x") {
		return nil, fmt.Errorf("bad iv")
	}
	s := iv[2:]
	if len(s)%2 != 0 {
		s = "0" + s
	}
	return hex.DecodeString(strings.Repeat("0", 32-len(s)) + s)
}

func (e *hlsAESEncryptor) encryptionIV(mediaSequence int, segIndex int) []byte {
	if e.iv != nil {
		return bytes.Clone(e.iv)
	}
	return segmentIV(mediaSequence, segIndex)
}

func (e *hlsAESEncryptor) encrypt(plaintext []byte) []byte {
	pad := aes.BlockSize - len(plaintext)%aes.BlockSize
	plaintext = append(bytes.Clone(plaintext), bytes.Repeat([]byte{byte(pad)}, pad)...)

	ciphertext := make([]byte, len(plaintext))
	cipher.NewCBCEncrypter(e.block, e.iv).CryptBlocks(ciphertext, plaintext)
	return ciphertext
}

// ---------------------------------------------------------------------------
// media generation
// ---------------------------------------------------------------------------

func hlsAESBuildTSSegment(t *testing.T, samples []hlsAESTSample) []byte {
	t.Helper()

	var buf bytes.Buffer
	track := &mpegts.Track{Codec: &tscodecs.H264{}}
	writer := &mpegts.Writer{W: &buf, Tracks: []*mpegts.Track{track}}
	if err := writer.Initialize(); err != nil {
		t.Fatal(err)
	}

	for _, sample := range samples {
		if err := writer.WriteH264(track, int64(sample.ticks), int64(sample.ticks), sample.au); err != nil {
			t.Fatal(err)
		}
	}

	return buf.Bytes()
}

type hlsAESTSample struct {
	ticks int
	au    [][]byte
}

func hlsAESBuildFMP4Init(t *testing.T) []byte {
	t.Helper()

	var buf seekablebuffer.Buffer
	err := (&fmp4.Init{
		Tracks: []*fmp4.InitTrack{
			{
				ID:        1,
				TimeScale: 90000,
				Codec: &mp4codecs.H264{
					SPS: testSPS,
					PPS: testPPS,
				},
			},
		},
	}).Marshal(&buf)
	if err != nil {
		t.Fatal(err)
	}

	return buf.Bytes()
}

func hlsAESBuildFMP4Part(t *testing.T, baseTime uint64, samples []*fmp4.Sample) []byte {
	t.Helper()

	var buf seekablebuffer.Buffer
	err := (&fmp4.Part{
		Tracks: []*fmp4.PartTrack{
			{
				ID:       1,
				BaseTime: baseTime,
				Samples:  samples,
			},
		},
	}).Marshal(&buf)
	if err != nil {
		t.Fatal(err)
	}

	return buf.Bytes()
}

func hlsAESFMP4Sample(duration uint32, payload [][]byte) *fmp4.Sample {
	return &fmp4.Sample{
		Duration: duration,
		Payload:  mustMarshalAVCC(payload),
	}
}

// ---------------------------------------------------------------------------
// MPEG-TS scenario
// ---------------------------------------------------------------------------

// TestHLSAESTS plays a MPEG-TS live playlist that begins at a non-zero
// MEDIA-SEQUENCE, joins the stream mid-window with an explicit IV, rotates
// between keys A and B, returns to clear playback and then slides the window.
func TestHLSAESTS(t *testing.T) {
	keyA := bytes.Repeat([]byte{0x11}, 16)
	keyB := bytes.Repeat([]byte{0x22}, 16)

	const mediaSequence = 41
	const totalSegments = 6

	// key used by each global segment: A, B, A, B, clear, clear
	keyOf := func(i int) string {
		switch i {
		case 0, 2:
			return "keyA"
		case 1, 3:
			return "keyB"
		}
		return ""
	}

	// each segment holds two access units; timestamps are absolute and
	// continuous across segments (6000 ticks of media per segment).
	plain := make([][]byte, totalSegments)
	ciphertext := make([][]byte, totalSegments)
	for i := 0; i < totalSegments; i++ {
		base := i * 6000
		plain[i] = hlsAESBuildTSSegment(t, []hlsAESTSample{
			{ticks: base, au: [][]byte{{7, 1, 2, 3}, {8}, {5}}},
			{ticks: base + 3000, au: [][]byte{{1, 4, 5, 6}}},
		})

		switch keyOf(i) {
		case "keyA":
			// key A always carries a short explicit IV (left-padded to 16 bytes)
			enc := newHLSEncryptor(keyA, "0xaa")
			ciphertext[i] = enc.encrypt(plain[i])
		case "keyB":
			// key B relies on the implicit IV: MEDIA-SEQUENCE + segment index
			enc := newHLSEncryptor(keyB, "")
			enc.iv = enc.encryptionIV(mediaSequence, i)
			ciphertext[i] = enc.encrypt(plain[i])
		default:
			ciphertext[i] = plain[i]
		}
	}

	// First playlist: sequence 41 with segments 0..4. With the default
	// StartDistance=3 the client joins at index 2 (mid-window).
	playlist0 := "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:1\n" +
		"#EXT-X-MEDIA-SEQUENCE:" + fmt.Sprint(mediaSequence) + "\n" +
		"#EXT-X-KEY:METHOD=AES-128,URI=\"keyA.bin\",IV=0xaa\n" +
		"#EXTINF:1,\nseg0.ts\n" +
		"#EXT-X-KEY:METHOD=AES-128,URI=\"keyB.bin\"\n" +
		"#EXTINF:1,\nseg1.ts\n" +
		"#EXT-X-KEY:METHOD=AES-128,URI=\"keyA.bin\",IV=0xaa\n" +
		"#EXTINF:1,\nseg2.ts\n" +
		"#EXT-X-KEY:METHOD=AES-128,URI=\"keyB.bin\"\n" +
		"#EXTINF:1,\nseg3.ts\n" +
		"#EXT-X-KEY:METHOD=NONE\n" +
		"#EXTINF:1,\nseg4.ts\n"

	// Second playlist: the window has slid to sequence 43 (segments 2..5);
	// the implicit IV of seg3 must use the real media sequence (44), not
	// the download counter.
	playlist1 := "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:1\n" +
		"#EXT-X-MEDIA-SEQUENCE:" + fmt.Sprint(mediaSequence+2) + "\n" +
		"#EXT-X-KEY:METHOD=AES-128,URI=\"keyA.bin\",IV=0xaa\n" +
		"#EXTINF:1,\nseg2.ts\n" +
		"#EXT-X-KEY:METHOD=AES-128,URI=\"keyB.bin\"\n" +
		"#EXTINF:1,\nseg3.ts\n" +
		"#EXT-X-KEY:METHOD=NONE\n" +
		"#EXTINF:1,\nseg4.ts\n" +
		"#EXTINF:1,\nseg5.ts\n" +
		"#EXT-X-ENDLIST\n"

	var (
		mu          sync.Mutex
		playlistIdx int
		requests    []string
		dtsSeen     []int64
		auSeen      [][][]byte
	)

	client := &Client{URI: "http://aes.invalid/live/index.m3u8"}
	client.HTTPClient = &http.Client{Transport: hlsAESTransport(func(r *http.Request) (*http.Response, error) {
		mu.Lock()
		requests = append(requests, r.URL.Path)
		mu.Unlock()

		switch {
		case r.URL.Path == "/live/index.m3u8":
			mu.Lock()
			n := playlistIdx
			playlistIdx++
			mu.Unlock()
			if n == 0 {
				return newHLSResponse(r, []byte(playlist0)), nil
			}
			return newHLSResponse(r, []byte(playlist1)), nil

		case r.URL.Path == "/live/keyA.bin":
			return newHLSResponse(r, keyA), nil

		case r.URL.Path == "/live/keyB.bin":
			return newHLSResponse(r, keyB), nil

		case strings.HasPrefix(r.URL.Path, "/live/seg"):
			var idx int
			if _, err := fmt.Sscanf(r.URL.Path, "/live/seg%d.ts", &idx); err != nil {
				return nil, err
			}
			return newHLSResponse(r, ciphertext[idx]), nil
		}

		return nil, fmt.Errorf("unexpected request %s", r.URL.Path)
	})}

	client.OnTracks = func(tracks []*Track) error {
		if len(tracks) != 1 {
			return fmt.Errorf("tracks=%d", len(tracks))
		}
		client.OnDataH26x(tracks[0], func(pts int64, dts int64, au [][]byte) {
			cloned := make([][]byte, len(au))
			for i := range au {
				cloned[i] = bytes.Clone(au[i])
			}
			mu.Lock()
			dtsSeen = append(dtsSeen, dts)
			auSeen = append(auSeen, cloned)
			mu.Unlock()
		})
		return nil
	}

	if err := client.Start(); err != nil {
		t.Fatal(err)
	}
	defer client.Close()

	select {
	case err := <-client.Wait():
		if err != ErrClientEOS {
			t.Fatalf("unexpected EOS error: %v", err)
		}
	case <-time.After(20 * time.Second):
		t.Fatal("client timed out")
	}

	mu.Lock()
	defer mu.Unlock()

	// played segments: 2..5, two access units each
	if len(auSeen) != 8 {
		t.Fatalf("received %d samples, want 8", len(auSeen))
	}

	// sample bytes must match the plaintext exactly
	for i, au := range auSeen {
		if i%2 == 0 {
			if len(au) != 3 || !bytes.Equal(au[0], []byte{7, 1, 2, 3}) ||
				!bytes.Equal(au[1], []byte{8}) || !bytes.Equal(au[2], []byte{5}) {
				t.Fatalf("sample %d: unexpected AU %v", i, au)
			}
		} else {
			if len(au) != 1 || !bytes.Equal(au[0], []byte{1, 4, 5, 6}) {
				t.Fatalf("sample %d: unexpected AU %v", i, au)
			}
		}
	}

	// timestamps must be continuous with a fixed 3000-tick step
	for i := 1; i < len(dtsSeen); i++ {
		if dtsSeen[i]-dtsSeen[i-1] != 3000 {
			t.Fatalf("non-continuous dts at %d: %v -> %v", i, dtsSeen[i-1], dtsSeen[i])
		}
	}

	// keys must have been fetched through the HTTP client
	var sawKeyA, sawKeyB bool
	for _, p := range requests {
		switch p {
		case "/live/keyA.bin":
			sawKeyA = true
		case "/live/keyB.bin":
			sawKeyB = true
		}
	}
	if !sawKeyA || !sawKeyB {
		t.Fatalf("missing key requests: A=%v B=%v", sawKeyA, sawKeyB)
	}
}

// ---------------------------------------------------------------------------
// fMP4 scenario
// ---------------------------------------------------------------------------

// TestHLSAESFMP4 plays an encrypted fMP4 playlist whose MAP uses key A and
// whose first media segment uses key B, followed by a clear segment.
func TestHLSAESFMP4(t *testing.T) {
	keyA := bytes.Repeat([]byte{0x33}, 16)
	keyB := bytes.Repeat([]byte{0x44}, 16)

	const mediaSequence = 50

	initPlain := hlsAESBuildFMP4Init(t)
	initEnc := newHLSEncryptor(keyA, "0x0000000000000000000000000000000a")
	initCipher := initEnc.encrypt(initPlain)

	seg0Plain := hlsAESBuildFMP4Part(t, 90000, []*fmp4.Sample{
		hlsAESFMP4Sample(3000, [][]byte{{7, 1, 2, 3}, {8}, {5}}),
		hlsAESFMP4Sample(3000, [][]byte{{1, 4, 5, 6}}),
	})
	seg1Plain := hlsAESBuildFMP4Part(t, 96000, []*fmp4.Sample{
		hlsAESFMP4Sample(3000, [][]byte{{5}}),
	})

	seg0Enc := newHLSEncryptor(keyB, "")
	seg0Enc.iv = seg0Enc.encryptionIV(mediaSequence, 0)
	seg0Cipher := seg0Enc.encrypt(seg0Plain)

	pl := "#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-TARGETDURATION:1\n" +
		"#EXT-X-MEDIA-SEQUENCE:" + fmt.Sprint(mediaSequence) + "\n" +
		"#EXT-X-KEY:METHOD=AES-128,URI=\"keyA.bin\",IV=0xa\n" +
		"#EXT-X-MAP:URI=\"init.mp4\"\n" +
		"#EXT-X-KEY:METHOD=AES-128,URI=\"keyB.bin\"\n" +
		"#EXTINF:1,\nseg0.mp4\n" +
		"#EXT-X-KEY:METHOD=NONE\n" +
		"#EXTINF:1,\nseg1.mp4\n" +
		"#EXT-X-ENDLIST\n"

	var (
		mu         sync.Mutex
		requests   []string
		trackCalls int
		auSeen     [][]byte
		dtsSeen    []int64
	)

	client := &Client{URI: "http://aes.invalid/live/index.m3u8"}
	client.HTTPClient = &http.Client{Transport: hlsAESTransport(func(r *http.Request) (*http.Response, error) {
		mu.Lock()
		requests = append(requests, r.URL.Path)
		mu.Unlock()

		switch r.URL.Path {
		case "/live/index.m3u8":
			return newHLSResponse(r, []byte(pl)), nil
		case "/live/init.mp4":
			return newHLSResponse(r, initCipher), nil
		case "/live/seg0.mp4":
			return newHLSResponse(r, seg0Cipher), nil
		case "/live/seg1.mp4":
			return newHLSResponse(r, seg1Plain), nil
		case "/live/keyA.bin":
			return newHLSResponse(r, keyA), nil
		case "/live/keyB.bin":
			return newHLSResponse(r, keyB), nil
		}
		return nil, fmt.Errorf("unexpected request %s", r.URL.Path)
	})}

	client.OnTracks = func(tracks []*Track) error {
		mu.Lock()
		trackCalls++
		mu.Unlock()

		if len(tracks) != 1 {
			return fmt.Errorf("tracks=%d", len(tracks))
		}
		client.OnDataH26x(tracks[0], func(pts int64, dts int64, au [][]byte) {
			cloned := make([][]byte, len(au))
			for i := range au {
				cloned[i] = bytes.Clone(au[i])
			}
			mu.Lock()
			auSeen = append(auSeen, bytes.Join(cloned, []byte{0}))
			dtsSeen = append(dtsSeen, dts)
			mu.Unlock()
		})
		return nil
	}

	if err := client.Start(); err != nil {
		t.Fatal(err)
	}
	defer client.Close()

	select {
	case err := <-client.Wait():
		if err != ErrClientEOS {
			t.Fatalf("EOS: %v", err)
		}
	case <-time.After(20 * time.Second):
		t.Fatal("client timed out")
	}

	mu.Lock()
	defer mu.Unlock()

	if trackCalls != 1 {
		t.Fatalf("tracks discovered %d times, want 1", trackCalls)
	}

	if len(auSeen) != 3 {
		t.Fatalf("received %d samples, want 3", len(auSeen))
	}

	want := [][]byte{
		bytes.Join([][]byte{{7, 1, 2, 3}, {8}, {5}}, []byte{0}),
		{1, 4, 5, 6},
		{5},
	}
	for i := range want {
		if !bytes.Equal(auSeen[i], want[i]) {
			t.Fatalf("sample %d: got %v, want %v", i, auSeen[i], want[i])
		}
	}

	for i := 1; i < len(dtsSeen); i++ {
		if dtsSeen[i]-dtsSeen[i-1] != 3000 {
			t.Fatalf("non-continuous dts at %d: %v -> %v", i, dtsSeen[i-1], dtsSeen[i])
		}
	}

	var sawInit, sawKeyA, sawKeyB bool
	for _, p := range requests {
		switch p {
		case "/live/init.mp4":
			sawInit = true
		case "/live/keyA.bin":
			sawKeyA = true
		case "/live/keyB.bin":
			sawKeyB = true
		}
	}
	if !sawInit || !sawKeyA || !sawKeyB {
		t.Fatalf("missing requests: init=%v keyA=%v keyB=%v", sawInit, sawKeyA, sawKeyB)
	}
}

// ---------------------------------------------------------------------------
// failure scenarios
// ---------------------------------------------------------------------------

// errReader always fails on Read.
type errReader struct{}

func (e *errReader) Read(_ []byte) (int, error) { return 0, fmt.Errorf("read error") }
func (e *errReader) Close() error               { return nil }

// runHLSFailure starts a client against a custom transport, expects a fatal
// error containing expected (or an error matching errorCheck) and checks that
// no sample was delivered.
func runHLSFailure(
	t *testing.T,
	handler func(*http.Request) (*http.Response, error),
	expected string,
	expectKeyRequest bool,
) {
	t.Helper()

	var (
		mu           sync.Mutex
		keyRequested bool
		sawOnRequest bool
		receivedData bool
	)

	client := &Client{
		URI: "http://aes.invalid/live/index.m3u8",
		OnRequest: func(r *http.Request) {
			mu.Lock()
			sawOnRequest = true
			if strings.HasSuffix(r.URL.Path, "key.bin") {
				keyRequested = true
			}
			mu.Unlock()
		},
	}
	client.HTTPClient = &http.Client{Transport: hlsAESTransport(handler)}
	client.OnDownloadSegment = func(_ string) {}

	client.OnTracks = func(tracks []*Track) error {
		client.OnDataH26x(tracks[0], func(_ int64, _ int64, _ [][]byte) {
			mu.Lock()
			receivedData = true
			mu.Unlock()
		})
		return nil
	}

	if err := client.Start(); err != nil {
		t.Fatal(err)
	}
	err := client.Wait2()
	client.Close()

	if err == nil || !strings.Contains(err.Error(), expected) {
		t.Fatalf("expected error containing %q, got %v", expected, err)
	}

	mu.Lock()
	defer mu.Unlock()

	if receivedData {
		t.Fatal("samples were delivered for a failing encrypted object")
	}

	if !sawOnRequest {
		t.Fatal("OnRequest was not called")
	}

	if expectKeyRequest && !keyRequested {
		t.Fatal("key request did not go through OnRequest")
	}
}

const hlsAESPlainPlaylist = "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:1\n" +
	"#EXT-X-MEDIA-SEQUENCE:0\n#EXT-X-KEY:METHOD=AES-128,URI=\"key.bin\"\n" +
	"#EXTINF:1,\nseg0.ts\n#EXT-X-ENDLIST\n"

const hlsAESPlainMapPlaylist = "#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-TARGETDURATION:1\n" +
	"#EXT-X-MEDIA-SEQUENCE:0\n#EXT-X-KEY:METHOD=AES-128,URI=\"key.bin\"\n" +
	"#EXT-X-MAP:URI=\"init.mp4\"\n" +
	"#EXTINF:1,\nseg0.mp4\n#EXT-X-ENDLIST\n"

func TestHLSAESFailures(t *testing.T) {
	key := bytes.Repeat([]byte{0x55}, 16)

	tsMedia := func(t *testing.T) []byte {
		return hlsAESBuildTSSegment(t, []hlsAESTSample{{ticks: 0, au: [][]byte{{5}}}})
	}

	encryptedTS := func(t *testing.T) []byte {
		enc := newHLSEncryptor(key, "")
		enc.iv = enc.encryptionIV(0, 0)
		return enc.encrypt(tsMedia(t))
	}

	t.Run("key length", func(t *testing.T) {
		runHLSFailure(t, func(r *http.Request) (*http.Response, error) {
			switch {
			case strings.HasSuffix(r.URL.Path, ".m3u8"):
				return newHLSResponse(r, []byte(hlsAESPlainPlaylist)), nil
			case strings.HasSuffix(r.URL.Path, "key.bin"):
				return newHLSResponse(r, bytes.Repeat([]byte{0x00}, 15)), nil
			}
			return nil, fmt.Errorf("unexpected request %s", r.URL.Path)
		}, "invalid key length", true)
	})

	t.Run("key status", func(t *testing.T) {
		runHLSFailure(t, func(r *http.Request) (*http.Response, error) {
			switch {
			case strings.HasSuffix(r.URL.Path, ".m3u8"):
				return newHLSResponse(r, []byte(hlsAESPlainPlaylist)), nil
			case strings.HasSuffix(r.URL.Path, "key.bin"):
				return &http.Response{
					StatusCode: http.StatusInternalServerError,
					Header:     make(http.Header),
					Body:       io.NopCloser(bytes.NewReader(nil)),
					Request:    r,
				}, nil
			}
			return nil, fmt.Errorf("unexpected request %s", r.URL.Path)
		}, "bad status code", true)
	})

	t.Run("key read", func(t *testing.T) {
		runHLSFailure(t, func(r *http.Request) (*http.Response, error) {
			switch {
			case strings.HasSuffix(r.URL.Path, ".m3u8"):
				return newHLSResponse(r, []byte(hlsAESPlainPlaylist)), nil
			case strings.HasSuffix(r.URL.Path, "key.bin"):
				return &http.Response{
					StatusCode: http.StatusOK,
					Header:     make(http.Header),
					Body:       &errReader{},
					Request:    r,
				}, nil
			}
			return nil, fmt.Errorf("unexpected request %s", r.URL.Path)
		}, "read error", true)
	})

	t.Run("truncated ciphertext", func(t *testing.T) {
		runHLSFailure(t, func(r *http.Request) (*http.Response, error) {
			switch {
			case strings.HasSuffix(r.URL.Path, ".m3u8"):
				return newHLSResponse(r, []byte(hlsAESPlainPlaylist)), nil
			case strings.HasSuffix(r.URL.Path, "key.bin"):
				return newHLSResponse(r, key), nil
			case strings.HasSuffix(r.URL.Path, ".ts"):
				c := encryptedTS(t)
				return newHLSResponse(r, c[:len(c)-5]), nil
			}
			return nil, fmt.Errorf("unexpected request %s", r.URL.Path)
		}, "not a multiple", true)
	})

	t.Run("empty ciphertext", func(t *testing.T) {
		runHLSFailure(t, func(r *http.Request) (*http.Response, error) {
			switch {
			case strings.HasSuffix(r.URL.Path, ".m3u8"):
				return newHLSResponse(r, []byte(hlsAESPlainPlaylist)), nil
			case strings.HasSuffix(r.URL.Path, "key.bin"):
				return newHLSResponse(r, key), nil
			case strings.HasSuffix(r.URL.Path, ".ts"):
				return newHLSResponse(r, nil), nil
			}
			return nil, fmt.Errorf("unexpected request %s", r.URL.Path)
		}, "empty", true)
	})

	t.Run("padding byte", func(t *testing.T) {
		runHLSFailure(t, func(r *http.Request) (*http.Response, error) {
			switch {
			case strings.HasSuffix(r.URL.Path, ".m3u8"):
				return newHLSResponse(r, []byte(hlsAESPlainPlaylist)), nil
			case strings.HasSuffix(r.URL.Path, "key.bin"):
				return newHLSResponse(r, key), nil
			case strings.HasSuffix(r.URL.Path, ".ts"):
				c := encryptedTS(t)
				c[len(c)-1] ^= 0xff // invalid padding length
				return newHLSResponse(r, c), nil
			}
			return nil, fmt.Errorf("unexpected request %s", r.URL.Path)
		}, "invalid PKCS#7 padding", true)
	})

	t.Run("padding consistency", func(t *testing.T) {
		runHLSFailure(t, func(r *http.Request) (*http.Response, error) {
			switch {
			case strings.HasSuffix(r.URL.Path, ".m3u8"):
				return newHLSResponse(r, []byte(hlsAESPlainPlaylist)), nil
			case strings.HasSuffix(r.URL.Path, "key.bin"):
				return newHLSResponse(r, key), nil
			case strings.HasSuffix(r.URL.Path, ".ts"):
				// encrypt a plaintext with at least two padding bytes, then
				// corrupt a padding byte while keeping the length valid
				enc := newHLSEncryptor(key, "")
				enc.iv = enc.encryptionIV(0, 0)
				plain := tsMedia(t)
				padTo := (len(plain)/aes.BlockSize + 2) * aes.BlockSize
				plain = append(plain, bytes.Repeat([]byte{0x00}, padTo-len(plain))...)
				c := enc.encrypt(plain)
				c[len(c)-2] ^= 0x01
				return newHLSResponse(r, c), nil
			}
			return nil, fmt.Errorf("unexpected request %s", r.URL.Path)
		}, "invalid PKCS#7 padding", true)
	})

	for _, iv := range []string{"0xzz", "0x", "0x" + strings.Repeat("a", 34), "nope"} {
		iv := iv
		t.Run("invalid IV "+iv, func(t *testing.T) {
			runHLSFailure(t, func(r *http.Request) (*http.Response, error) {
				switch {
				case strings.HasSuffix(r.URL.Path, ".m3u8"):
					return newHLSResponse(r, []byte("#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:1\n"+
						"#EXT-X-MEDIA-SEQUENCE:0\n#EXT-X-KEY:METHOD=AES-128,URI=\"key.bin\",IV="+iv+"\n"+
						"#EXTINF:1,\nseg0.ts\n#EXT-X-ENDLIST\n")), nil
				}
				return nil, fmt.Errorf("unexpected request %s", r.URL.Path)
			}, "invalid IV", false)
		})
	}

	t.Run("MAP missing IV", func(t *testing.T) {
		runHLSFailure(t, func(r *http.Request) (*http.Response, error) {
			switch {
			case strings.HasSuffix(r.URL.Path, ".m3u8"):
				return newHLSResponse(r, []byte(hlsAESPlainMapPlaylist)), nil
			}
			return nil, fmt.Errorf("unexpected request %s", r.URL.Path)
		}, "requires an explicit IV", false)
	})

	t.Run("unsupported keyformat", func(t *testing.T) {
		runHLSFailure(t, func(r *http.Request) (*http.Response, error) {
			if strings.HasSuffix(r.URL.Path, ".m3u8") {
				return newHLSResponse(r, []byte("#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:1\n"+
					"#EXT-X-MEDIA-SEQUENCE:0\n"+
					"#EXT-X-KEY:METHOD=AES-128,URI=\"key.bin\",KEYFORMAT=\"com.example.drm\"\n"+
					"#EXTINF:1,\nseg0.ts\n#EXT-X-ENDLIST\n")), nil
			}
			return nil, fmt.Errorf("unexpected request %s", r.URL.Path)
		}, "unsupported KEYFORMAT", false)
	})

	t.Run("unsupported keyformatversions", func(t *testing.T) {
		runHLSFailure(t, func(r *http.Request) (*http.Response, error) {
			if strings.HasSuffix(r.URL.Path, ".m3u8") {
				return newHLSResponse(r, []byte("#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:1\n"+
					"#EXT-X-MEDIA-SEQUENCE:0\n"+
					"#EXT-X-KEY:METHOD=AES-128,URI=\"key.bin\",KEYFORMATVERSIONS=\"2\"\n"+
					"#EXTINF:1,\nseg0.ts\n#EXT-X-ENDLIST\n")), nil
			}
			return nil, fmt.Errorf("unexpected request %s", r.URL.Path)
		}, "unsupported KEYFORMATVERSIONS", false)
	})

	t.Run("unsupported method", func(t *testing.T) {
		runHLSFailure(t, func(r *http.Request) (*http.Response, error) {
			if strings.HasSuffix(r.URL.Path, ".m3u8") {
				return newHLSResponse(r, []byte("#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:1\n"+
					"#EXT-X-MEDIA-SEQUENCE:0\n"+
					"#EXT-X-KEY:METHOD=SAMPLE-AES,URI=\"key.bin\"\n"+
					"#EXTINF:1,\nseg0.ts\n#EXT-X-ENDLIST\n")), nil
			}
			return nil, fmt.Errorf("unexpected request %s", r.URL.Path)
		}, "unsupported encryption method", false)
	})

	t.Run("cancellation", func(t *testing.T) {
		client := &Client{URI: "http://aes.invalid/live/index.m3u8"}
		client.HTTPClient = &http.Client{Transport: hlsAESTransport(func(r *http.Request) (*http.Response, error) {
			switch {
			case strings.HasSuffix(r.URL.Path, ".m3u8"):
				return newHLSResponse(r, []byte(hlsAESPlainPlaylist)), nil
			case strings.HasSuffix(r.URL.Path, "key.bin"):
				return newHLSResponse(r, key), nil
			case strings.HasSuffix(r.URL.Path, ".ts"):
				// block until the request context is cancelled
				<-r.Context().Done()
				return nil, r.Context().Err()
			}
			return nil, fmt.Errorf("unexpected request %s", r.URL.Path)
		})}

		if err := client.Start(); err != nil {
			t.Fatal(err)
		}

		done := make(chan struct{})
		go func() {
			client.Close()
			close(done)
		}()

		select {
		case <-done:
		case <-time.After(10 * time.Second):
			t.Fatal("Close did not return after cancellation")
		}
	})
}
