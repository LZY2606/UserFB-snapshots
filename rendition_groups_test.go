package m3u8

import (
	"bytes"
	"strings"
	"testing"

	"github.com/grafov/m3u8/task_support/taskcheck"
)

// renditionFixture exercises every behavior the catalog has to carry:
// a shared audio group, same-named subtitles in separate groups, a group no
// variant references, an unquoted INSTREAM-ID and the NONE closed captions
// sentinel.
const renditionFixture = `#EXTM3U
#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="audio",NAME="English",DEFAULT=YES,AUTOSELECT=YES,LANGUAGE="en",URI="audio/en.m3u8"
#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="audio",NAME="French",DEFAULT=NO,AUTOSELECT=YES,LANGUAGE="fr",URI="audio/fr.m3u8"
#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID="subs-main",NAME="English",DEFAULT=NO,AUTOSELECT=YES,LANGUAGE="en",URI="subs/en.m3u8"
#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID="subs-alt",NAME="English",DEFAULT=NO,AUTOSELECT=YES,LANGUAGE="en",URI="subs-alt/en.m3u8"
#EXT-X-MEDIA:TYPE=CLOSED-CAPTIONS,GROUP-ID="cc",NAME="CC1",DEFAULT=YES,INSTREAM-ID=CC1
#EXT-X-MEDIA:TYPE=CLOSED-CAPTIONS,GROUP-ID="cc",NAME="CC2",DEFAULT=NO,INSTREAM-ID=CC2
#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="spare",NAME="Commentary",DEFAULT=NO,URI="audio/commentary.m3u8"
#EXT-X-STREAM-INF:BANDWIDTH=1280000,AUDIO="audio",SUBTITLES="subs-main",CLOSED-CAPTIONS="cc"
low.m3u8
#EXT-X-STREAM-INF:BANDWIDTH=2560000,AUDIO="audio",SUBTITLES="subs-alt"
high.m3u8
`

func decodeMaster(t *testing.T, text string) *MasterPlaylist {
	t.Helper()
	p := NewMasterPlaylist()
	if err := p.DecodeFrom(strings.NewReader(text), true); err != nil {
		t.Fatalf("DecodeFrom: %v", err)
	}
	return p
}

func checksum(alts []*Alternative) []string {
	out := make([]string, 0, len(alts))
	for _, alt := range alts {
		out = append(out, alt.Type+"/"+alt.GroupId+"/"+alt.Name)
	}
	return out
}

func containsAll(text string, needles ...string) bool {
	for _, n := range needles {
		if !strings.Contains(text, n) {
			return false
		}
	}
	return true
}

