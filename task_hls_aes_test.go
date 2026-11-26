package gohlslib

import (
	"bytes"
	"crypto/aes"
	"crypto/cipher"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"net/http"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"github.com/bluenviron/mediacommon/v2/pkg/formats/fmp4"
	mp4codecs "github.com/bluenviron/mediacommon/v2/pkg/formats/mp4/codecs"
	"github.com/bluenviron/mediacommon/v2/pkg/formats/mpegts"
	tscodecs "github.com/bluenviron/mediacommon/v2/pkg/formats/mpegts/codecs"
	"github.com/stretchr/testify/require"
)

type aesTaskSample struct {
	pts int64
	dts int64
	au  [][]byte
}

type aesTaskHarness struct {
	client      *Client
	samples     chan aesTaskSample
	trackCalls  atomic.Int32
	sampleCalls atomic.Int64
	keyRequests atomic.Int32
}

func aesTaskResponse(status int, body []byte) *http.Response {
	return &http.Response{
		StatusCode: status,
		Header:     make(http.Header),
		Body:       io.NopCloser(bytes.NewReader(body)),
	}
}

func aesTaskEncrypt(plaintext, key, iv []byte) []byte {
	padding := aes.BlockSize - len(plaintext)%aes.BlockSize
	padded := append(bytes.Clone(plaintext), bytes.Repeat([]byte{byte(padding)}, padding)...)
	ret := make([]byte, len(padded))
	cipher.NewCBCEncrypter(aesTaskMustBlock(key), iv).CryptBlocks(ret, padded)
	return ret
}

func aesTaskEncryptRaw(plaintext, key, iv []byte) []byte {
	if len(plaintext)%aes.BlockSize != 0 {
		panic("raw plaintext must be block aligned")
	}
	ret := make([]byte, len(plaintext))
	cipher.NewCBCEncrypter(aesTaskMustBlock(key), iv).CryptBlocks(ret, plaintext)
	return ret
}

func aesTaskMustBlock(key []byte) cipher.Block {
	block, err := aes.NewCipher(key)
	if err != nil {
		panic(err)
	}
	return block
}

func aesTaskSequenceIV(sequence int) []byte {
	iv := make([]byte, aes.BlockSize)
	binary.BigEndian.PutUint64(iv[aes.BlockSize-8:], uint64(sequence))
	return iv
}

func aesTaskExplicitIV(t *testing.T, value string) []byte {
	t.Helper()
	require.True(t, strings.HasPrefix(value, "0x"))
	digits := value[2:]
	padded := strings.Repeat("0", 32-len(digits)) + digits
	iv, err := hex.DecodeString(padded)
	require.NoError(t, err)
	return iv
}

func aesTaskTSSegment(t *testing.T, marker byte, pts int64) []byte {
	t.Helper()

	track := &mpegts.Track{Codec: &tscodecs.H264{}}
	var buf bytes.Buffer
	w := &mpegts.Writer{W: &buf, Tracks: []*mpegts.Track{track}}
	require.NoError(t, w.Initialize())
	require.NoError(t, w.WriteH264(track, pts, pts, [][]byte{
		{0x67, 1, 2, 3},
		{0x68},
		{0x05, marker},
	}))
	return bytes.Clone(buf.Bytes())
}

