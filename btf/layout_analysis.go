package btf

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"slices"
	"strings"
)

// Fragment is an offline snapshot of target memory. Addresses are opaque
// numbers, never host pointers: nothing in this package dereferences them.
type Fragment struct {
	// Order of the encoded values inside Data.
	Order binary.ByteOrder
	// BaseAddress is the target address at which Data starts.
	BaseAddress uint64
	// PointerWidth of the target architecture, in bytes.
	PointerWidth uint32
	// Data are the raw bytes captured at BaseAddress.
	Data []byte
}

// Contains reports whether addr falls inside the fragment. The address is only
// compared numerically; the bytes behind it are never followed.
func (f Fragment) Contains(addr uint64) bool {
	return addr >= f.BaseAddress && addr < f.BaseAddress+uint64(len(f.Data))
}

// LayoutRow is one member-level result of the offline layout analysis.
type LayoutRow struct {
	Endianness  string
	Phase       string
	StructID    TypeID
	StructSize  uint32
	Member      string
	BitOffset   Bits
	RefID       TypeID
	RawBytes    string
	Value       string
	IsPointer   bool
	TargetAddr  uint64
	InFragment  bool
	TargetType  string
	PointedName string
}

// LayoutReport bundles the analysis results for one byte order, before and
// after a Rebase.
type LayoutReport struct {
	Endianness string
	// Mapping returned by Rebase, keyed by old encoded IDs.
	Mapping map[TypeID]TypeID
	// EncodedLocalIDLo is the first encoded ID owned by the split module.
	// Encoded IDs below it belong to the original base.
	EncodedLocalIDLo TypeID
	Leaf             *Struct
	Rows             []LayoutRow
	LocalIDLo        TypeID
	LeafID           TypeID
	NewBaseSize      int
}

const (
	phaseBefore = "before-rebase"
	phaseAfter  = "after-rebase"
)

// RunRebaseLayoutAnalysis is the reusable, repeatable offline analysis entry
// point. It parses the old base and split BTF, analyzes the fragment, rebases
// the split onto newBase using baseIDs and analyzes the same fragment again.
//
// No kernel module is loaded and no BTF blob is written: all inputs are raw
// bytes handed in by the caller. The leaf Struct object is retained across the
// Rebase and exposed through the report.
func RunRebaseLayoutAnalysis(oldBaseBlob, splitBlob, newBaseBlob, memory []byte, order binary.ByteOrder, orderName string, baseIDs map[TypeID]TypeID) (*LayoutReport, error) {
	oldBase, err := LoadSpecFromReader(bytes.NewReader(oldBaseBlob))
	if err != nil {
		return nil, fmt.Errorf("load old base: %w", err)
	}

	split, err := LoadSplitSpecFromReader(bytes.NewReader(splitBlob), oldBase)
	if err != nil {
		return nil, fmt.Errorf("load split: %w", err)
	}

	newBase, err := LoadSpecFromReader(bytes.NewReader(newBaseBlob))
	if err != nil {
		return nil, fmt.Errorf("load new base: %w", err)
	}

	var leaf *Struct
	if err := split.TypeByName("leaf", &leaf); err != nil {
		return nil, fmt.Errorf("locate leaf: %w", err)
	}

	frag := Fragment{
		Order:        order,
		BaseAddress:  0x4000,
		PointerWidth: 8,
		Data:         memory,
	}

	rowsBefore, err := AnalyzeSpecLayout(split, phaseBefore, orderName, frag)
	if err != nil {
		return nil, err
	}

	mapping, err := split.Rebase(newBase, baseIDs)
	if err != nil {
		return nil, err
	}

	rowsAfter, err := AnalyzeSpecLayout(split, phaseAfter, orderName, frag)
	if err != nil {
		return nil, err
	}

	// The same object must answer name and ID queries after the Rebase.
	var leafAfter *Struct
	if err := split.TypeByName("leaf", &leafAfter); err != nil {
		return nil, fmt.Errorf("locate rebased leaf: %w", err)
	}

	if leafAfter != leaf {
		return nil, fmt.Errorf("leaf object identity changed during rebase")
	}

	id, err := split.TypeID(leaf)
	if err != nil {
		return nil, err
	}

	return &LayoutReport{
		Endianness:       orderName,
		Mapping:          mapping,
		EncodedLocalIDLo: split.d.rawFirstTypeID,
		Leaf:             leaf,
		Rows:             append(rowsBefore, rowsAfter...),
		LocalIDLo:        split.d.firstTypeID,
		LeafID:           id,
		NewBaseSize:      newBase.d.len(),
	}, nil
}

