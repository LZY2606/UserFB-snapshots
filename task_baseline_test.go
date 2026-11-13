package gohlslib

import (
	"bytes"
	"fmt"
	"io"
	"net/http"
	"reflect"
	"strings"
	"testing"
	"time"

	"github.com/bluenviron/gohlslib/v2/pkg/playlist"
	"github.com/bluenviron/mediacommon/v2/pkg/formats/mpegts"
	tscodecs "github.com/bluenviron/mediacommon/v2/pkg/formats/mpegts/codecs"
)

type taskBaselineTransport func(*http.Request) (*http.Response, error)

func (f taskBaselineTransport) RoundTrip(r *http.Request) (*http.Response, error) { return f(r) }

func TestTaskBaselineClient(t *testing.T) {
	var media bytes.Buffer
	track := &mpegts.Track{Codec: &tscodecs.H264{}}
	writer := &mpegts.Writer{W: &media, Tracks: []*mpegts.Track{track}}
	if err := writer.Initialize(); err != nil {
		t.Fatal(err)
	}
	if err := writer.WriteH264(track, 90000, 90000, [][]byte{{7, 1, 2, 3}, {8}, {5}}); err != nil {
		t.Fatal(err)
	}
	received := make(chan [][]byte, 2)
	client := &Client{URI: "http://baseline.invalid/live/index.m3u8"}
	client.HTTPClient = &http.Client{Transport: taskBaselineTransport(func(r *http.Request) (*http.Response, error) {
		var body []byte
		switch r.URL.Path {
		case "/live/index.m3u8":
			body = []byte("#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:1\n#EXT-X-MEDIA-SEQUENCE:41\n#EXTINF:1,\none.ts\n#EXT-X-ENDLIST\n")
		case "/live/one.ts":
			body = media.Bytes()
		default:
			return nil, fmt.Errorf("unexpected request %s", r.URL.Path)
		}
		return &http.Response{StatusCode: 200, Header: make(http.Header), Body: io.NopCloser(bytes.NewReader(body)), Request: r}, nil
	})}
	client.OnTracks = func(tracks []*Track) error {
		if len(tracks) != 1 {
			return fmt.Errorf("tracks=%d", len(tracks))
		}
		client.OnDataH26x(tracks[0], func(_ int64, _ int64, au [][]byte) {
			copyAU := make([][]byte, len(au))
			for i := range au {
				copyAU[i] = bytes.Clone(au[i])
			}
			received <- copyAU
		})
		return nil
	}
	if err := client.Start(); err != nil {
		t.Fatal(err)
	}
	defer client.Close()
	select {
	case au := <-received:
		if len(au) != 3 || !bytes.Equal(au[0], []byte{7, 1, 2, 3}) || !bytes.Equal(au[2], []byte{5}) {
			t.Fatalf("unexpected AU %v", au)
		}
	case <-time.After(10 * time.Second):
		t.Fatal("client callback timed out")
	}
	select {
	case err := <-client.Wait():
		if err != ErrClientEOS {
			t.Fatalf("EOS: %v", err)
		}
	case <-time.After(10 * time.Second):
		t.Fatal("EOS timed out")
	}
	t.Log("BASELINE_REAL_TS samples=1 sequence=41 EOS=true")
}

func TestTaskBaselineKeyRepresentation(t *testing.T) {
	var pl playlist.Media
	data := []byte("#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-TARGETDURATION:1\n#EXT-X-MEDIA-SEQUENCE:41\n#EXT-X-KEY:METHOD=AES-128,URI=\"map.key\",IV=0x00000000000000000000000000000001\n#EXT-X-MAP:URI=\"init.mp4\"\n#EXT-X-KEY:METHOD=AES-128,URI=\"media.key\"\n#EXTINF:1,\none.mp4\n#EXT-X-ENDLIST\n")
	if err := pl.Unmarshal(data); err != nil {
		t.Fatal(err)
	}
	if pl.Map == nil || len(pl.Segments) != 1 || pl.Segments[0].Key.URI != "media.key" {
		t.Fatal("segment key parsing failed")
	}
	out, err := pl.Marshal()
	if err != nil {
		t.Fatal(err)
	}
	_, hasKey := reflect.TypeOf(*pl.Map).FieldByName("Key")
	t.Logf("OBSERVED_MAP_KEY_FIELD=%t MAP_KEY_SURVIVES_ROUNDTRIP=%t", hasKey, strings.Contains(string(out), "map.key"))
}