func aesTaskNewHarness(
	t *testing.T,
	uri string,
	startDistance int,
	handle func(r *http.Request) (*http.Response, error),
) *aesTaskHarness {
	t.Helper()

	h := &aesTaskHarness{
		samples: make(chan aesTaskSample, 64),
	}

	h.client = &Client{
		URI: uri,
		HTTPClient: &http.Client{Transport: taskBaselineTransport(func(r *http.Request) (*http.Response, error) {
			return handle(r)
		})},
		OnRequest: func(req *http.Request) {
			if strings.Contains(req.URL.Path, "key") {
				h.keyRequests.Add(1)
				req.Header.Set("Authorization", "Bearer aes-task")
			}
		},
		OnDownloadPrimaryPlaylist: func(string) {},
		OnDownloadStreamPlaylist:  func(string) {},
		OnDownloadSegment:         func(string) {},
		OnDownloadPart:            func(string) {},
		OnDecodeError:             func(error) {},
	}

	h.client.OnTracks = func(tracks []*Track) error {
		h.trackCalls.Add(1)
		if len(tracks) != 1 {
			return fmt.Errorf("expected one track, got %d", len(tracks))
		}
		h.client.OnDataH26x(tracks[0], func(pts, dts int64, au [][]byte) {
			h.sampleCalls.Add(1)
			copied := make([][]byte, len(au))
			for i := range au {
				copied[i] = bytes.Clone(au[i])
			}
			select {
			case h.samples <- aesTaskSample{pts: pts, dts: dts, au: copied}:
			default:
				t.Errorf("sample channel full")
			}
		})
		return nil
	}

	if startDistance != 0 {
		h.client.StartDistance = startDistance
	}

	require.NoError(t, h.client.Start())
	t.Cleanup(h.client.Close)
	return h
}

func (h *aesTaskHarness) waitEOS(t *testing.T) {
	t.Helper()
	select {
	case err := <-h.client.Wait():
		require.ErrorIs(t, err, ErrClientEOS)
	case <-time.After(15 * time.Second):
		t.Fatal("timed out waiting for EOS")
	}
}

func (h *aesTaskHarness) waitError(t *testing.T) error {
	t.Helper()
	select {
	case err := <-h.client.Wait():
		require.Error(t, err)
		return err
	case <-time.After(15 * time.Second):
		t.Fatal("timed out waiting for client error")
		return nil
	}
}

func aesTaskRecordAuth(r *http.Request, observed *atomic.Bool) {
	if strings.Contains(r.URL.Path, "key") &&
		r.Header.Get("Authorization") == "Bearer aes-task" {
		observed.Store(true)
	}
}

func aesTaskIDRMarker(s aesTaskSample) (byte, bool) {
	for _, nalu := range s.au {
		if len(nalu) >= 2 && nalu[0] == 0x05 {
			return nalu[1], true
		}
	}
	return 0, false
}