func TestRenditionCatalog(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		c.Check(len(renditionFixture) > 0, "fixture missing")
		p := decodeMaster(t, renditionFixture)

		c.Check(len(p.Renditions) == 7, "catalog should keep all 7 declarations, got %d", len(p.Renditions))
		if len(p.Renditions) == 7 {
			c.Check(p.Renditions[0].GroupId == "audio" && p.Renditions[0].Name == "English", "first catalog entry order lost: %+v", p.Renditions[0])
			c.Check(p.Renditions[6].GroupId == "spare", "unreferenced group must be last in declaration order, got %q", p.Renditions[6].GroupId)
		}

		var spare *Alternative
		for _, alt := range p.Renditions {
			if alt.GroupId == "spare" {
				spare = alt
			}
		}
		referenced := false
		for _, v := range p.Variants {
			for _, alt := range v.Alternatives {
				if alt == spare {
					referenced = true
				}
			}
		}
		c.Check(spare != nil, "unreferenced rendition missing from catalog")
		c.Check(!referenced, "unreferenced rendition must not be attached to a variant")

		var captions *Alternative
		for _, alt := range p.Renditions {
			if alt.Type == "CLOSED-CAPTIONS" && alt.Name == "CC1" {
				captions = alt
			}
		}
		c.Check(captions != nil && captions.InstreamId == "CC1", "INSTREAM-ID not parsed: %+v", captions)

		// The same audio group is referenced by both variants; each variant
		// gets only the entries of its own groups, once.
		c.Check(len(p.Variants) == 2, "variants = %d", len(p.Variants))
		audioLinks := 0
		for _, v := range p.Variants {
			seen := make(map[*Alternative]int)
			for _, alt := range v.Alternatives {
				seen[alt]++
			}
			for alt, n := range seen {
				c.Check(n == 1, "rendition %s attached %d times to variant %s", alt.Name, n, v.URI)
				if alt.Type == "AUDIO" && alt.GroupId == "audio" {
					audioLinks++
				}
			}
		}
		c.Check(audioLinks == 4, "two audio renditions across two variants = 4 links, got %d", audioLinks)

		joined := strings.Join(checksum(p.Renditions), ",")
		c.Check(containsAll(joined, "AUDIO/audio/English", "SUBTITLES/subs-alt/English"), "catalog identity lost: %s", joined)

		// Same-named subtitles in different groups must stay distinct.
		c.Check(len(p.Variants[0].Alternatives) == 5, "low variant should link two audio, one subtitle and two captions, got %d", len(p.Variants[0].Alternatives))
		c.Check(len(p.Variants[1].Alternatives) == 3, "high variant should link two audio and one subtitle rendition, got %d", len(p.Variants[1].Alternatives))

		// Strict parsing rejects conflicting redeclarations of one identity.
		conflict := `#EXTM3U
#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="audio",NAME="English",URI="one.m3u8"
#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="audio",NAME="English",URI="two.m3u8"
#EXT-X-STREAM-INF:BANDWIDTH=100,AUDIO="audio"
low.m3u8
`
		bad := NewMasterPlaylist()
		err := bad.DecodeFrom(strings.NewReader(conflict), true)
		c.Check(err != nil, "conflicting duplicate declaration should fail in strict mode")
		if err != nil {
			c.Check(strings.Contains(err.Error(), "audio") && strings.Contains(err.Error(), "English"), "conflict error should locate the identity: %v", err)
		}

		// Identical duplicates collapse into one entry.
		dup := `#EXTM3U
#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="audio",NAME="English",URI="one.m3u8"
#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="audio",NAME="English",URI="one.m3u8"
#EXT-X-STREAM-INF:BANDWIDTH=100,AUDIO="audio"
low.m3u8
`
		pdup := decodeMaster(t, dup)
		c.Check(len(pdup.Renditions) == 1, "identical duplicates should collapse, got %d", len(pdup.Renditions))
		c.Check(len(pdup.Variants[0].Alternatives) == 1, "duplicate must not be attached twice")

		// URI is not identity: equal URIs with different groups stay separate.
		sameURI := `#EXTM3U
#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="a",NAME="Main",URI="shared.m3u8"
#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="b",NAME="Main",URI="shared.m3u8"
#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID="a",NAME="Main",URI="shared.m3u8"
#EXT-X-STREAM-INF:BANDWIDTH=100,AUDIO="a"
low.m3u8
`
		psame := decodeMaster(t, sameURI)
		c.Check(len(psame.Renditions) == 3, "equal URI must not fold renditions of different type/group, got %d", len(psame.Renditions))

		// Decode autodetection must produce the same catalog.
		auto, listType, err := DecodeFrom(strings.NewReader(renditionFixture), true)
		c.Check(err == nil, "DecodeFrom autodetect: %v", err)
		c.Check(listType == MASTER, "autodetect list type = %v", listType)
		if am, ok := auto.(*MasterPlaylist); ok {
			c.Check(len(am.Renditions) == len(p.Renditions), "autodetect catalog size %d != %d", len(am.Renditions), len(p.Renditions))
			c.Check(strings.Join(checksum(am.Renditions), ",") == strings.Join(checksum(p.Renditions), ","), "autodetect catalog differs from direct decode")
		} else {
			c.Check(false, "autodetected playlist is not a master playlist")
		}

		// Catalog-only playlist with no variants still encodes its catalog.
		only := `#EXTM3U
#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="audio",NAME="English",DEFAULT=YES,URI="audio/en.m3u8"
`
		po := decodeMaster(t, only)
		out := po.Encode().String()
		c.Check(strings.Contains(out, `#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="audio",NAME="English"`), "empty-variant catalog not encoded:\n%s", out)

		// Shared groups are emitted once, INSTREAM-ID unquoted, URI-less
		// captions keep their attributes.
		encoded := p.Encode().String()
		c.Check(strings.Count(encoded, `GROUP-ID="audio"`) == 2, "audio group declarations duplicated:\n%s", encoded)
		c.Check(strings.Count(encoded, "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"audio\"") == 2, "shared group encoded wrong number of times:\n%s", encoded)
		c.Check(strings.Contains(encoded, ",INSTREAM-ID=CC1"), "INSTREAM-ID must be unquoted:\n%s", encoded)
		c.Check(strings.Contains(encoded, `NAME="CC1",DEFAULT=YES,INSTREAM-ID=CC1`), "URI-less caption attributes lost:\n%s", encoded)
		c.Check(strings.Count(encoded, `GROUP-ID="spare"`) == 1, "unreferenced group not encoded exactly once:\n%s", encoded)

		// Re-parsing the encoded output yields the same catalog.
		round := decodeMaster(t, encoded)
		c.Check(strings.Join(checksum(round.Renditions), ",") == strings.Join(checksum(p.Renditions), ","), "catalog changed after round trip")

		// Legacy alternatives survive alongside the catalog.
		legacy := NewMasterPlaylist()
		legacyAlt := &Alternative{Type: "AUDIO", GroupId: "legacy", Name: "Main", URI: "legacy.m3u8"}
		legacy.Append("low.m3u8", nil, VariantParams{Alternatives: []*Alternative{legacyAlt}})
		legacy.Renditions = append(legacy.Renditions, &Alternative{Type: "AUDIO", GroupId: "catalog", Name: "Main", URI: "catalog.m3u8"})
		lout := legacy.Encode().String()
		c.Check(strings.Index(lout, `GROUP-ID="catalog"`) < strings.Index(lout, `GROUP-ID="legacy"`), "catalog entries must precede legacy entries:\n%s", lout)
		c.Check(strings.Count(lout, `GROUP-ID="catalog"`) == 1 && strings.Count(lout, `GROUP-ID="legacy"`) == 1, "legacy/catalog merge wrong:\n%s", lout)
	})
}