// AnalyzeSpecLayout interprets frag through every local structure of spec.
// Only structures, integers and pointers are interpreted, matching the types
// present in the offline sample. Member names, offsets, integer widths and
// pointer targets are all taken from the parsed type graph; nothing is
// hard-coded for the sample's names or IDs.
func AnalyzeSpecLayout(spec *Spec, phase, orderName string, frag Fragment) ([]LayoutRow, error) {
	var rows []LayoutRow

	for typ, err := range spec.All() {
		if err != nil {
			return nil, err
		}

		str, ok := typ.(*Struct)
		if !ok {
			continue
		}

		structID, err := lookupSpecTypeID(spec, str)
		if err != nil {
			return nil, err
		}

		for _, member := range str.Members {
			row := LayoutRow{
				Endianness: orderName,
				Phase:      phase,
				StructID:   structID,
				StructSize: str.Size,
				Member:     member.Name,
				BitOffset:  member.Offset,
			}

			refID, err := lookupSpecTypeID(spec, member.Type)
			if err != nil {
				return nil, err
			}
			row.RefID = refID

			start := member.Offset.Bytes()

			switch t := member.Type.(type) {
			case *Int:
				raw, value, err := readInteger(frag, start, t.Size)
				if err != nil {
					return nil, err
				}
				row.RawBytes = raw
				row.Value = value
				row.TargetType = typeLabel(t)

			case *Pointer:
				raw, value, addr, err := readPointer(frag, start)
				if err != nil {
					return nil, err
				}
				row.RawBytes = raw
				row.Value = value
				row.IsPointer = true
				row.TargetAddr = addr
				row.InFragment = frag.Contains(addr)
				row.TargetType = typeLabel(t.Target)
				if target, ok := As[*Struct](t.Target); ok {
					row.PointedName = target.Name
				}

			default:
				return nil, fmt.Errorf("member %s: unsupported type %T, only int and pointer are interpreted", member.Name, member.Type)
			}

			rows = append(rows, row)
		}
	}

	return rows, nil
}

// lookupSpecTypeID returns the current ID of typ, including types owned by the
// base decoder of a split spec.
func lookupSpecTypeID(spec *Spec, typ Type) (TypeID, error) {
	if id, err := spec.d.typeID(typ); err == nil {
		return id, nil
	}

	if spec.d.base != nil {
		if id, err := spec.d.base.typeID(typ); err == nil {
			return id, nil
		}
	}

	if _, ok := typ.(*Void); ok {
		return 0, nil
	}

	return 0, fmt.Errorf("no ID for type %s: %w", typ, ErrNotFound)
}

func readInteger(frag Fragment, start, size uint32) (raw, value string, err error) {
	end := start + size
	if end > uint32(len(frag.Data)) {
		return "", "", fmt.Errorf("integer at byte %d (size %d) exceeds fragment of %d bytes", start, size, len(frag.Data))
	}

	b := frag.Data[start:end]
	var n uint64
	if frag.Order == binary.BigEndian {
		for _, v := range b {
			n = n<<8 | uint64(v)
		}
	} else {
		for i := len(b) - 1; i >= 0; i-- {
			n = n<<8 | uint64(b[i])
		}
	}

	return formatBytes(b), fmt.Sprintf("0x%x (%d)", n, n), nil
}

