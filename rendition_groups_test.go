package m3u8

/*
 Rendition catalog and rendition group renaming tests.

 Copyright 2013-2019 The Project Developers.
 See the AUTHORS and LICENSE files at the top-level directory of this distribution
 and at https://github.com/grafov/m3u8/

 ॐ तारे तुत्तारे तुरे स्व
*/

import (
	"strings"
	"testing"

	"github.com/grafov/m3u8/task_support/taskcheck"
)

const renditionCatalogInput = `#EXTM3U
#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="shared",NAME="main",DEFAULT=YES,LANGUAGE="en",URI="media/rendition.m3u8"
#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID="shared",NAME="main",DEFAULT=NO,LANGUAGE="en",URI="media/rendition.m3u8"
#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="shared",NAME="alt",DEFAULT=NO,URI="audio/alt.m3u8"
#EXT-X-MEDIA:TYPE=CLOSED-CAPTIONS,GROUP-ID="cc",NAME="cc1",DEFAULT=NO,INSTREAM-ID=CC1
#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="embedded",NAME="embedded",DEFAULT=NO
#EXT-X-STREAM-INF:BANDWIDTH=100,AUDIO="shared",SUBTITLES="shared",CLOSED-CAPTIONS="cc"
low.m3u8
#EXT-X-STREAM-INF:BANDWIDTH=200,AUDIO="shared",SUBTITLES="shared",CLOSED-CAPTIONS=NONE
high.m3u8
#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="spare",NAME="spare",URI="spare.m3u8"
#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="shared",NAME="main",DEFAULT=YES,LANGUAGE="en",URI="media/rendition.m3u8"
`

const renditionRenameInput = `#EXTM3U
#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="a",NAME="main",DEFAULT=YES,URI="a.m3u8"
#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="b",NAME="alt",DEFAULT=NO,URI="b.m3u8"
#EXT-X-MEDIA:TYPE=VIDEO,GROUP-ID="a",NAME="video",URI="v.m3u8"
#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID="sub",NAME="sub",DEFAULT=NO,URI="sub.m3u8"
#EXT-X-MEDIA:TYPE=CLOSED-CAPTIONS,GROUP-ID="cc",NAME="cc1",INSTREAM-ID=CC1
#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="spare",NAME="spare",URI="spare.m3u8"
#EXT-X-STREAM-INF:BANDWIDTH=100,AUDIO="a",VIDEO="a",SUBTITLES="sub",CLOSED-CAPTIONS="cc"
low.m3u8
#EXT-X-STREAM-INF:BANDWIDTH=200,AUDIO="b",VIDEO="a",SUBTITLES="sub",CLOSED-CAPTIONS=NONE
high.m3u8
`

// decodeMasterCatalog decodes input through both master parsing entry points
// and returns the catalog from each: the direct MasterPlaylist.DecodeFrom and
// the automatic type-detecting package-level DecodeFrom.
func decodeMasterCatalog(t *testing.T, input string) (*MasterPlaylist, *MasterPlaylist) {
	t.Helper()
	direct := NewMasterPlaylist()
	if err := direct.DecodeFrom(strings.NewReader(input), true); err != nil {
		t.Fatalf("direct master DecodeFrom failed: %v", err)
	}
	detected, listType, err := DecodeFrom(strings.NewReader(input), true)
	if err != nil {
		t.Fatalf("auto DecodeFrom failed: %v", err)
	}
	if listType != MASTER {
		t.Fatalf("auto detection returned list type %v, want MASTER", listType)
	}
	detectedMaster, ok := detected.(*MasterPlaylist)
	if !ok {
		t.Fatalf("auto detection returned %T, want *MasterPlaylist", detected)
	}
	return direct, detectedMaster
}

// alternativeFingerprint renders every field of an Alternative so two catalog
// entries can be compared field by field.
func alternativeFingerprint(alt *Alternative) string {
	def := "no"
	if alt.Default {
		def = "yes"
	}
	return strings.Join([]string{
		alt.Type, alt.GroupId, alt.Name, alt.URI, alt.Language,
		alt.Autoselect, alt.Forced, alt.Characteristics, alt.Subtitles,
		alt.InstreamId, def,
	}, "|")
}

