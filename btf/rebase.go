package btf

import (
	"fmt"
	"maps"
)

// Rebase swaps the base BTF of a split Spec.
//
// The receiver must be a split Spec. newBase must be a complete (non-split)
// base Spec which uses the same byte order. baseIDs maps every non-zero type
// ID of the currently attached base to a valid non-zero ID in newBase. Several
// old IDs may map to the same new ID; ID zero is always void and must not be
// remapped. Type equivalence is never inferred from names: the caller supplies
// the complete correspondence.
//
// Local types keep their relative order and are numbered consecutively after
// the last ID of newBase. References encoded in the split BTF are rewritten
// through the returned table. Type objects already obtained from the Spec stay
// the same objects, but edges which used to point into the old base are changed
// to reference objects of newBase. Types inflated lazily after Rebase follow
// the same rules.
//
// The split's raw type and string data is untouched: name offsets stay relative
// to the original split string section, so local and member names do not change
// when newBase carries a differently sized string table.
//
// The returned map covers every old record ID of the module, including
// declaration tags hidden by [Spec.All]. Neither the input bytes nor the two
// base Specs are modified. On a repeated Rebase, baseIDs is interpreted
// relative to the currently attached base.
func (s *Spec) Rebase(newBase *Spec, baseIDs map[TypeID]TypeID) (map[TypeID]TypeID, error) {
	if s == nil {
		return nil, fmt.Errorf("rebase: nil receiver")
	}

	if s.d.base == nil {
		return nil, fmt.Errorf("rebase: receiver is a base spec, expected a split spec")
	}

	if newBase == nil {
		return nil, fmt.Errorf("rebase: new base spec is nil")
	}

	if newBase.d.base != nil {
		return nil, fmt.Errorf("rebase: new base must be a complete base spec, got a split spec")
	}

	if s.d.byteOrder != newBase.d.byteOrder {
		return nil, fmt.Errorf("rebase: byte order mismatch: split %v vs base %v", s.d.byteOrder, newBase.d.byteOrder)
	}

	return s.d.rebase(newBase.d, baseIDs)
}