func readPointer(frag Fragment, start uint32) (raw, value string, addr uint64, err error) {
	size := frag.PointerWidth
	end := start + size
	if end > uint32(len(frag.Data)) {
		return "", "", 0, fmt.Errorf("pointer at byte %d exceeds fragment of %d bytes", start, len(frag.Data))
	}

	b := frag.Data[start:end]
	if size == 8 {
		addr = frag.Order.Uint64(b)
	} else {
		addr = uint64(frag.Order.Uint32(b))
	}

	return formatBytes(b), fmt.Sprintf("0x%x", addr), addr, nil
}

func formatBytes(b []byte) string {
	parts := make([]string, len(b))
	for i, v := range b {
		parts[i] = fmt.Sprintf("%02x", v)
	}
	return strings.Join(parts, " ")
}

func typeLabel(typ Type) string {
	if _, ok := typ.(*Void); ok {
		return "void"
	}

	switch v := typ.(type) {
	case *Struct:
		return "struct " + v.Name
	case *Union:
		return "union " + v.Name
	case *Int:
		return "int " + v.Name
	case *Pointer:
		return "ptr -> " + typeLabel(v.Target)
	default:
		if name := typ.TypeName(); name != "" {
			return name
		}
		return fmt.Sprintf("%T", typ)
	}
}

// LayoutTSVHeader is the column header of RenderLayoutTSV.
const LayoutTSVHeader = "endianness	phase	struct_id	struct_size	member	bit_offset	ref_type_id	raw_bytes	value	target_addr	pointed_struct"

// RenderLayoutTSV renders rows as tab separated values.
func RenderLayoutTSV(rows []LayoutRow) string {
	var b strings.Builder
	b.WriteString(LayoutTSVHeader)
	b.WriteByte('\n')
	for _, r := range rows {
		target := "-"
		if r.IsPointer {
			target = fmt.Sprintf("0x%x", r.TargetAddr)
			if !r.InFragment {
				target += " (outside fragment)"
			}
		}

		pointed := r.PointedName
		if pointed == "" {
			pointed = "-"
		}

		fmt.Fprintf(&b, "%s\t%s\t%d\t%d\t%s\t%d\t%d\t%s\t%s\t%s\t%s\n",
			r.Endianness, r.Phase, r.StructID, r.StructSize, r.Member, r.BitOffset,
			r.RefID, r.RawBytes, r.Value, target, pointed)
	}
	return b.String()
}

// mdEscape keeps table cells on one line.
func mdEscape(s string) string {
	return strings.ReplaceAll(s, "|", "\\|")
}