func TestRenditionRename(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		p := decodeMaster(t, renditionFixture)

		// A -> B and B -> A applied to the names as they were before the call.
		err := p.RenameRenditionGroups(map[RenditionGroupKey]string{
			{Type: "AUDIO", GroupID: "audio"}:         "renamed",
			{Type: "SUBTITLES", GroupID: "subs-main"}: "subs-renamed",
		})
		c.Check(err == nil, "rename failed: %v", err)

		names := make(map[string]bool)
		for _, alt := range p.Renditions {
			names[alt.Type+"/"+alt.GroupId+"/"+alt.Name] = true
		}
		c.Check(names["AUDIO/renamed/English"] && names["AUDIO/renamed/French"], "catalog audio group not renamed: %v", names)
		c.Check(names["SUBTITLES/subs-renamed/English"], "catalog subtitles group not renamed")
		c.Check(names["AUDIO/spare/Commentary"], "unmapped group should stay untouched")
		c.Check(p.Variants[0].Audio == "renamed" && p.Variants[1].Audio == "renamed", "variant AUDIO references not renamed")
		c.Check(p.Variants[0].Subtitles == "subs-renamed" && p.Variants[1].Subtitles == "subs-alt", "variant SUBTITLES references wrong")
		for _, v := range p.Variants {
			for _, alt := range v.Alternatives {
				if alt.GroupId == "audio" || alt.GroupId == "subs-main" {
					c.Check(false, "legacy alternatives not renamed: %+v", alt)
				}
			}
		}

		// Encoding after rename uses the new names.
		out := p.Encode().String()
		c.Check(strings.Contains(out, `GROUP-ID="renamed"`), "encoded output did not use new group name:\n%s", out)
		c.Check(!strings.Contains(out, `GROUP-ID="audio"`), "encoded output still has old group name:\n%s", out)
		round := decodeMaster(t, out)
		c.Check(len(round.Renditions) == len(p.Renditions), "renamed round trip changed catalog size")

		// Swap a->b, b->a in a single call.
		swap := decodeMaster(t, renditionFixture)
		err = swap.RenameRenditionGroups(map[RenditionGroupKey]string{
			{Type: "AUDIO", GroupID: "audio"}: "spare",
			{Type: "AUDIO", GroupID: "spare"}: "audio",
		})
		c.Check(err == nil, "swap rename failed: %v", err)
		groups := make(map[string]string)
		for _, alt := range swap.Renditions {
			if alt.Type == "AUDIO" {
				groups[alt.Name] = alt.GroupId
			}
		}
		c.Check(groups["English"] == "spare" && groups["French"] == "spare", "swap did not move audio group: %v", groups)
		c.Check(groups["Commentary"] == "audio", "swap did not move spare group: %v", groups)
		c.Check(swap.Variants[0].Audio == "spare", "swap did not update variant reference")

		// Renaming an unreferenced-only group works.
		spareOnly := decodeMaster(t, `#EXTM3U
#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="spare",NAME="Commentary",URI="c.m3u8"
`)
		err = spareOnly.RenameRenditionGroups(map[RenditionGroupKey]string{{Type: "AUDIO", GroupID: "spare"}: "commentary"})
		c.Check(err == nil, "rename unreferenced group failed: %v", err)
		c.Check(spareOnly.Renditions[0].GroupId == "commentary", "unreferenced group not renamed")

		// Same group name under different types is independent.
		two := decodeMaster(t, `#EXTM3U
#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="shared",NAME="A",URI="a.m3u8"
#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID="shared",NAME="S",URI="s.m3u8"
`)
		err = two.RenameRenditionGroups(map[RenditionGroupKey]string{{Type: "AUDIO", GroupID: "shared"}: "only-audio"})
		c.Check(err == nil, "type-scoped rename failed: %v", err)
		c.Check(two.Renditions[0].GroupId == "only-audio" && two.Renditions[1].GroupId == "shared", "rename leaked across types")

		// Error cases leave the playlist untouched.
		checkUnchanged := func(label string, renames map[RenditionGroupKey]string, want string) {
			q := decodeMaster(t, renditionFixture)
			snapshot := q.Encode().String()
			verr := q.RenameRenditionGroups(renames)
			c.Check(verr != nil, "%s: expected error", label)
			if verr != nil {
				c.Check(strings.Contains(verr.Error(), want), "%s: error %q should locate %q", label, verr, want)
			}
			c.Check(q.Encode().String() == snapshot, "%s: playlist changed on error", label)
			c.Check(q.Renditions[6].GroupId == "spare", "%s: catalog mutated on error", label)
		}
		checkUnchanged("missing source", map[RenditionGroupKey]string{{Type: "AUDIO", GroupID: "nope"}: "x"}, "nope")
		checkUnchanged("empty target", map[RenditionGroupKey]string{{Type: "AUDIO", GroupID: "audio"}: ""}, "audio")
		checkUnchanged("unknown type", map[RenditionGroupKey]string{{Type: "TEXT", GroupID: "audio"}: "x"}, "TEXT")
		checkUnchanged("collision", map[RenditionGroupKey]string{
			{Type: "SUBTITLES", GroupID: "subs-main"}: "subs-alt",
		}, "subs-alt")
		checkUnchanged("captions NONE", map[RenditionGroupKey]string{{Type: "CLOSED-CAPTIONS", GroupID: "cc"}: "NONE"}, "NONE")

		// CLOSED-CAPTIONS=NONE is not a reference and stays untouched.
		noneText := `#EXTM3U
#EXT-X-MEDIA:TYPE=CLOSED-CAPTIONS,GROUP-ID="cc",NAME="CC1",INSTREAM-ID=CC1
#EXT-X-STREAM-INF:BANDWIDTH=100,CLOSED-CAPTIONS=NONE
low.m3u8
`
		none := decodeMaster(t, noneText)
		err = none.RenameRenditionGroups(map[RenditionGroupKey]string{{Type: "CLOSED-CAPTIONS", GroupID: "cc"}: "captions"})
		c.Check(err == nil, "rename captions group failed: %v", err)
		c.Check(none.Variants[0].Captions == "NONE", "CLOSED-CAPTIONS=NONE must remain NONE")

		// Two successive renames and an encode/read round trip stay duplicate free.
		twice := decodeMaster(t, renditionFixture)
		c.Check(twice.RenameRenditionGroups(map[RenditionGroupKey]string{{Type: "AUDIO", GroupID: "audio"}: "first"}) == nil, "first rename failed")
		c.Check(twice.RenameRenditionGroups(map[RenditionGroupKey]string{{Type: "AUDIO", GroupID: "first"}: "second"}) == nil, "second rename failed")
		twiceOut := twice.Encode().String()
		rt := decodeMaster(t, twiceOut)
		c.Check(len(rt.Renditions) == len(twice.Renditions), "double rename produced duplicates: %d vs %d", len(rt.Renditions), len(twice.Renditions))
		c.Check(strings.Count(twiceOut, `GROUP-ID="second"`) == 2, "double rename encoded wrong declarations:\n%s", twiceOut)

		// Empty map is a successful no-op.
		empty := decodeMaster(t, renditionFixture)
		snapshot := empty.Encode().String()
		c.Check(empty.RenameRenditionGroups(nil) == nil, "nil map should be a no-op")
		c.Check(empty.Encode().String() == snapshot, "nil map changed the playlist")

		// Rename after Encode: the cached encoding must reflect the new names.
		pre := decodeMaster(t, renditionFixture)
		_ = pre.Encode()
		c.Check(pre.RenameRenditionGroups(map[RenditionGroupKey]string{{Type: "AUDIO", GroupID: "audio"}: "post"}) == nil, "rename after encode failed")
		c.Check(strings.Contains(pre.Encode().String(), `GROUP-ID="post"`), "cached encode not invalidated after rename")
	})
}

