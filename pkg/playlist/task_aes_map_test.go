package playlist

import (
	"testing"

	"github.com/stretchr/testify/require"
)

// TestHLSAESMapBinding checks that the key in effect when a MAP is declared
// is bound to the MAP itself and survives a parse/marshal/parse round trip,
// independently from the keys of the media segments.
func TestHLSAESMapBinding(t *testing.T) {
	t.Run("map key is independent from segment keys", func(t *testing.T) {
		in := "#EXTM3U\n" +
			"#EXT-X-VERSION:7\n" +
			"#EXT-X-TARGETDURATION:2\n" +
			"#EXT-X-MEDIA-SEQUENCE:40\n" +
			"#EXT-X-KEY:METHOD=AES-128,URI=\"keyA.bin\",IV=0x00000000000000000000000000000001\n" +
			"#EXT-X-MAP:URI=\"init.mp4\"\n" +
			"#EXT-X-KEY:METHOD=AES-128,URI=\"keyB.bin\"\n" +
			"#EXTINF:2,\n" +
			"seg1.mp4\n" +
			"#EXT-X-KEY:METHOD=NONE\n" +
			"#EXTINF:2,\n" +
			"seg2.mp4\n"

		var pl Media
		require.NoError(t, pl.Unmarshal([]byte(in)))

		require.NotNil(t, pl.Map)
		require.NotNil(t, pl.Map.Key)
		require.Equal(t, MediaKeyMethodAES128, pl.Map.Key.Method)
		require.Equal(t, "keyA.bin", pl.Map.Key.URI)
		require.Equal(t, "0x00000000000000000000000000000001", pl.Map.Key.IV)

		require.Len(t, pl.Segments, 2)
		require.Equal(t, "keyB.bin", pl.Segments[0].Key.URI)
		require.Equal(t, MediaKeyMethodNone, pl.Segments[1].Key.Method)

		// the segment keys and the map key must not share mutable state
		require.NotSame(t, pl.Map.Key, pl.Segments[0].Key)

		out, err := pl.Marshal()
		require.NoError(t, err)

		var pl2 Media
		require.NoError(t, pl2.Unmarshal(out))

		require.NotNil(t, pl2.Map)
		require.NotNil(t, pl2.Map.Key)
		require.Equal(t, "keyA.bin", pl2.Map.Key.URI)
		require.Equal(t, "0x00000000000000000000000000000001", pl2.Map.Key.IV)
		require.Equal(t, "keyB.bin", pl2.Segments[0].Key.URI)
		require.Equal(t, MediaKeyMethodNone, pl2.Segments[1].Key.Method)
	})

	t.Run("marshal writes map key before map", func(t *testing.T) {
		pl := Media{
			Version:        7,
			TargetDuration: 2,
			Map: &MediaMap{
				URI: "init.mp4",
				Key: &MediaKey{
					Method: MediaKeyMethodAES128,
					URI:    "map.key",
					IV:     "0x01",
				},
			},
			Segments: []*MediaSegment{
				{
					Duration: 2e9,
					URI:      "seg1.mp4",
					Key: &MediaKey{
						Method: MediaKeyMethodAES128,
						URI:    "media.key",
					},
				},
			},
		}

		out, err := pl.Marshal()
		require.NoError(t, err)

		s := string(out)
		require.Contains(t, s, "#EXT-X-KEY:METHOD=AES-128,URI=\"map.key\",IV=0x01\n#EXT-X-MAP:URI=\"init.mp4\"\n")
		require.Contains(t, s, "#EXT-X-KEY:METHOD=AES-128,URI=\"media.key\"\n#EXTINF:")
	})

	t.Run("clear map followed by encrypted segment", func(t *testing.T) {
		in := "#EXTM3U\n" +
			"#EXT-X-VERSION:7\n" +
			"#EXT-X-TARGETDURATION:2\n" +
			"#EXT-X-MAP:URI=\"init.mp4\"\n" +
			"#EXT-X-KEY:METHOD=AES-128,URI=\"keyA.bin\"\n" +
			"#EXTINF:2,\n" +
			"seg1.mp4\n"

		var pl Media
		require.NoError(t, pl.Unmarshal([]byte(in)))
		require.Nil(t, pl.Map.Key)
		require.Equal(t, "keyA.bin", pl.Segments[0].Key.URI)

		out, err := pl.Marshal()
		require.NoError(t, err)

		var pl2 Media
		require.NoError(t, pl2.Unmarshal(out))
		require.Nil(t, pl2.Map.Key)
		require.Equal(t, "keyA.bin", pl2.Segments[0].Key.URI)
	})

	t.Run("encrypted map followed by clear segment", func(t *testing.T) {
		in := "#EXTM3U\n" +
			"#EXT-X-VERSION:7\n" +
			"#EXT-X-TARGETDURATION:2\n" +
			"#EXT-X-MEDIA-SEQUENCE:10\n" +
			"#EXT-X-KEY:METHOD=AES-128,URI=\"map.key\",IV=0x02\n" +
			"#EXT-X-MAP:URI=\"init.mp4\"\n" +
			"#EXT-X-KEY:METHOD=NONE\n" +
			"#EXTINF:2,\n" +
			"seg1.mp4\n"

		var pl Media
		require.NoError(t, pl.Unmarshal([]byte(in)))
		require.NotNil(t, pl.Map.Key)
		require.Equal(t, MediaKeyMethodNone, pl.Segments[0].Key.Method)

		out, err := pl.Marshal()
		require.NoError(t, err)

		// the MAP key is followed by a METHOD=NONE that restores clear playback
		s := string(out)
		require.Contains(t, s, "#EXT-X-MAP:URI=\"init.mp4\"\n#EXT-X-KEY:METHOD=NONE\n#EXTINF:")

		var pl2 Media
		require.NoError(t, pl2.Unmarshal(out))
		require.Equal(t, "map.key", pl2.Map.Key.URI)
		require.NotNil(t, pl2.Segments[0].Key)
		require.Equal(t, MediaKeyMethodNone, pl2.Segments[0].Key.Method)
	})

	t.Run("calling marshal does not modify caller keys", func(t *testing.T) {
		mapKey := &MediaKey{Method: MediaKeyMethodAES128, URI: "map.key", IV: "0x03"}
		segKey := &MediaKey{Method: MediaKeyMethodAES128, URI: "media.key"}
		pl := Media{
			Version:        7,
			TargetDuration: 2,
			Map:            &MediaMap{URI: "init.mp4", Key: mapKey},
			Segments: []*MediaSegment{
				{Duration: 2e9, URI: "seg1.mp4", Key: segKey},
			},
		}

		_, err := pl.Marshal()
		require.NoError(t, err)
		require.Equal(t, &MediaKey{Method: MediaKeyMethodAES128, URI: "map.key", IV: "0x03"}, mapKey)
		require.Equal(t, &MediaKey{Method: MediaKeyMethodAES128, URI: "media.key"}, segKey)
	})

	t.Run("encrypted map and clear first segment needs METHOD=NONE", func(t *testing.T) {
		pl := Media{
			Version:        7,
			TargetDuration: 2,
			Map: &MediaMap{
				URI: "init.mp4",
				Key: &MediaKey{Method: MediaKeyMethodAES128, URI: "map.key", IV: "0x04"},
			},
			Segments: []*MediaSegment{
				{Duration: 2e9, URI: "seg1.mp4"},
			},
		}

		out, err := pl.Marshal()
		require.NoError(t, err)

		s := string(out)
		require.Contains(t, s, "#EXT-X-KEY:METHOD=AES-128,URI=\"map.key\",IV=0x04\n")
		require.Contains(t, s, "#EXT-X-MAP:URI=\"init.mp4\"\n#EXT-X-KEY:METHOD=NONE\n#EXTINF:")
	})

	t.Run("legacy plain playlists stay unchanged", func(t *testing.T) {
		in := "#EXTM3U\n" +
			"#EXT-X-VERSION:3\n" +
			"#EXT-X-TARGETDURATION:2\n" +
			"#EXT-X-MEDIA-SEQUENCE:0\n" +
			"#EXTINF:2.00000,\n" +
			"seg1.ts\n" +
			"#EXTINF:2.00000,\n" +
			"seg2.ts\n"

		var pl Media
		require.NoError(t, pl.Unmarshal([]byte(in)))
		require.Nil(t, pl.Map)
		require.Nil(t, pl.Segments[0].Key)
		require.Nil(t, pl.Segments[1].Key)

		out, err := pl.Marshal()
		require.NoError(t, err)
		require.Equal(t, in, string(out))
	})
}