func TestHLSAESTS(t *testing.T) {
	t.Run("vod explicit-iv-ab-rotation-and-none", func(t *testing.T) {
		keyA := []byte("0123456789abcdef")
		keyB := []byte("fedcba9876543210")

		plain50 := aesTaskTSSegment(t, 0xa0, 90_000)
		plain51 := aesTaskTSSegment(t, 0xa1, 93_000)
		plain52 := aesTaskTSSegment(t, 0xa2, 96_000)

		seg50 := aesTaskEncrypt(plain50, keyA, aesTaskExplicitIV(t, "0x10"))
		seg51 := aesTaskEncrypt(plain51, keyB, aesTaskSequenceIV(51))

		playlistText := []byte("#EXTM3U\n" +
			"#EXT-X-VERSION:3\n" +
			"#EXT-X-TARGETDURATION:1\n" +
			"#EXT-X-MEDIA-SEQUENCE:50\n" +
			"#EXT-X-KEY:METHOD=AES-128,URI=\"a.key\",IV=0x10\n" +
			"#EXTINF:1,\nseg0.ts\n" +
			"#EXT-X-KEY:METHOD=AES-128,URI=\"b.key\"\n" +
			"#EXTINF:1,\nseg1.ts\n" +
			"#EXT-X-KEY:METHOD=NONE\n" +
			"#EXTINF:1,\nseg2.ts\n" +
			"#EXT-X-ENDLIST\n")

		var authSeen atomic.Bool
		var h *aesTaskHarness
		h = aesTaskNewHarness(t, "http://aes.invalid/vod/index.m3u8", 0, func(r *http.Request) (*http.Response, error) {
			aesTaskRecordAuth(r, &authSeen)
			switch r.URL.Path {
			case "/vod/index.m3u8":
				return aesTaskResponse(200, playlistText), nil
			case "/vod/a.key":
				return aesTaskResponse(200, keyA), nil
			case "/vod/b.key":
				return aesTaskResponse(200, keyB), nil
			case "/vod/seg0.ts":
				return aesTaskResponse(200, seg50), nil
			case "/vod/seg1.ts":
				return aesTaskResponse(200, seg51), nil
			case "/vod/seg2.ts":
				return aesTaskResponse(200, plain52), nil
			default:
				return aesTaskResponse(404, nil), nil
			}
		})

		h.waitEOS(t)

		require.Equal(t, int32(1), h.trackCalls.Load())
		require.GreaterOrEqual(t, h.keyRequests.Load(), int32(2))
		require.True(t, authSeen.Load())

		var samples []aesTaskSample
		for len(samples) < 3 {
			select {
			case sample := <-h.samples:
				samples = append(samples, sample)
			case <-time.After(5 * time.Second):
				t.Fatalf("only %d samples", len(samples))
			}
		}

		wantMarkers := []byte{0xa0, 0xa1, 0xa2}
		wantPTS := []int64{0, 3000, 6000}
		for i, sample := range samples {
			marker, ok := aesTaskIDRMarker(sample)
			require.True(t, ok, "sample %d has no IDR", i)
			require.Equal(t, wantMarkers[i], marker)
			require.Equal(t, wantPTS[i], sample.pts)
			require.Equal(t, wantPTS[i], sample.dts)
		}
	})

	t.Run("live-join-middle-and-slide", func(t *testing.T) {
		key := []byte("live-aes-key-16!")
		plain102 := aesTaskTSSegment(t, 0x02, 90_000)
		plain103 := aesTaskTSSegment(t, 0x03, 93_000)
		plain104 := aesTaskTSSegment(t, 0x04, 96_000)
		seg102 := aesTaskEncrypt(plain102, key, aesTaskSequenceIV(102))
		seg103 := aesTaskEncrypt(plain103, key, aesTaskSequenceIV(103))
		seg104 := aesTaskEncrypt(plain104, key, aesTaskSequenceIV(104))

		initialPlaylist := []byte("#EXTM3U\n" +
			"#EXT-X-VERSION:3\n" +
			"#EXT-X-TARGETDURATION:1\n" +
			"#EXT-X-MEDIA-SEQUENCE:100\n" +
			"#EXT-X-KEY:METHOD=AES-128,URI=\"live.key\"\n" +
			"#EXTINF:1,\nseg100.ts\n" +
			"#EXTINF:1,\nseg101.ts\n" +
			"#EXTINF:1,\nseg102.ts\n" +
			"#EXTINF:1,\nseg103.ts\n")
		slidPlaylist := "#EXTM3U\n" +
			"#EXT-X-VERSION:3\n" +
			"#EXT-X-TARGETDURATION:1\n" +
			"#EXT-X-MEDIA-SEQUENCE:101\n" +
			"#EXT-X-KEY:METHOD=AES-128,URI=\"live.key\"\n" +
			"#EXTINF:1,\nseg101.ts\n" +
			"#EXTINF:1,\nseg102.ts\n" +
			"#EXTINF:1,\nseg103.ts\n" +
			"#EXTINF:1,\nseg104.ts\n"
		slidEndlistPlaylist := []byte(slidPlaylist + "#EXT-X-ENDLIST\n")

		var playlistRequests atomic.Int32
		var authSeen atomic.Bool
		var h *aesTaskHarness
		h = aesTaskNewHarness(t, "http://aes.invalid/live/index.m3u8", 2, func(r *http.Request) (*http.Response, error) {
			aesTaskRecordAuth(r, &authSeen)
			switch r.URL.Path {
			case "/live/index.m3u8":
				n := playlistRequests.Add(1)
				if n == 1 {
					return aesTaskResponse(200, initialPlaylist), nil
				}
				if n == 2 {
					return aesTaskResponse(200, []byte(slidPlaylist)), nil
				}
				return aesTaskResponse(200, slidEndlistPlaylist), nil
			case "/live/live.key":
				return aesTaskResponse(200, key), nil
			case "/live/seg102.ts":
				return aesTaskResponse(200, seg102), nil
			case "/live/seg103.ts":
				return aesTaskResponse(200, seg103), nil
			case "/live/seg104.ts":
				return aesTaskResponse(200, seg104), nil
			default:
				return aesTaskResponse(404, nil), nil
			}
		})

		h.waitEOS(t)
		require.Equal(t, int32(1), h.trackCalls.Load())
		require.True(t, authSeen.Load())

		var samples []aesTaskSample
		for len(samples) < 3 {
			select {
			case sample := <-h.samples:
				samples = append(samples, sample)
			case <-time.After(5 * time.Second):
				t.Fatalf("only %d live samples", len(samples))
			}
		}
		require.Equal(t, []byte{0x02, 0x03, 0x04}, []byte{
			aesTaskMustMarker(t, samples[0]),
			aesTaskMustMarker(t, samples[1]),
			aesTaskMustMarker(t, samples[2]),
		})
		require.Equal(t, []int64{0, 3000, 6000}, []int64{samples[0].pts, samples[1].pts, samples[2].pts})
	})
}