// rebase performs the decoder level work described in [Spec.Rebase].
func (d *decoder) rebase(newBase *decoder, baseIDs map[TypeID]TypeID) (map[TypeID]TypeID, error) {
	oldBase := d.base
	newFirst := TypeID(newBase.len())

	// Validate that the caller supplied a valid, non-void target for every ID
	// of the currently attached base. baseIDs is keyed by these current IDs,
	// both for the first and any repeated Rebase.
	for currentID := TypeID(1); currentID < d.firstTypeID; currentID++ {
		newID, ok := baseIDs[currentID]
		if !ok {
			return nil, fmt.Errorf("rebase: missing mapping for base type id %d", currentID)
		}

		if newID == 0 {
			return nil, fmt.Errorf("rebase: base type id %d maps to void (id 0)", currentID)
		}

		if int(newID) >= newBase.len() {
			return nil, fmt.Errorf("rebase: target id %d for base type id %d does not exist in new base", newID, currentID)
		}
	}

	// Build the translation table from the IDs encoded in the original split
	// BTF to the IDs in effect after this Rebase. The table covers every record
	// of the module, including declaration tags.
	full := make(map[TypeID]TypeID, d.rawFirstTypeID+TypeID(d.len()))
	full[0] = 0

	if d.rebasedIDs == nil {
		// First Rebase: encoded base IDs are the current base IDs.
		for currentID := TypeID(1); currentID < d.firstTypeID; currentID++ {
			full[currentID] = baseIDs[currentID]
		}
	} else {
		// Repeated Rebase: translate the original encoded IDs through the
		// mapping currently in effect, then through the caller supplied one.
		for encoded, currentID := range d.rebasedIDs {
			if encoded == 0 || encoded >= d.rawFirstTypeID {
				continue
			}

			newID, ok := baseIDs[currentID]
			if !ok {
				return nil, fmt.Errorf("rebase: missing mapping for current base type id %d", currentID)
			}
			full[encoded] = newID
		}
	}

	// Local records keep their order and follow the new base directly.
	for idx := range d.len() {
		encodedID := d.rawFirstTypeID + TypeID(idx)
		full[encodedID] = newFirst + TypeID(idx)
	}

	// Rewire edges of already inflated local types. The objects themselves are
	// retained; only edges crossing into the old base change. Local to local
	// edges, including cycles, keep pointing at the same objects.
	rewireEdge := func(child *Type) error {
		t := *child
		if t == nil {
			return nil
		}

		if _, ok := t.(*Void); ok {
			return nil
		}

		if id, ok := d.typeIDs[t]; ok && id >= d.firstTypeID {
			// Local object, identity must be preserved. Note that typeIDs can
			// also contain base types reached through the split decoder; those
			// carry ids below firstTypeID and must be rewired.
			return nil
		}

		currentID, err := oldBase.typeID(t)
		if err != nil {
			// children() synthesizes declTag nodes from Tags fields. They are
			// not part of any decoder and only reference local types, so they
			// have no base edge to rewire.
			if _, ok := t.(*declTag); ok {
				return nil
			}

			return fmt.Errorf("rebase: type edge points outside of module: %w", err)
		}

		replacement, err := newBase.typeByID(baseIDs[currentID])
		if err != nil {
			return fmt.Errorf("rebase: inflate replacement for base id %d: %w", currentID, err)
		}

		*child = replacement
		return nil
	}

	for _, typ := range d.types {
		// declTag has no children in the generic traversal, handle its edge
		// explicitly so a base reference is not carried over.
		if dt, ok := typ.(*declTag); ok {
			if err := rewireEdge(&dt.Type); err != nil {
				return nil, err
			}
		}

		for child := range children(typ) {
			if err := rewireEdge(child); err != nil {
				return nil, err
			}
		}
	}

	// Renumber inflated local types and their reverse index. d.types only
	// contains local records; base types reached through delegation live in the
	// base decoder and must not be renumbered here.
	types := make(map[TypeID]Type, len(d.types))
	typeIDs := make(map[Type]TypeID, len(d.typeIDs))
	for oldID, typ := range d.types {
		if oldID < d.firstTypeID {
			continue
		}

		newID := newFirst + (oldID - d.firstTypeID)
		types[newID] = typ
		typeIDs[typ] = newID
	}

	// Keep the reverse index usable for base objects, mirroring how inflation
	// through the split caches delegated base types before a Rebase.
	for typ, id := range newBase.typeIDs {
		typeIDs[typ] = id
	}

	legacyBitfields := make(map[TypeID][2]Bits, len(d.legacyBitfields))
	for oldID, bits := range d.legacyBitfields {
		if oldID < d.firstTypeID {
			continue
		}
		newID := newFirst + (oldID - d.firstTypeID)
		legacyBitfields[newID] = bits
	}

	namedTypes, declTags, err := d.reindexLocalTypes(newFirst, full)
	if err != nil {
		return nil, err
	}

	// The split's raw name offsets are encoded relative to the length of the
	// original base string section. A replacement base with a different string
	// length would shift every offset, so keep the original offset arithmetic
	// intact by detaching from both base string tables: prepend a snapshot of
	// the original base string section (names such as member names may live
	// there) followed by the split's own bytes at their original offsets. The
	// strings are plain data and do not carry the old base type graph along.
	// Repeated Rebases rebuild the same table from the immutable snapshots.
	padded := make([]byte, d.stringBaseLen+uint32(len(d.localStringBytes)))
	copy(padded, d.baseStringBytes)
	copy(padded[d.stringBaseLen:], d.localStringBytes)
	strings := &stringTable{bytes: padded}

	d.base = newBase
	d.firstTypeID = newFirst
	d.strings = strings
	d.types = types
	d.typeIDs = typeIDs
	d.legacyBitfields = legacyBitfields
	d.namedTypes = namedTypes
	d.declTags = declTags
	d.rebasedIDs = maps.Clone(full)

	return full, nil
}

// reindexLocalTypes rebuilds the name index and decl tag index using current
// IDs after a Rebase. Raw type and name offsets are unchanged.
func (d *decoder) reindexLocalTypes(newFirst TypeID, full map[TypeID]TypeID) (*fuzzyStringIndex, map[TypeID][]TypeID, error) {
	var (
		header     btfType
		namedTypes = newFuzzyStringIndex(d.len())
		declTags   = make(map[TypeID][]TypeID)
	)

	idx := 0
	for _, err := range allBtfTypeOffsets(d.raw, d.byteOrder, &header) {
		if err != nil {
			return nil, nil, fmt.Errorf("rebase: scan local types: %w", err)
		}

		newID := newFirst + TypeID(idx)

		if header.Kind() == kindDeclTag {
			target, ok := full[header.Type()]
			if !ok {
				return nil, nil, fmt.Errorf("rebase: decl tag %d references unknown encoded id %d", newID, header.Type())
			}
			declTags[target] = append(declTags[target], newID)
		}

		if header.NameOff != 0 {
			name, err := d.strings.LookupBytes(header.NameOff)
			if err != nil {
				return nil, nil, fmt.Errorf("rebase: lookup name for id %d: %w", newID, err)
			}

			if len(name) > 0 {
				namedTypes.Add(name[:essentialNameLen(name)], newID)
			}
		}

		idx++
	}

	if idx != d.len() {
		return nil, nil, fmt.Errorf("rebase: rescanned %d local types, expected %d", idx, d.len())
	}

	namedTypes.Build()
	return namedTypes, declTags, nil
}

// resolveID translates an ID as encoded in the raw split BTF into the ID in
// effect after any Rebase. Encoded and current IDs are identical before the
// first Rebase.
func (d *decoder) resolveID(encoded TypeID) TypeID {
	if encoded == 0 || d.rebasedIDs == nil {
		return encoded
	}

	if current, ok := d.rebasedIDs[encoded]; ok {
		return current
	}

	return encoded
}