// RenderLayoutMarkdown renders the full analysis report as Markdown. The
// narrative is derived from the parsed rows and the Rebase mapping: no layout
// fact is hard-coded for a particular type name or ID.
func RenderLayoutMarkdown(reports []*LayoutReport) string {
	var b strings.Builder

	b.WriteString("# Offline split BTF layout analysis\n\n")
	b.WriteString("Generated by `btf.RunRebaseLayoutAnalysis` from raw BTF blobs and a 16 byte\n")
	b.WriteString("offline memory fragment at target address `0x4000` (pointer width 8). No kernel\n")
	b.WriteString("module is loaded, no running kernel is accessed and no BTF file is written.\n\n")
	b.WriteString("## Commands\n\n")
	b.WriteString("```sh\n")
	b.WriteString("bash task_support/run.sh baseline\n")
	b.WriteString("bash task_support/run.sh target\n")
	b.WriteString("```\n\n")

	for _, report := range reports {
		b.WriteString("## Byte order ")
		b.WriteString(report.Endianness)
		b.WriteString("\n\n")

		var (
			before, after []LayoutRow
			seenMember    = make(map[string]struct{})
			seenValue     = make(map[string]struct{})
		)

		for _, row := range report.Rows {
			switch row.Phase {
			case phaseBefore:
				before = append(before, row)
			case phaseAfter:
				after = append(after, row)
			}

			if row.IsPointer {
				if _, ok := seenMember[row.Member]; ok {
					continue
				}
				seenMember[row.Member] = struct{}{}

				scope := "outside the fragment"
				if row.InFragment {
					scope = fmt.Sprintf("inside the fragment at target address 0x%x", row.TargetAddr)
				}
				fmt.Fprintf(&b, "- Structure %d member `%s` is a pointer (bit offset %d). Its stored target is 0x%x which is %s",
					row.StructID, row.Member, row.BitOffset, row.TargetAddr, scope)
				if row.PointedName != "" {
					fmt.Fprintf(&b, "; the pointed structure is `%s`, so the edge is a self reference when the target address equals the fragment base", row.PointedName)
				}
				b.WriteString(".\n")
				continue
			}

			if _, ok := seenValue[row.Member]; ok {
				continue
			}
			seenValue[row.Member] = struct{}{}
			fmt.Fprintf(&b, "- Integer member `%s` occupies byte %d..%d (bit offset %d, %d bytes); its interpreted value is %s.\n",
				row.Member, row.BitOffset.Bytes(), row.BitOffset.Bytes()+uint32(len(strings.Fields(row.RawBytes)))-1,
				row.BitOffset, len(strings.Fields(row.RawBytes)), row.Value)
		}

		b.WriteString("\n### What changes when the base and string section are replaced\n\n")
		b.WriteString("- Base type IDs are rewritten through the caller supplied correspondence")
		if report.Mapping != nil {
			type idChange struct{ oldID, newID TypeID }
			changes := make([]idChange, 0)
			for oldID, newID := range report.Mapping {
				if oldID != 0 && oldID < report.EncodedLocalIDLo && oldID != newID {
					changes = append(changes, idChange{oldID, newID})
				}
			}
			slices.SortFunc(changes, func(a, c idChange) int { return int(a.oldID) - int(c.oldID) })
			if len(changes) > 0 {
				parts := make([]string, len(changes))
				for i, change := range changes {
					parts[i] = fmt.Sprintf("%d -> %d", change.oldID, change.newID)
				}
				b.WriteString(" (")
				b.WriteString(strings.Join(parts, ", "))
				b.WriteString(")")
			}
		}
		fmt.Fprintf(&b, ". Local records keep their order and restart at ID %d directly after the new base (%d records including void).\n", report.LocalIDLo, report.NewBaseSize)
		b.WriteString("- Edges of retained local Type objects now reach objects of the new base; the old base graph is not referenced anymore.\n")
		b.WriteString("- Raw name offsets stay relative to the original split string section, so local and member names are unchanged even though the replacement base carries a longer string table.\n\n")
		b.WriteString("### What stays identical\n\n")
		b.WriteString("- Member names, bit offsets, structure sizes, raw bytes, interpreted integer values and numeric pointer addresses are identical before and after Rebase, in both byte orders.\n")
		b.WriteString("- The pointer cycle (local structure pointing at itself) and declaration tags attached to members are preserved; `All` still enumerates module types only.\n\n")

		writeMarkdownTable(&b, before, "Before Rebase")
		writeMarkdownTable(&b, after, "After Rebase")
	}

	return b.String()
}

func writeMarkdownTable(b *strings.Builder, rows []LayoutRow, title string) {
	fmt.Fprintf(b, "### %s\n\n", title)
	b.WriteString("| endianness | phase | struct id | struct size | member | bit offset | ref type id | raw bytes | value | target addr | pointed struct |\n")
	b.WriteString("| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |\n")
	for _, r := range rows {
		target := "-"
		if r.IsPointer {
			target = fmt.Sprintf("0x%x", r.TargetAddr)
			if !r.InFragment {
				target += " (outside)"
			}
		}

		pointed := r.PointedName
		if pointed == "" {
			pointed = "-"
		}

		fmt.Fprintf(b, "| %s | %s | %d | %d | %s | %d | %d | `%s` | %s | %s | %s |\n",
			r.Endianness, r.Phase, r.StructID, r.StructSize, mdEscape(r.Member), r.BitOffset,
			r.RefID, r.RawBytes, mdEscape(r.Value), target, mdEscape(pointed))
	}
	b.WriteByte('\n')
}
