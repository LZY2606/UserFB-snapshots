package m3u8

/*
 Part of M3U8 parser & generator library.
 This file implements the rendition (EXT-X-MEDIA) catalog: identity,
 duplicate handling and group renaming.

 Copyright 2013-2019 The Project Developers.
 See the AUTHORS and LICENSE files at the top-level directory of this distribution
 and at https://github.com/grafov/m3u8/

 ॐ तारे तुत्तारे तुरे स्व
*/

import (
	"fmt"
	"sort"
)

// renditionIdentity is the identity of a rendition entry. A rendition is
// identified by the TYPE, GROUP-ID and NAME triple, never by its URI or
// by a concatenation of string fields.
type renditionIdentity struct {
	Type    string
	GroupID string
	Name    string
}

func identityOf(alt *Alternative) renditionIdentity {
	return renditionIdentity{Type: alt.Type, GroupID: alt.GroupId, Name: alt.Name}
}

// renditionIndex maps the rendition identity of every entry currently in the
// catalog to its slice position.
func (p *MasterPlaylist) renditionIndex() map[renditionIdentity]int {
	index := make(map[renditionIdentity]int, len(p.Renditions))
	for i, alt := range p.Renditions {
		if alt == nil {
			continue
		}
		if _, ok := index[identityOf(alt)]; !ok {
			index[identityOf(alt)] = i
		}
	}
	return index
}

// addRendition records a parsed EXT-X-MEDIA declaration in the catalog.
// Declarations with identical identity and identical content collapse into a
// single entry; conflicting content is an error in strict mode.
func (p *MasterPlaylist) addRendition(state *decodingState, alt *Alternative, strict bool) error {
	if state.renditions == nil {
		state.renditions = p.renditionIndex()
	}
	id := identityOf(alt)
	if i, ok := state.renditions[id]; ok {
		existing := p.Renditions[i]
		if existing == nil || *existing == *alt {
			return nil
		}
		if strict {
			return fmt.Errorf("EXT-X-MEDIA: conflicting redeclaration for TYPE=%s GROUP-ID=%q NAME=%q", id.Type, id.GroupID, id.Name)
		}
		return nil
	}
	p.Renditions = append(p.Renditions, alt)
	state.renditions[id] = len(p.Renditions) - 1
	return nil
}

// renditionGroups returns every rendition group known to the playlist from
// the catalog and from alternatives attached directly to variants.
func (p *MasterPlaylist) renditionGroups() map[RenditionGroupKey]bool {
	groups := make(map[RenditionGroupKey]bool)
	remember := func(alt *Alternative) {
		if alt == nil || alt.Type == "" || alt.GroupId == "" {
			return
		}
		groups[RenditionGroupKey{Type: alt.Type, GroupID: alt.GroupId}] = true
	}
	for _, alt := range p.Renditions {
		remember(alt)
	}
	for _, v := range p.Variants {
		if v == nil {
			continue
		}
		for _, alt := range v.Alternatives {
			remember(alt)
		}
	}
	return groups
}

// RenditionGroupKey identifies a rendition group by its TYPE and GROUP-ID.
type RenditionGroupKey struct {
	Type    string
	GroupID string
}

func (k RenditionGroupKey) String() string {
	return fmt.Sprintf("{TYPE=%s GROUP-ID=%q}", k.Type, k.GroupID)
}

func knownRenditionType(typ string) bool {
	switch typ {
	case "AUDIO", "VIDEO", "SUBTITLES", "CLOSED-CAPTIONS":
		return true
	}
	return false
}