func aesTaskMustMarker(t *testing.T, s aesTaskSample) byte {
	t.Helper()
	m, ok := aesTaskIDRMarker(s)
	require.True(t, ok)
	return m
}

func aesTaskFMP4Init(t *testing.T) []byte {
	t.Helper()
	var buf bytes.Buffer
	require.NoError(t, mp4ToWriter(&fmp4.Init{
		Tracks: []*fmp4.InitTrack{{
			ID:        1,
			TimeScale: 90_000,
			Codec: &mp4codecs.H264{
				SPS: testSPS,
				PPS: testPPS,
			},
		}},
	}, &buf))
	return bytes.Clone(buf.Bytes())
}

func aesTaskFMP4Segment(t *testing.T, marker byte, baseTime uint64) []byte {
	t.Helper()
	var buf bytes.Buffer
	require.NoError(t, mp4ToWriter(&fmp4.Part{
		Tracks: []*fmp4.PartTrack{{
			ID:       1,
			BaseTime: baseTime,
			Samples: []*fmp4.Sample{{
				Duration:  3000,
				PTSOffset: 0,
				Payload: mustMarshalAVCC([][]byte{
					{0x67, 1, 2, 3},
					{0x68},
					{0x05, marker},
				}),
			}},
		}},
	}, &buf))
	return bytes.Clone(buf.Bytes())
}