func catalogFingerprints(catalog []*Alternative) []string {
	out := make([]string, 0, len(catalog))
	for _, alt := range catalog {
		out = append(out, alternativeFingerprint(alt))
	}
	return out
}

func sameFingerprints(a, b []string) bool {
	return strings.Join(a, "\n") == strings.Join(b, "\n")
}

func renditionIdentity(alt *Alternative) string {
	return alt.Type + "/" + alt.GroupId + "/" + alt.Name
}

func countRenditionsByIdentity(alts []*Alternative, identity string) int {
	n := 0
	for _, alt := range alts {
		if alt != nil && renditionIdentity(alt) == identity {
			n++
		}
	}
	return n
}

// renditionLine returns the encoded EXT-X-MEDIA line whose GROUP-ID and NAME
// match, or "" when it is absent.
func renditionLine(out, group, name string) string {
	needle := `GROUP-ID="` + group + `",NAME="` + name + `"`
	for _, line := range strings.Split(out, "\n") {
		if strings.HasPrefix(line, "#EXT-X-MEDIA:") && strings.Contains(line, needle) {
			return line
		}
	}
	return ""
}

func TestRenditionCatalog(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		direct, auto := decodeMasterCatalog(t, renditionCatalogInput)

		// The catalog is the union of every declaration, in first-seen order,
		// including groups no variant references.
		wantCatalog := []*Alternative{
			{Type: "AUDIO", GroupId: "shared", Name: "main", Default: true, Language: "en", URI: "media/rendition.m3u8"},
			{Type: "SUBTITLES", GroupId: "shared", Name: "main", Language: "en", URI: "media/rendition.m3u8"},
			{Type: "AUDIO", GroupId: "shared", Name: "alt", URI: "audio/alt.m3u8"},
			{Type: "CLOSED-CAPTIONS", GroupId: "cc", Name: "cc1", InstreamId: "CC1"},
			{Type: "AUDIO", GroupId: "embedded", Name: "embedded"},
			{Type: "AUDIO", GroupId: "spare", Name: "spare", URI: "spare.m3u8"},
		}
		c.Check(sameFingerprints(catalogFingerprints(direct.Renditions), catalogFingerprints(wantCatalog)),
			"catalog mismatch: got %v", catalogFingerprints(direct.Renditions))

		// Both parsing entry points produce the same catalog.
		c.Check(sameFingerprints(catalogFingerprints(direct.Renditions), catalogFingerprints(auto.Renditions)),
			"DecodeFrom and auto Decode disagree: %v vs %v",
			catalogFingerprints(direct.Renditions), catalogFingerprints(auto.Renditions))

		// Exact duplicate declarations collapse into a single entry.
		c.Check(countRenditionsByIdentity(direct.Renditions, "AUDIO/shared/main") == 1,
			"duplicate EXT-X-MEDIA declaration was not merged")
		c.Check(len(direct.Renditions) == len(wantCatalog), "catalog has %d entries, want %d", len(direct.Renditions), len(wantCatalog))

		// Identity is (TYPE, GROUP-ID, NAME), not the URI: the AUDIO and
		// SUBTITLES renditions sharing a URI stay distinct.
		c.Check(countRenditionsByIdentity(direct.Renditions, "AUDIO/shared/main") == 1 &&
			countRenditionsByIdentity(direct.Renditions, "SUBTITLES/shared/main") == 1,
			"entries sharing a URI but differing in TYPE were folded together")

		// Unreferenced groups survive in the catalog and on encode.
		spare := false
		embedded := ""
		for _, alt := range direct.Renditions {
			if alt.GroupId == "spare" {
				spare = true
			}
			if alt.GroupId == "embedded" {
				embedded = alternativeFingerprint(alt)
			}
		}
		c.Check(spare, "unreferenced group %q was dropped from the catalog", "spare")
		c.Check(embedded != "" && !strings.Contains(embedded, "m3u8"), "embedded rendition lost its attributes: %q", embedded)
		c.Check(embedded != "" && strings.Contains(embedded, "CC1") == false, "unexpected instream id")

		// Legacy Variant.Alternatives is populated only for TYPE+GROUP-ID
		// matches and is never duplicated.
		c.Check(len(direct.Variants) == 2, "expected 2 variants, got %d", len(direct.Variants))
		c.Check(len(direct.Variants[0].Alternatives) == 4, "variant 0 has %d alternatives, want 4", len(direct.Variants[0].Alternatives))
		c.Check(len(direct.Variants[1].Alternatives) == 3, "variant 1 has %d alternatives, want 3", len(direct.Variants[1].Alternatives))
		c.Check(countRenditionsByIdentity(direct.Variants[0].Alternatives, "AUDIO/shared/main") == 1,
			"shared audio rendition linked more than once")
		c.Check(countRenditionsByIdentity(direct.Variants[0].Alternatives, "CLOSED-CAPTIONS/cc/cc1") == 1,
			"caption rendition not linked")
		c.Check(countRenditionsByIdentity(direct.Variants[1].Alternatives, "CLOSED-CAPTIONS/cc/cc1") == 0,
			"CLOSED-CAPTIONS=NONE still linked a caption rendition")

		// Encoding: catalog order first, every identity once, INSTREAM-ID
		// unquoted, embedded (URI-less) rendition keeps its attributes.
		out := direct.Encode().String()
		c.Check(strings.Count(out, "#EXT-X-MEDIA:") == len(wantCatalog),
			"encoded %d EXT-X-MEDIA lines, want %d\n%s", strings.Count(out, "#EXT-X-MEDIA:"), len(wantCatalog), out)

		idxMain := strings.Index(out, `TYPE=AUDIO,GROUP-ID="shared",NAME="main"`)
		idxSub := strings.Index(out, `TYPE=SUBTITLES,GROUP-ID="shared",NAME="main"`)
		idxCC := strings.Index(out, `TYPE=CLOSED-CAPTIONS,GROUP-ID="cc",NAME="cc1"`)
		idxEmbedded := strings.Index(out, `TYPE=AUDIO,GROUP-ID="embedded",NAME="embedded"`)
		idxSpare := strings.Index(out, `TYPE=AUDIO,GROUP-ID="spare",NAME="spare"`)
		c.Check(idxMain >= 0 && idxSub > idxMain && idxCC > idxSub && idxEmbedded > idxCC && idxSpare > idxEmbedded,
			"catalog order not preserved on encode: main=%d sub=%d cc=%d embedded=%d spare=%d\n%s",
			idxMain, idxSub, idxCC, idxEmbedded, idxSpare, out)
		c.Check(strings.Contains(out, ",INSTREAM-ID=CC1") && !strings.Contains(out, `INSTREAM-ID="CC1"`),
			"INSTREAM-ID must be written unquoted\n%s", out)
		embeddedLine := renditionLine(out, "embedded", "embedded")
		c.Check(embeddedLine != "" && !strings.Contains(embeddedLine, "URI="),
			"URI-less rendition lost or gained a URI: %q", embeddedLine)
		c.Check(strings.Count(out, `GROUP-ID="shared",NAME="alt"`) == 1,
			"rendition referenced by two variants was duplicated")
		c.Check(strings.Contains(out, "spare.m3u8"), "unreferenced rendition missing from output")

		// Round-trip through the auto-detecting decoder yields the same catalog.
		roundTrip, listType, err := DecodeFrom(strings.NewReader(out), true)
		c.Check(err == nil && listType == MASTER, "round-trip decode failed: type=%v err=%v", listType, err)
		if err == nil {
			rt := roundTrip.(*MasterPlaylist)
			c.Check(sameFingerprints(catalogFingerprints(rt.Renditions), catalogFingerprints(wantCatalog)),
				"round-trip catalog changed: %v", catalogFingerprints(rt.Renditions))
			c.Check(len(rt.Variants) == 2 && len(rt.Variants[0].Alternatives) == 4,
				"round-trip lost variant associations")
		}

		// Strict parsing rejects conflicting duplicate declarations.
		conflict := "#EXTM3U\n" +
			`#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="dup",NAME="main",URI="one.m3u8"` + "\n" +
			`#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="dup",NAME="main",URI="two.m3u8"` + "\n"
		strictPlaylist := NewMasterPlaylist()
		err = strictPlaylist.DecodeFrom(strings.NewReader(conflict), true)
		c.Check(err != nil && strings.Contains(err.Error(), "dup"), "strict conflict not reported: %v", err)

		_, _, autoErr := DecodeFrom(strings.NewReader(conflict), true)
		c.Check(autoErr != nil, "auto decoder did not report strict conflict")

		// Non-strict parsing keeps the first declaration.
		laxPlaylist := NewMasterPlaylist()
		laxErr := laxPlaylist.DecodeFrom(strings.NewReader(conflict), false)
		c.Check(laxErr == nil && len(laxPlaylist.Renditions) == 1 && laxPlaylist.Renditions[0].URI == "one.m3u8",
			"non-strict conflict handling: err=%v catalog=%v", laxErr, catalogFingerprints(laxPlaylist.Renditions))

		// A catalog is emitted even without any variant.
		empty := NewMasterPlaylist()
		empty.Renditions = []*Alternative{{Type: "AUDIO", GroupId: "only", Name: "solo", URI: "solo.m3u8"}}
		emptyOut := empty.Encode().String()
		c.Check(strings.Contains(emptyOut, `GROUP-ID="only"`), "catalog not emitted without variants\n%s", emptyOut)

		// Decoding into an existing playlist must neither duplicate the
		// catalog nor re-link the same rendition twice.
		reused := NewMasterPlaylist()
		if err := reused.DecodeFrom(strings.NewReader(renditionCatalogInput), true); err != nil {
			c.Check(false, "first decode into reused playlist failed: %v", err)
		}
		if err := reused.DecodeFrom(strings.NewReader(renditionCatalogInput), true); err != nil {
			c.Check(false, "second decode into reused playlist failed: %v", err)
		}
		c.Check(len(reused.Renditions) == len(wantCatalog),
			"second decode duplicated catalog entries: %d, want %d", len(reused.Renditions), len(wantCatalog))
	})
}