// RenameRenditionGroups renames rendition groups in the catalog, in every
// matching AUDIO/VIDEO/SUBTITLES/CLOSED-CAPTIONS reference of the variants and
// in legacy Variant.Alternatives entries.
//
// All renames in renames apply simultaneously to the group names as they are
// when the method is called, so swapping two names (a->b, b->a) works. Groups
// absent from the map are left untouched; groups not referenced by any variant
// can be renamed as well.
//
// The whole playlist is left unchanged when an error is returned.
func (p *MasterPlaylist) RenameRenditionGroups(renames map[RenditionGroupKey]string) error {
	if len(renames) == 0 {
		return nil
	}

	groups := p.renditionGroups()

	keys := make([]RenditionGroupKey, 0, len(renames))
	for key := range renames {
		keys = append(keys, key)
	}
	sort.Slice(keys, func(i, j int) bool {
		if keys[i].Type != keys[j].Type {
			return keys[i].Type < keys[j].Type
		}
		return keys[i].GroupID < keys[j].GroupID
	})

	for _, key := range keys {
		target := renames[key]
		if !knownRenditionType(key.Type) {
			return fmt.Errorf("rename rendition group %s: unknown rendition type", key)
		}
		if !groups[key] {
			return fmt.Errorf("rename rendition group %s: source group not found", key)
		}
		if target == "" {
			return fmt.Errorf("rename rendition group %s: target name is empty", key)
		}
		if key.Type == "CLOSED-CAPTIONS" && target == "NONE" {
			return fmt.Errorf("rename rendition group %s: target name %q is reserved for no closed captions", key, target)
		}
	}

	// Validate that no two groups of the same type end up with one name.
	groupKeys := make([]RenditionGroupKey, 0, len(groups))
	for group := range groups {
		groupKeys = append(groupKeys, group)
	}
	sort.Slice(groupKeys, func(i, j int) bool {
		if groupKeys[i].Type != groupKeys[j].Type {
			return groupKeys[i].Type < groupKeys[j].Type
		}
		return groupKeys[i].GroupID < groupKeys[j].GroupID
	})
	finalOwners := make(map[string]map[string]RenditionGroupKey)
	for _, group := range groupKeys {
		final := group.GroupID
		if target, ok := renames[group]; ok {
			final = target
		}
		byName := finalOwners[group.Type]
		if byName == nil {
			byName = make(map[string]RenditionGroupKey)
			finalOwners[group.Type] = byName
		}
		if owner, ok := byName[final]; ok && owner != group {
			offender := group
			if _, renamed := renames[group]; !renamed {
				offender = owner
			}
			return fmt.Errorf("rename rendition group %s: final name %q collides with group %s", offender, final, owner)
		}
		byName[final] = group
	}

	finalName := func(typ, group string) string {
		if target, ok := renames[RenditionGroupKey{Type: typ, GroupID: group}]; ok {
			return target
		}
		return group
	}

	// Renditions may be shared between the catalog and legacy
	// Variant.Alternatives, so each pointer must be visited exactly once.
	seen := make(map[*Alternative]bool)
	renameAlt := func(alt *Alternative) {
		if alt == nil || seen[alt] {
			return
		}
		seen[alt] = true
		alt.GroupId = finalName(alt.Type, alt.GroupId)
		// SUBTITLES on a rendition names a subtitles group.
		if alt.Subtitles != "" {
			alt.Subtitles = finalName("SUBTITLES", alt.Subtitles)
		}
	}
	for _, alt := range p.Renditions {
		renameAlt(alt)
	}
	for _, v := range p.Variants {
		if v == nil {
			continue
		}
		for _, alt := range v.Alternatives {
			renameAlt(alt)
		}
		if v.Audio != "" {
			v.Audio = finalName("AUDIO", v.Audio)
		}
		if v.Video != "" {
			v.Video = finalName("VIDEO", v.Video)
		}
		if v.Subtitles != "" {
			v.Subtitles = finalName("SUBTITLES", v.Subtitles)
		}
		if v.Captions != "" && v.Captions != "NONE" {
			v.Captions = finalName("CLOSED-CAPTIONS", v.Captions)
		}
	}

	p.buf.Reset()
	return nil
}

// renditionsForEncoding merges the catalog with alternatives built directly
// through Variant.Alternatives. Catalog entries come first in catalog order;
// the remaining entries follow in first-seen variant order. Every identity is
// emitted once.
func (p *MasterPlaylist) renditionsForEncoding() []*Alternative {
	seen := make(map[renditionIdentity]bool)
	out := make([]*Alternative, 0, len(p.Renditions))
	add := func(alt *Alternative) {
		if alt == nil {
			return
		}
		id := identityOf(alt)
		if seen[id] {
			return
		}
		seen[id] = true
		out = append(out, alt)
	}
	for _, alt := range p.Renditions {
		add(alt)
	}
	for _, v := range p.Variants {
		if v == nil {
			continue
		}
		for _, alt := range v.Alternatives {
			add(alt)
		}
	}
	return out
}
