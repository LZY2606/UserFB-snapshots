package playlist

import (
	"strings"
	"testing"

	"github.com/stretchr/testify/require"
)

func TestHLSAESMapBinding(t *testing.T) {
	original := []byte("#EXTM3U\n" +
		"#EXT-X-VERSION:7\n" +
		"#EXT-X-TARGETDURATION:2\n" +
		"#EXT-X-MEDIA-SEQUENCE:41\n" +
		"#EXT-X-KEY:METHOD=AES-128,URI=\"map.key\",IV=0x00000000000000000000000000000001\n" +
		"#EXT-X-MAP:URI=\"init.mp4\"\n" +
		"#EXT-X-KEY:METHOD=AES-128,URI=\"media.key\",IV=0x00000000000000000000000000000002\n" +
		"#EXTINF:2,\n" +
		"one.mp4\n" +
		"#EXT-X-KEY:METHOD=NONE\n" +
		"#EXTINF:2,\n" +
		"two.mp4\n" +
		"#EXT-X-ENDLIST\n")

	var parsed Media
	require.NoError(t, parsed.Unmarshal(original))
	require.NotNil(t, parsed.Map)
	require.NotNil(t, parsed.Map.Key)
	require.Equal(t, MediaKeyMethodAES128, parsed.Map.Key.Method)
	require.Equal(t, "map.key", parsed.Map.Key.URI)
	require.Equal(t, "0x00000000000000000000000000000001", parsed.Map.Key.IV)

	require.Len(t, parsed.Segments, 2)
	require.NotNil(t, parsed.Segments[0].Key)
	require.Equal(t, "media.key", parsed.Segments[0].Key.URI)
	require.Equal(t, "0x00000000000000000000000000000002", parsed.Segments[0].Key.IV)
	require.NotNil(t, parsed.Segments[1].Key)
	require.Equal(t, MediaKeyMethodNone, parsed.Segments[1].Key.Method)

	// The MAP snapshot and the segment bindings must not alias each other.
	require.NotSame(t, parsed.Map.Key, parsed.Segments[0].Key)

	marshaled, err := parsed.Marshal()
	require.NoError(t, err)
	text := string(marshaled)

	mapKeyPos := strings.Index(text, "URI=\"map.key\"")
	mapPos := strings.Index(text, "#EXT-X-MAP:")
	mediaKeyPos := strings.Index(text, "URI=\"media.key\"")
	firstSegmentPos := strings.Index(text, "one.mp4")
	nonePos := strings.Index(text, "#EXT-X-KEY:METHOD=NONE")
	secondSegmentPos := strings.Index(text, "two.mp4")
	require.Greater(t, mapKeyPos, -1)
	require.Less(t, mapKeyPos, mapPos)
	require.Less(t, mapPos, mediaKeyPos)
	require.Less(t, mediaKeyPos, firstSegmentPos)
	require.Less(t, firstSegmentPos, nonePos)
	require.Less(t, nonePos, secondSegmentPos)

	var reparsed Media
	require.NoError(t, reparsed.Unmarshal(marshaled))
	require.Equal(t, parsed.Map.Key, reparsed.Map.Key)
	require.Equal(t, parsed.Segments[0].Key, reparsed.Segments[0].Key)
	require.Equal(t, parsed.Segments[1].Key, reparsed.Segments[1].Key)

	// An encrypted MAP followed by a segment without an explicit key binding
	// must return to plaintext before writing that segment.
	mapKey := &MediaKey{
		Method: MediaKeyMethodAES128,
		URI:    "map-a.key",
		IV:     "0x01",
	}
	mapKeyBefore := *mapKey
	pl := Media{
		Version:        7,
		TargetDuration: 2,
		Map: &MediaMap{
			URI: "init.mp4",
			Key: mapKey,
		},
		Segments: []*MediaSegment{{
			Duration: 2_000_000_000,
			URI:      "plain-after-map.mp4",
		}},
	}

	out, err := pl.Marshal()
	require.NoError(t, err)
	require.Equal(t, mapKeyBefore, *mapKey)

	outText := string(out)
	require.Less(t,
		strings.Index(outText, "map-a.key"),
		strings.Index(outText, "#EXT-X-MAP:"))
	require.Less(t,
		strings.Index(outText, "#EXT-X-MAP:"),
		strings.Index(outText, "#EXT-X-KEY:METHOD=NONE"))
	require.Less(t,
		strings.Index(outText, "#EXT-X-KEY:METHOD=NONE"),
		strings.Index(outText, "plain-after-map.mp4"))

	var reparsedPlain Media
	require.NoError(t, reparsedPlain.Unmarshal(out))
	require.NotNil(t, reparsedPlain.Map.Key)
	require.Equal(t, "map-a.key", reparsedPlain.Map.Key.URI)
	require.NotNil(t, reparsedPlain.Segments[0].Key)
	require.Equal(t, MediaKeyMethodNone, reparsedPlain.Segments[0].Key.Method)

	// Lists without encryption remain free of KEY tags.
	plain := Media{
		Version:        3,
		TargetDuration: 1,
		Segments: []*MediaSegment{{
			Duration: 1_000_000_000,
			URI:      "one.ts",
		}},
	}
	plainOut, err := plain.Marshal()
	require.NoError(t, err)
	require.NotContains(t, string(plainOut), "#EXT-X-KEY")
}