func TestHLSAESFMP4(t *testing.T) {
	t.Run("encrypted-map-a-media-b-ab-rotation-none", func(t *testing.T) {
		keyA := []byte("0123456789abcdef")
		keyB := []byte("fedcba9876543210")
		mapIV := aesTaskExplicitIV(t, "0x0102030405060708090a0b0c0d0e0f10")

		initPlain := aesTaskFMP4Init(t)
		initEnc := aesTaskEncrypt(initPlain, keyA, mapIV)

		plain70 := aesTaskFMP4Segment(t, 0xb0, 90_000)
		plain71 := aesTaskFMP4Segment(t, 0xb1, 93_000)
		plain72 := aesTaskFMP4Segment(t, 0xb2, 96_000)
		seg70 := aesTaskEncrypt(plain70, keyB, aesTaskExplicitIV(t, "0x20"))
		seg71 := aesTaskEncrypt(plain71, keyA, aesTaskSequenceIV(71))

		playlistText := []byte("#EXTM3U\n" +
			"#EXT-X-VERSION:7\n" +
			"#EXT-X-TARGETDURATION:1\n" +
			"#EXT-X-MEDIA-SEQUENCE:70\n" +
			"#EXT-X-KEY:METHOD=AES-128,URI=\"a.key\",IV=0x0102030405060708090a0b0c0d0e0f10\n" +
			"#EXT-X-MAP:URI=\"init.mp4\"\n" +
			"#EXT-X-KEY:METHOD=AES-128,URI=\"b.key\",IV=0x20\n" +
			"#EXTINF:1,\nseg0.m4s\n" +
			"#EXT-X-KEY:METHOD=AES-128,URI=\"a.key\"\n" +
			"#EXTINF:1,\nseg1.m4s\n" +
			"#EXT-X-KEY:METHOD=NONE\n" +
			"#EXTINF:1,\nseg2.m4s\n" +
			"#EXT-X-ENDLIST\n")

		var authSeen atomic.Bool
		var h *aesTaskHarness
		h = aesTaskNewHarness(t, "http://aes.invalid/fmp4/index.m3u8", 0, func(r *http.Request) (*http.Response, error) {
			aesTaskRecordAuth(r, &authSeen)
			switch r.URL.Path {
			case "/fmp4/index.m3u8":
				return aesTaskResponse(200, playlistText), nil
			case "/fmp4/a.key":
				return aesTaskResponse(200, keyA), nil
			case "/fmp4/b.key":
				return aesTaskResponse(200, keyB), nil
			case "/fmp4/init.mp4":
				return aesTaskResponse(200, initEnc), nil
			case "/fmp4/seg0.m4s":
				return aesTaskResponse(200, seg70), nil
			case "/fmp4/seg1.m4s":
				return aesTaskResponse(200, seg71), nil
			case "/fmp4/seg2.m4s":
				return aesTaskResponse(200, plain72), nil
			default:
				return aesTaskResponse(404, nil), nil
			}
		})

		h.waitEOS(t)
		require.Equal(t, int32(1), h.trackCalls.Load())
		require.GreaterOrEqual(t, h.keyRequests.Load(), int32(3))
		require.True(t, authSeen.Load())

		var samples []aesTaskSample
		for len(samples) < 3 {
			select {
			case sample := <-h.samples:
				samples = append(samples, sample)
			case <-time.After(5 * time.Second):
				t.Fatalf("only %d fMP4 samples", len(samples))
			}
		}
		require.Equal(t, []byte{0xb0, 0xb1, 0xb2}, []byte{
			aesTaskMustMarker(t, samples[0]),
			aesTaskMustMarker(t, samples[1]),
			aesTaskMustMarker(t, samples[2]),
		})
		require.Equal(t, []int64{0, 3000, 6000}, []int64{samples[0].pts, samples[1].pts, samples[2].pts})
	})

	t.Run("live-slide-default-sequence-iv", func(t *testing.T) {
		key := []byte("live-fmp4-key-16")
		mapIV := aesTaskExplicitIV(t, "0x0102030405060708")
		initEnc := aesTaskEncrypt(aesTaskFMP4Init(t), key, mapIV)

		plain82 := aesTaskFMP4Segment(t, 0xc2, 90_000)
		plain83 := aesTaskFMP4Segment(t, 0xc3, 93_000)
		plain84 := aesTaskFMP4Segment(t, 0xc4, 96_000)
		seg82 := aesTaskEncrypt(plain82, key, aesTaskSequenceIV(82))
		seg83 := aesTaskEncrypt(plain83, key, aesTaskSequenceIV(83))
		seg84 := aesTaskEncrypt(plain84, key, aesTaskSequenceIV(84))

		initial := []byte("#EXTM3U\n" +
			"#EXT-X-VERSION:7\n" +
			"#EXT-X-TARGETDURATION:1\n" +
			"#EXT-X-MEDIA-SEQUENCE:80\n" +
			"#EXT-X-KEY:METHOD=AES-128,URI=\"live.key\",IV=0x0102030405060708\n" +
			"#EXT-X-MAP:URI=\"init.mp4\"\n" +
			"#EXT-X-KEY:METHOD=AES-128,URI=\"live.key\"\n" +
			"#EXTINF:1,\nseg80.m4s\n" +
			"#EXTINF:1,\nseg81.m4s\n" +
			"#EXTINF:1,\nseg82.m4s\n" +
			"#EXTINF:1,\nseg83.m4s\n")
		slid := "#EXTM3U\n" +
			"#EXT-X-VERSION:7\n" +
			"#EXT-X-TARGETDURATION:1\n" +
			"#EXT-X-MEDIA-SEQUENCE:81\n" +
			"#EXT-X-KEY:METHOD=AES-128,URI=\"live.key\",IV=0x0102030405060708\n" +
			"#EXT-X-MAP:URI=\"init.mp4\"\n" +
			"#EXT-X-KEY:METHOD=AES-128,URI=\"live.key\"\n" +
			"#EXTINF:1,\nseg81.m4s\n" +
			"#EXTINF:1,\nseg82.m4s\n" +
			"#EXTINF:1,\nseg83.m4s\n" +
			"#EXTINF:1,\nseg84.m4s\n"
		endlist := []byte(slid + "#EXT-X-ENDLIST\n")

		var playlistRequests atomic.Int32
		var authSeen atomic.Bool
		var h *aesTaskHarness
		h = aesTaskNewHarness(t, "http://aes.invalid/fmp4live/index.m3u8", 2, func(r *http.Request) (*http.Response, error) {
			aesTaskRecordAuth(r, &authSeen)
			switch r.URL.Path {
			case "/fmp4live/index.m3u8":
				n := playlistRequests.Add(1)
				if n == 1 {
					return aesTaskResponse(200, initial), nil
				}
				if n == 2 {
					return aesTaskResponse(200, []byte(slid)), nil
				}
				return aesTaskResponse(200, endlist), nil
			case "/fmp4live/live.key":
				return aesTaskResponse(200, key), nil
			case "/fmp4live/init.mp4":
				return aesTaskResponse(200, initEnc), nil
			case "/fmp4live/seg82.m4s":
				return aesTaskResponse(200, seg82), nil
			case "/fmp4live/seg83.m4s":
				return aesTaskResponse(200, seg83), nil
			case "/fmp4live/seg84.m4s":
				return aesTaskResponse(200, seg84), nil
			default:
				return aesTaskResponse(404, nil), nil
			}
		})

		h.waitEOS(t)
		require.Equal(t, int32(1), h.trackCalls.Load())
		require.True(t, authSeen.Load())
		var samples []aesTaskSample
		for len(samples) < 3 {
			select {
			case sample := <-h.samples:
				samples = append(samples, sample)
			case <-time.After(5 * time.Second):
				t.Fatalf("only %d live fMP4 samples", len(samples))
			}
		}
		require.Equal(t, []byte{0xc2, 0xc3, 0xc4}, []byte{
			aesTaskMustMarker(t, samples[0]),
			aesTaskMustMarker(t, samples[1]),
			aesTaskMustMarker(t, samples[2]),
		})
		require.Equal(t, []int64{0, 3000, 6000}, []int64{samples[0].pts, samples[1].pts, samples[2].pts})
	})
}