func newRenamePlaylist(t *testing.T) *MasterPlaylist {
	t.Helper()
	p := NewMasterPlaylist()
	if err := p.DecodeFrom(strings.NewReader(renditionRenameInput), true); err != nil {
		t.Fatalf("failed to decode rename fixture: %v", err)
	}
	return p
}

func findCatalog(p *MasterPlaylist, typ, group, name string) *Alternative {
	for _, alt := range p.Renditions {
		if alt.Type == typ && alt.GroupId == group && alt.Name == name {
			return alt
		}
	}
	return nil
}

func findVariantAlternative(v *Variant, typ, group, name string) *Alternative {
	if v == nil {
		return nil
	}
	for _, alt := range v.Alternatives {
		if alt.Type == typ && alt.GroupId == group && alt.Name == name {
			return alt
		}
	}
	return nil
}

func TestRenditionRename(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// a→b and b→a simultaneously, on AUDIO only. The VIDEO group named
		// "a" is a different type and must stay untouched; "spare" is never
		// referenced but is still renamed.
		p := newRenamePlaylist(t)
		err := p.RenameRenditionGroups(map[RenditionGroupKey]string{
			{Type: "AUDIO", GroupID: "a"}:     "b",
			{Type: "AUDIO", GroupID: "b"}:     "a",
			{Type: "AUDIO", GroupID: "spare"}: "unused",
		})
		c.Check(err == nil, "rename failed: %v", err)

		if main := findCatalog(p, "AUDIO", "b", "main"); main == nil {
			c.Check(false, "AUDIO/main was not renamed to group b")
		}
		if alt := findCatalog(p, "AUDIO", "a", "alt"); alt == nil {
			c.Check(false, "AUDIO/alt was not renamed to group a")
		}
		c.Check(findCatalog(p, "AUDIO", "unused", "spare") != nil, "unreferenced group was not renamed")
		c.Check(findCatalog(p, "VIDEO", "a", "video") != nil, "same-named VIDEO group was renamed")
		c.Check(findCatalog(p, "SUBTITLES", "sub", "sub") != nil, "unmapped group was modified")
		c.Check(findCatalog(p, "CLOSED-CAPTIONS", "cc", "cc1") != nil, "unmapped caption group was modified")
		c.Check(len(p.Variants) == 2, "variant count changed")
		c.Check(p.Variants[0].Audio == "b", "variant 0 AUDIO = %q, want b", p.Variants[0].Audio)
		c.Check(p.Variants[1].Audio == "a", "variant 1 AUDIO = %q, want a", p.Variants[1].Audio)
		c.Check(p.Variants[0].Video == "a" && p.Variants[1].Video == "a", "VIDEO references were changed")
		c.Check(p.Variants[0].Subtitles == "sub", "SUBTITLES reference was changed")
		c.Check(p.Variants[0].Captions == "cc", "CLOSED-CAPTIONS reference was changed")
		c.Check(p.Variants[1].Captions == "NONE", "CLOSED-CAPTIONS=NONE was not preserved: %q", p.Variants[1].Captions)

		// Legacy alternatives follow the same names.
		if alt := findVariantAlternative(p.Variants[0], "AUDIO", "b", "main"); alt == nil {
			c.Check(false, "legacy alternative for renamed audio group is stale")
		}
		if alt := findVariantAlternative(p.Variants[1], "AUDIO", "a", "alt"); alt == nil {
			c.Check(false, "legacy alternative for swapped audio group is stale")
		}
		if alt := findVariantAlternative(p.Variants[0], "VIDEO", "a", "video"); alt == nil {
			c.Check(false, "legacy VIDEO alternative was renamed across types")
		}

		out := p.Encode().String()
		c.Check(strings.Count(out, "#EXT-X-MEDIA:") == 6, "unexpected rendition count after rename:\n%s", out)
		c.Check(strings.Contains(out, `GROUP-ID="unused"`), "renamed unreferenced group missing from output")
		c.Check(strings.Contains(out, `AUDIO="b"`) && strings.Contains(out, `AUDIO="a"`), "variant audio references not encoded with new names")

		// Encoding before renaming must still yield the new names afterwards.
		q := newRenamePlaylist(t)
		c.Check(strings.Contains(q.Encode().String(), `GROUP-ID="a"`), "pre-rename output missing original group")
		if err := q.RenameRenditionGroups(map[RenditionGroupKey]string{{Type: "AUDIO", GroupID: "a"}: "z"}); err != nil {
			c.Check(false, "rename after encode failed: %v", err)
		}
		c.Check(strings.Contains(q.Encode().String(), `GROUP-ID="z"`), "cached output was not refreshed after rename")

		// Two successive renames on the same object must not duplicate entries.
		if err := q.RenameRenditionGroups(map[RenditionGroupKey]string{{Type: "AUDIO", GroupID: "z"}: "x"}); err != nil {
			c.Check(false, "second rename failed: %v", err)
		}
		qOut := q.Encode().String()
		c.Check(strings.Count(qOut, "#EXT-X-MEDIA:") == 6, "entry duplicated after successive renames:\n%s", qOut)
		reparsed := NewMasterPlaylist()
		if err := reparsed.DecodeFrom(strings.NewReader(qOut), true); err != nil {
			c.Check(false, "reparse after renames failed: %v", err)
		}
		c.Check(len(reparsed.Renditions) == 6 && countRenditionsByIdentity(reparsed.Renditions, "AUDIO/x/main") == 1,
			"write-read cycle produced duplicate entries: %v", catalogFingerprints(reparsed.Renditions))

		// A CLOSED-CAPTIONS group can be renamed to a normal name, while
		// CLOSED-CAPTIONS=NONE keeps meaning "no captions".
		captions := newRenamePlaylist(t)
		if err := captions.RenameRenditionGroups(map[RenditionGroupKey]string{
			{Type: "CLOSED-CAPTIONS", GroupID: "cc"}: "captions",
		}); err != nil {
			c.Check(false, "caption group rename failed: %v", err)
		}
		c.Check(findCatalog(captions, "CLOSED-CAPTIONS", "captions", "cc1") != nil, "caption group not renamed")
		c.Check(captions.Variants[0].Captions == "captions", "caption reference = %q, want captions", captions.Variants[0].Captions)
		c.Check(captions.Variants[1].Captions == "NONE", "CLOSED-CAPTIONS=NONE became %q", captions.Variants[1].Captions)
		captionsOut := captions.Encode().String()
		c.Check(strings.Contains(captionsOut, `CLOSED-CAPTIONS="captions"`), "renamed caption reference not encoded")
		c.Check(strings.Contains(captionsOut, "CLOSED-CAPTIONS=NONE"), "CLOSED-CAPTIONS=NONE not preserved after caption rename")

		// Renaming a subtitles group also updates EXT-X-MEDIA SUBTITLES
		// cross-references, not just variant SUBTITLES attributes.
		subs := newRenamePlaylist(t)
		if err := subs.RenameRenditionGroups(map[RenditionGroupKey]string{
			{Type: "SUBTITLES", GroupID: "sub"}: "subs2",
		}); err != nil {
			c.Check(false, "subtitle group rename failed: %v", err)
		}
		subsOut := subs.Encode().String()
		c.Check(strings.Contains(subsOut, `SUBTITLES="subs2"`), "variant subtitle reference not renamed\n%s", subsOut)
		c.Check(!strings.Contains(subsOut, `SUBTITLES="sub"`), "stale subtitle reference remained\n%s", subsOut)

		// An empty mapping succeeds and changes nothing.
		before := newRenamePlaylist(t)
		beforeOut := before.Encode().String()
		c.Check(before.RenameRenditionGroups(map[RenditionGroupKey]string{}) == nil, "empty mapping returned an error")
		c.Check(before.Encode().String() == beforeOut, "empty mapping modified the playlist")

		// Invalid mappings fail atomically. The error names the offending key.
		type badRename struct {
			name  string
			input map[RenditionGroupKey]string
			parts []string
		}
		cases := []badRename{
			{"missing source", map[RenditionGroupKey]string{{Type: "AUDIO", GroupID: "nope"}: "x"}, []string{"AUDIO", "nope"}},
			{"empty target", map[RenditionGroupKey]string{{Type: "AUDIO", GroupID: "a"}: ""}, []string{"AUDIO", "a"}},
			{"blank target", map[RenditionGroupKey]string{{Type: "AUDIO", GroupID: "a"}: "   "}, []string{"AUDIO", "a"}},
			{"unknown type", map[RenditionGroupKey]string{{Type: "DATA", GroupID: "a"}: "x"}, []string{"DATA"}},
			{"collision", map[RenditionGroupKey]string{
				{Type: "AUDIO", GroupID: "a"}: "b",
				{Type: "AUDIO", GroupID: "b"}: "b",
			}, []string{"AUDIO"}},
			{"collision with unmapped", map[RenditionGroupKey]string{{Type: "AUDIO", GroupID: "a"}: "b"}, []string{"AUDIO", "b"}},
			{"captions to NONE", map[RenditionGroupKey]string{{Type: "CLOSED-CAPTIONS", GroupID: "cc"}: "NONE"}, []string{"CLOSED-CAPTIONS", "cc"}},
		}
		for _, tc := range cases {
			subject := newRenamePlaylist(t)
			snapshotOut := subject.Encode().String()
			snapshotCatalog := catalogFingerprints(subject.Renditions)
			renameErr := subject.RenameRenditionGroups(tc.input)
			c.Check(renameErr != nil, "%s: expected an error", tc.name)
			if renameErr != nil {
				for _, part := range tc.parts {
					c.Check(strings.Contains(renameErr.Error(), part),
						"%s: error %q does not mention %q", tc.name, renameErr.Error(), part)
				}
			}
			c.Check(subject.Encode().String() == snapshotOut, "%s: generated output changed after failure", tc.name)
			c.Check(sameFingerprints(catalogFingerprints(subject.Renditions), snapshotCatalog), "%s: catalog changed after failure", tc.name)
			c.Check(subject.Variants[0].Audio == "a" && subject.Variants[1].Audio == "b", "%s: variant references changed after failure", tc.name)
		}
	})
}