func TestRenditionLegacy(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Legacy Append-built alternatives are merged with the catalog,
		// catalog first, each identity once.
		m := NewMasterPlaylist()
		legacyAudio := &Alternative{Type: "AUDIO", GroupId: "audio", Name: "Main", URI: "audio.m3u8"}
		m.Append("low.m3u8", nil, VariantParams{Audio: "audio", Alternatives: []*Alternative{legacyAudio}})
		m.Variants = append(m.Variants, &Variant{
			URI:           "high.m3u8",
			VariantParams: VariantParams{Audio: "audio", Alternatives: []*Alternative{legacyAudio}},
		})
		m.Renditions = []*Alternative{
			{Type: "AUDIO", GroupId: "audio", Name: "Main", URI: "audio.m3u8"},
			{Type: "AUDIO", GroupId: "audio", Name: "Commentary", URI: "commentary.m3u8"},
			{Type: "AUDIO", GroupId: "unused", Name: "Unused", URI: "unused.m3u8"},
		}
		out := m.Encode().String()
		c.Check(strings.Count(out, `#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="audio",NAME="Main"`) == 1, "shared legacy/catalog identity emitted more than once:\n%s", out)
		c.Check(strings.Count(out, `GROUP-ID="audio"`) == 2, "audio group should have two names:\n%s", out)
		c.Check(strings.Contains(out, `GROUP-ID="unused"`), "unreferenced catalog entry dropped:\n%s", out)
		c.Check(m.Version() == 4, "Append with alternatives should keep version 4, got %d", m.Version())

		// CLOSED-CAPTIONS=NONE keeps its unquoted spelling.
		none := NewMasterPlaylist()
		none.Append("low.m3u8", nil, VariantParams{Captions: "NONE"})
		c.Check(strings.Contains(none.Encode().String(), "CLOSED-CAPTIONS=NONE"), "CLOSED-CAPTIONS=NONE broken")

		// Encoding twice in a row is stable and duplicate free.
		first := m.Encode().String()
		m.ResetCache()
		second := m.Encode().String()
		c.Check(first == second, "consecutive encodes differ")

		// Custom tags and I-frame variants keep working.
		custom := NewMasterPlaylist()
		custom.SetCustomTag(testMasterTag("#EXT-X-CUSTOM:value"))
		custom.Append("iframe.m3u8", nil, VariantParams{Bandwidth: 90000, Iframe: true})
		custom.Renditions = append(custom.Renditions, &Alternative{Type: "AUDIO", GroupId: "audio", Name: "Only", URI: "only.m3u8"})
		customOut := custom.Encode().String()
		c.Check(strings.Contains(customOut, "#EXT-X-CUSTOM:value"), "custom master tag lost:\n%s", customOut)
		c.Check(strings.Contains(customOut, "#EXT-X-I-FRAME-STREAM-INF:"), "I-frame variant lost:\n%s", customOut)
		c.Check(strings.Contains(customOut, `#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="audio",NAME="Only"`), "catalog lost with custom tags:\n%s", customOut)

		// Legacy-only playlist (no catalog) still encodes, as before.
		legacy := NewMasterPlaylist()
		legacy.Append("chunklist1.m3u8", nil, VariantParams{Alternatives: []*Alternative{
			{GroupId: "audio", URI: "rendition.m3u8", Type: "AUDIO", Name: "main", Default: true, Autoselect: "YES", Language: "english"},
		}})
		expected := `#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="audio",NAME="main",DEFAULT=YES,AUTOSELECT=YES,LANGUAGE="english",URI="rendition.m3u8"`
		c.Check(strings.Contains(legacy.Encode().String(), expected), "legacy alternative encoding changed:\n%s", legacy.Encode().String())

		// Decoding a legacy sample file still links and encodes both
		// referenced and unreferenced material without duplicates.
		text := `#EXTM3U
#EXT-X-MEDIA:TYPE=VIDEO,GROUP-ID="low",NAME="Main",DEFAULT=YES,URI="low/main.m3u8"
#EXT-X-MEDIA:TYPE=VIDEO,GROUP-ID="low",NAME="Dugout",DEFAULT=NO,URI="low/dugout.m3u8"
#EXT-X-STREAM-INF:BANDWIDTH=1280000,VIDEO="low"
low/main.m3u8
`
		p := decodeMaster(t, text)
		c.Check(len(p.Variants) == 1 && len(p.Variants[0].Alternatives) == 2, "legacy decode linking wrong: %d", len(p.Variants[0].Alternatives))
		enc := p.Encode().String()
		c.Check(strings.Count(enc, `GROUP-ID="low"`) == 2, "decode/encode duplicated renditions:\n%s", enc)
		c.Check(bytes.Equal([]byte(p.String()), []byte(enc)), "String() and Encode() disagree")
	})
}

// testMasterTag is a minimal CustomTag used to exercise custom master tags.
type testMasterTag string

func (t testMasterTag) TagName() string { return "#EXT-X-CUSTOM:" }
func (t testMasterTag) String() string  { return string(t) }
func (t testMasterTag) Encode() *bytes.Buffer {
	b := new(bytes.Buffer)
	b.WriteString(string(t))
	return b
}