type aesTaskErrorBody struct{}

func (aesTaskErrorBody) Read([]byte) (int, error) {
	return 0, errors.New("simulated key body read failure")
}
func (aesTaskErrorBody) Close() error { return nil }

func aesTaskFailurePlaylist(keyLine string) []byte {
	return []byte("#EXTM3U\n" +
		"#EXT-X-VERSION:3\n" +
		"#EXT-X-TARGETDURATION:1\n" +
		"#EXT-X-MEDIA-SEQUENCE:10\n" +
		keyLine + "\n" +
		"#EXTINF:1,\n" +
		"seg.ts\n" +
		"#EXT-X-ENDLIST\n")
}

func TestHLSAESFailures(t *testing.T) {
	validKey := []byte("0123456789abcdef")
	validTS := aesTaskTSSegment(t, 0xd0, 90_000)
	validCiphertext := aesTaskEncrypt(validTS, validKey, aesTaskSequenceIV(10))

	paddingZero := aesTaskEncryptRaw(make([]byte, 16), validKey, aesTaskSequenceIV(10))
	paddingTooLong := aesTaskEncryptRaw(bytes.Repeat([]byte{17}, 16), validKey, aesTaskSequenceIV(10))
	paddingMismatchPlain := append(bytes.Repeat([]byte{'A'}, 12), 1, 2, 3, 4)
	paddingMismatch := aesTaskEncryptRaw(paddingMismatchPlain, validKey, aesTaskSequenceIV(10))

	cases := []struct {
		name             string
		playlist         []byte
		keyStatus        int
		keyBody          []byte
		keyBodyReadError bool
		segment          []byte
		wantKeyRequest   bool
	}{
		{
			name:           "key-too-short",
			playlist:       aesTaskFailurePlaylist(`#EXT-X-KEY:METHOD=AES-128,URI="key.bin"`),
			keyStatus:      200,
			keyBody:        []byte("short"),
			segment:        validCiphertext,
			wantKeyRequest: true,
		},
		{
			name:           "key-bad-status",
			playlist:       aesTaskFailurePlaylist(`#EXT-X-KEY:METHOD=AES-128,URI="key.bin"`),
			keyStatus:      500,
			segment:        validCiphertext,
			wantKeyRequest: true,
		},
		{
			name:             "key-read-error",
			playlist:         aesTaskFailurePlaylist(`#EXT-X-KEY:METHOD=AES-128,URI="key.bin"`),
			keyStatus:        200,
			keyBodyReadError: true,
			segment:          validCiphertext,
			wantKeyRequest:   true,
		},
		{
			name:           "ciphertext-empty",
			playlist:       aesTaskFailurePlaylist(`#EXT-X-KEY:METHOD=AES-128,URI="key.bin"`),
			keyStatus:      200,
			keyBody:        validKey,
			segment:        nil,
			wantKeyRequest: true,
		},
		{
			name:           "ciphertext-truncated",
			playlist:       aesTaskFailurePlaylist(`#EXT-X-KEY:METHOD=AES-128,URI="key.bin"`),
			keyStatus:      200,
			keyBody:        validKey,
			segment:        validCiphertext[:len(validCiphertext)-1],
			wantKeyRequest: true,
		},
		{
			name:           "padding-zero",
			playlist:       aesTaskFailurePlaylist(`#EXT-X-KEY:METHOD=AES-128,URI="key.bin"`),
			keyStatus:      200,
			keyBody:        validKey,
			segment:        paddingZero,
			wantKeyRequest: true,
		},
		{
			name:           "padding-length-too-long",
			playlist:       aesTaskFailurePlaylist(`#EXT-X-KEY:METHOD=AES-128,URI="key.bin"`),
			keyStatus:      200,
			keyBody:        validKey,
			segment:        paddingTooLong,
			wantKeyRequest: true,
		},
		{
			name:           "padding-bytes-mismatch",
			playlist:       aesTaskFailurePlaylist(`#EXT-X-KEY:METHOD=AES-128,URI="key.bin"`),
			keyStatus:      200,
			keyBody:        validKey,
			segment:        paddingMismatch,
			wantKeyRequest: true,
		},
		{
			name:           "iv-illegal-character",
			playlist:       aesTaskFailurePlaylist(`#EXT-X-KEY:METHOD=AES-128,URI="key.bin",IV=0xZZ`),
			segment:        validCiphertext,
			wantKeyRequest: false,
		},
		{
			name:           "iv-empty-value",
			playlist:       aesTaskFailurePlaylist(`#EXT-X-KEY:METHOD=AES-128,URI="key.bin",IV=0x`),
			segment:        validCiphertext,
			wantKeyRequest: false,
		},
		{
			name:           "iv-missing-prefix",
			playlist:       aesTaskFailurePlaylist(`#EXT-X-KEY:METHOD=AES-128,URI="key.bin",IV=0001`),
			segment:        validCiphertext,
			wantKeyRequest: false,
		},
		{
			name: "iv-too-long",
			playlist: aesTaskFailurePlaylist(`#EXT-X-KEY:METHOD=AES-128,URI="key.bin",IV=0x` +
				strings.Repeat("a", 33)),
			segment:        validCiphertext,
			wantKeyRequest: false,
		},
		{
			name: "map-missing-explicit-iv",
			playlist: []byte("#EXTM3U\n" +
				"#EXT-X-VERSION:7\n" +
				"#EXT-X-TARGETDURATION:1\n" +
				"#EXT-X-MEDIA-SEQUENCE:10\n" +
				`#EXT-X-KEY:METHOD=AES-128,URI="key.bin"` + "\n" +
				`#EXT-X-MAP:URI="init.mp4"` + "\n" +
				"#EXTINF:1,\nseg.ts\n#EXT-X-ENDLIST\n"),
			keyStatus:      200,
			keyBody:        validKey,
			segment:        validCiphertext,
			wantKeyRequest: false,
		},
		{
			name: "unsupported-keyformat",
			playlist: aesTaskFailurePlaylist(`#EXT-X-KEY:METHOD=AES-128,URI="key.bin",` +
				`KEYFORMAT="com.apple.streamingkeydelivery",KEYFORMATVERSIONS="1"`),
			segment:        validCiphertext,
			wantKeyRequest: false,
		},
		{
			name: "unsupported-keyformat-version",
			playlist: aesTaskFailurePlaylist(`#EXT-X-KEY:METHOD=AES-128,URI="key.bin",` +
				`KEYFORMAT="identity",KEYFORMATVERSIONS="2"`),
			segment:        validCiphertext,
			wantKeyRequest: false,
		},
		{
			name:           "unsupported-sample-aes",
			playlist:       aesTaskFailurePlaylist(`#EXT-X-KEY:METHOD=SAMPLE-AES,URI="key.bin"`),
			segment:        validCiphertext,
			wantKeyRequest: false,
		},
		{
			name:           "unsupported-unknown-method",
			playlist:       aesTaskFailurePlaylist(`#EXT-X-KEY:METHOD=FOO,URI="key.bin"`),
			segment:        validCiphertext,
			wantKeyRequest: false,
		},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			var h *aesTaskHarness
			h = aesTaskNewHarness(t, "http://aes-fail.invalid/live/index.m3u8", 0,
				func(r *http.Request) (*http.Response, error) {
					switch r.URL.Path {
					case "/live/index.m3u8":
						return aesTaskResponse(200, tc.playlist), nil
					case "/live/key.bin":
						if tc.keyBodyReadError {
							return &http.Response{
								StatusCode: 200,
								Header:     make(http.Header),
								Body:       aesTaskErrorBody{},
							}, nil
						}
						status := tc.keyStatus
						if status == 0 {
							status = 200
						}
						return aesTaskResponse(status, tc.keyBody), nil
					case "/live/seg.ts":
						return aesTaskResponse(200, tc.segment), nil
					case "/live/init.mp4":
						return aesTaskResponse(200, []byte("encrypted-init")), nil
					default:
						return aesTaskResponse(404, nil), nil
					}
				})

			err := h.waitError(t)
			require.NotErrorIs(t, err, ErrClientEOS)
			require.Zero(t, h.sampleCalls.Load(), "failed object produced a sample callback")
			if tc.wantKeyRequest {
				require.GreaterOrEqual(t, h.keyRequests.Load(), int32(1), "key request did not pass OnRequest")
			} else {
				require.Zero(t, h.keyRequests.Load())
			}
		})
	}

	t.Run("cancellation-unblocks-key-request-and-wait", func(t *testing.T) {
		playlist := aesTaskFailurePlaylist(`#EXT-X-KEY:METHOD=AES-128,URI="blocked.key"`)
		keyEntered := make(chan struct{}, 1)

		errCh := make(chan error, 1)
		var h *aesTaskHarness
		h = aesTaskNewHarness(t, "http://aes-cancel.invalid/live/index.m3u8", 0,
			func(r *http.Request) (*http.Response, error) {
				switch r.URL.Path {
				case "/live/index.m3u8":
					return aesTaskResponse(200, playlist), nil
				case "/live/blocked.key":
					select {
					case keyEntered <- struct{}{}:
					default:
					}
					<-r.Context().Done()
					return nil, r.Context().Err()
				case "/live/seg.ts":
					return aesTaskResponse(200, validCiphertext), nil
				default:
					return aesTaskResponse(404, nil), nil
				}
			})

		go func() {
			errCh <- <-h.client.Wait()
		}()

		select {
		case <-keyEntered:
		case <-time.After(10 * time.Second):
			t.Fatal("key request was not started")
		}
		require.GreaterOrEqual(t, h.keyRequests.Load(), int32(1))

		closeDone := make(chan struct{})
		go func() {
			h.client.Close()
			close(closeDone)
		}()
		select {
		case <-closeDone:
		case <-time.After(10 * time.Second):
			t.Fatal("Close did not finish after cancellation")
		}

		select {
		case err := <-errCh:
			require.Error(t, err)
		case <-time.After(10 * time.Second):
			t.Fatal("Wait did not finish after cancellation")
		}
		require.Zero(t, h.sampleCalls.Load())
	})
}