func TestRenditionLegacy(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Existing sample still parses with the legacy per-variant links.
		sample := NewMasterPlaylist()
		err := sample.DecodeFrom(strings.NewReader(`#EXTM3U
#EXT-X-MEDIA:TYPE=VIDEO,GROUP-ID="low",NAME="Main",DEFAULT=YES,URI="low/main.m3u8"
#EXT-X-MEDIA:TYPE=VIDEO,GROUP-ID="low",NAME="Centerfield",DEFAULT=NO,URI="low/centerfield.m3u8"
#EXT-X-MEDIA:TYPE=VIDEO,GROUP-ID="low",NAME="Dugout",DEFAULT=NO,URI="low/dugout.m3u8"
#EXT-X-STREAM-INF:BANDWIDTH=1280000,VIDEO="low"
low.m3u8
#EXT-X-STREAM-INF:BANDWIDTH=65000
audio.m3u8
`), true)
		c.Check(err == nil, "sample decode failed: %v", err)
		c.Check(len(sample.Variants) == 2, "sample variant count = %d", len(sample.Variants))
		if len(sample.Variants) == 2 {
			c.Check(len(sample.Variants[0].Alternatives) == 3, "legacy alternatives = %d, want 3", len(sample.Variants[0].Alternatives))
			c.Check(len(sample.Variants[1].Alternatives) == 0, "variant without references gained alternatives")
		}
		c.Check(len(sample.Renditions) == 3, "catalog did not capture declarations: %d", len(sample.Renditions))

		// Catalog entries take priority over legacy-only entries with the same
		// identity, and every identity is emitted once. Entries sharing a URI
		// but differing in type/group/name are kept apart.
		m := NewMasterPlaylist()
		catAudio := &Alternative{Type: "AUDIO", GroupId: "shared", Name: "main", URI: "shared.m3u8", Language: "en"}
		m.Renditions = []*Alternative{catAudio}
		legacySameIdentity := &Alternative{Type: "AUDIO", GroupId: "shared", Name: "main", URI: "shared.m3u8", Language: "fr"}
		legacyOtherName := &Alternative{Type: "AUDIO", GroupId: "shared", Name: "other", URI: "shared.m3u8"}
		legacyOtherType := &Alternative{Type: "VIDEO", GroupId: "shared", Name: "main", URI: "shared.m3u8"}
		media, e := NewMediaPlaylist(1, 1)
		c.Check(e == nil, "media playlist creation failed: %v", e)
		m.Append("v1.m3u8", media, VariantParams{Audio: "shared", Alternatives: []*Alternative{
			catAudio, legacySameIdentity, legacyOtherName, legacyOtherType,
		}})
		m.Append("v2.m3u8", media, VariantParams{Audio: "shared", Alternatives: []*Alternative{catAudio}})

		out := m.Encode().String()
		c.Check(strings.Count(out, "#EXT-X-MEDIA:") == 3,
			"legacy+catalog merge emitted %d lines, want 3:\n%s", strings.Count(out, "#EXT-X-MEDIA:"), out)
		mainLine := renditionLine(out, "shared", "main")
		c.Check(strings.Contains(mainLine, "TYPE=AUDIO") && strings.Contains(mainLine, `LANGUAGE="en"`),
			"catalog entry did not win over legacy duplicate: %q", mainLine)
		c.Check(!strings.Contains(out, `LANGUAGE="fr"`), "legacy duplicate was still encoded")
		idxMain := strings.Index(out, `TYPE=AUDIO,GROUP-ID="shared",NAME="main"`)
		idxOther := strings.Index(out, `TYPE=AUDIO,GROUP-ID="shared",NAME="other"`)
		idxVideo := strings.Index(out, `TYPE=VIDEO,GROUP-ID="shared",NAME="main"`)
		c.Check(idxMain >= 0 && idxOther > idxMain && idxVideo > idxOther,
			"merge order should be catalog then first-seen legacy: main=%d other=%d video=%d", idxMain, idxOther, idxVideo)
		c.Check(strings.Count(out, `GROUP-ID="shared",NAME="other"`) == 1, "same-URI legacy rendition duplicated")
		c.Check(strings.Count(out, `TYPE=VIDEO,GROUP-ID="shared"`) == 1, "same-URI different-type rendition folded")

		// Append with alternatives keeps the historical version bump.
		c.Check(m.Version() == 4, "version after Append with alternatives = %d, want 4", m.Version())

		// Repeated encoding is stable.
		c.Check(m.Encode().String() == out, "repeated Encode changed the output")

		// I-frame variants, custom tags and CLOSED-CAPTIONS=NONE still work.
		iframe := NewMasterPlaylist()
		iframe.Renditions = []*Alternative{{Type: "VIDEO", GroupId: "vid", Name: "main", URI: "vid.m3u8"}}
		iframe.Append("iframe.m3u8", media, VariantParams{Iframe: true, Bandwidth: 90000, Video: "vid",
			Alternatives: []*Alternative{{Type: "VIDEO", GroupId: "vid", Name: "main", URI: "vid.m3u8"}}})
		iframeOut := iframe.Encode().String()
		c.Check(strings.Contains(iframeOut, "#EXT-X-I-FRAME-STREAM-INF:"), "I-frame variant missing\n%s", iframeOut)
		c.Check(strings.Count(iframeOut, "#EXT-X-MEDIA:") == 1, "I-frame variant rendered extra renditions\n%s", iframeOut)

		custom := NewMasterPlaylist()
		custom.SetCustomTag(&MockCustomTag{name: "#CustomMTag", encodedString: "#CustomMTag"})
		custom.Renditions = []*Alternative{{Type: "AUDIO", GroupId: "aud", Name: "main", URI: "aud.m3u8"}}
		customOut := custom.Encode().String()
		c.Check(strings.Contains(customOut, "#CustomMTag") && strings.Contains(customOut, `GROUP-ID="aud"`),
			"custom tag or rendition missing\n%s", customOut)

		captions := NewMasterPlaylist()
		captions.Renditions = []*Alternative{{Type: "CLOSED-CAPTIONS", GroupId: "cc", Name: "cc1", InstreamId: "CC1"}}
		captions.Append("main.m3u8", media, VariantParams{Captions: "NONE"})
		captionsOut := captions.Encode().String()
		c.Check(strings.Contains(captionsOut, "CLOSED-CAPTIONS=NONE") && !strings.Contains(captionsOut, `CLOSED-CAPTIONS="NONE"`),
			"CLOSED-CAPTIONS=NONE broken\n%s", captionsOut)
		c.Check(strings.Contains(captionsOut, `GROUP-ID="cc"`), "caption rendition declaration dropped")

		// Old-style Append without alternatives stays at the base version.
		plain := NewMasterPlaylist()
		plain.Append("plain.m3u8", media, VariantParams{Bandwidth: 100})
		c.Check(plain.Version() == minver, "plain Append version = %d, want %d", plain.Version(), minver)
	})
}
