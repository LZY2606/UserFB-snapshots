package binary

import (
	"bytes"
	"fmt"
	"io"

	"github.com/tetratelabs/wazero/api"
	"github.com/tetratelabs/wazero/internal/leb128"
	"github.com/tetratelabs/wazero/internal/wasm"
)

// Limits flags encoded in a limits type, including the memory64 variants.
// See https://webassembly.github.io/memory64/core/binary/types.html#binary-limits
const (
	limitsFlagMinOnly         = 0x00
	limitsFlagMinMax          = 0x01
	limitsFlagSharedMinOnly   = 0x02
	limitsFlagSharedMinMax    = 0x03
	limitsFlag64MinOnly       = 0x04
	limitsFlag64MinMax        = 0x05
	inspectionMemory32Limit64 = uint64(1) << 16
	inspectionMemory64Limit64 = uint64(1) << 48
)

// DecodeModuleForInspection decodes a module for static memory inspection
// instead of execution. It reuses the ordinary section decoders, but the
// memory, import-memory and data section decoders keep the full 32/64-bit
// limits width, arbitrary memory indices and complete ULEB64 memarg offsets.
//
// The returned module is never connected to the execution engine: no linear
// memory is allocated, no imports are resolved or called, and no function is
// executed. Memory references are established afterwards by Module.Validate.
func DecodeModuleForInspection(binary []byte, enabledFeatures api.CoreFeatures) (*wasm.Module, error) {
	return decodeModule(binary, enabledFeatures, 0, false, false, false, true)
}

// decodeLimitsTypeInspection reads a limits type without narrowing the page
// counts to uint32. Non-shared flags 0/1 (memory32) and 4/5 (memory64) are
// supported; shared memory (flags 2/3) is rejected because inspection does not
// model the threads proposal.
func decodeLimitsTypeInspection(r *bytes.Reader) (is64 bool, min, max uint64, hasMax, shared bool, err error) {
	var flag byte
	if flag, err = r.ReadByte(); err != nil {
		err = fmt.Errorf("read leading byte: %v", err)
		return
	}
	switch flag {
	case limitsFlagMinOnly, limitsFlagMinMax:
		is64 = false
	case limitsFlag64MinOnly, limitsFlag64MinMax:
		is64 = true
	case limitsFlagSharedMinOnly, limitsFlagSharedMinMax:
		err = fmt.Errorf("shared memory is not supported in inspection mode (limits flag %#x)", flag)
		return
	default:
		err = fmt.Errorf("%v for limits: %#x not in (0x00, 0x01, 0x04, 0x05)", ErrInvalidByte, flag)
		return
	}
	hasMax = flag == limitsFlagMinMax || flag == limitsFlag64MinMax

	if min, _, err = readULEB128Uint64(r); err != nil {
		err = fmt.Errorf("read min of limit: %v", err)
		return
	}
	if hasMax {
		if max, _, err = readULEB128Uint64(r); err != nil {
			err = fmt.Errorf("read max of limit: %v", err)
			return
		}
	}

	limit := inspectionMemory32Limit64
	if is64 {
		limit = inspectionMemory64Limit64
	}
	if min > limit {
		err = fmt.Errorf("min %d pages over inspection limit of %d pages", min, limit)
		return
	}
	if hasMax {
		if max > limit {
			err = fmt.Errorf("max %d pages over inspection limit of %d pages", max, limit)
			return
		}
		if min > max {
			err = fmt.Errorf("min %d pages > max %d pages", min, max)
			return
		}
	}
	return
}

// readULEB128Uint64 reads an unsigned LEB128 value at full 64-bit width from
// the section reader. leb128 only exposes a 64-bit loader over a byte slice,
// so the immediate bytes are buffered first (at most 10 bytes per spec).
func readULEB128Uint64(r *bytes.Reader) (uint64, uint64, error) {
	var raw []byte
	for len(raw) < 10 {
		b, err := r.ReadByte()
		if err != nil {
			return 0, 0, err
		}
		raw = append(raw, b)
		if b < 0x80 {
			break
		}
	}
	return leb128.LoadUint64(raw)
}

// decodeMemoryDeclarationInspection decodes one memory type into a
// MemoryDeclaration at the given combined memory index.
func decodeMemoryDeclarationInspection(r *bytes.Reader, index uint32, imported bool,
	importModule, importName string,
) (wasm.MemoryDeclaration, error) {
	is64, min, max, hasMax, shared, err := decodeLimitsTypeInspection(r)
	if err != nil {
		return wasm.MemoryDeclaration{}, err
	}
	if shared {
		return wasm.MemoryDeclaration{}, fmt.Errorf("shared memory is not supported in inspection mode")
	}
	return wasm.MemoryDeclaration{
		Index:        index,
		Imported:     imported,
		Is64:         is64,
		Min:          min,
		Max:          max,
		HasMax:       hasMax,
		ImportModule: importModule,
		ImportName:   importName,
	}, nil
}

// decodeMemorySectionInspection reads a memory section that may define more
// than one memory, preserving 64-bit page counts.
func decodeMemorySectionInspection(r *bytes.Reader) ([]wasm.MemoryDeclaration, error) {
	vs, _, err := leb128.DecodeUint32(r)
	if err != nil {
		return nil, fmt.Errorf("error reading size")
	}
	ret := make([]wasm.MemoryDeclaration, vs)
	for i := uint32(0); i < vs; i++ {
		decl, err := decodeMemoryDeclarationInspection(r, i, false, "", "")
		if err != nil {
			return nil, err
		}
		ret[i] = decl
	}
	return ret, nil
}

// decodeImportSectionInspection mirrors decodeImportSection but decodes
// imported memories with the inspection (32/64-bit, multi-memory) limits
// decoder. The returned declarations are indexed from zero within the imported
// memory prefix; the caller re-indexes them after combining with local ones.
func decodeImportSectionInspection(
	r *bytes.Reader,
	enabledFeatures api.CoreFeatures,
) (result []wasm.Import,
	perModule map[string][]*wasm.Import,
	funcCount, globalCount, memoryCount, tableCount wasm.Index,
	memoryDeclarations []wasm.MemoryDeclaration, err error,
) {
	vs, _, err := leb128.DecodeUint32(r)
	if err != nil {
		err = fmt.Errorf("get size of vector: %w", err)
		return
	}

	perModule = make(map[string][]*wasm.Import)
	result = make([]wasm.Import, vs)
	for i := uint32(0); i < vs; i++ {
		imp := &result[i]
		if imp.Module, _, err = decodeUTF8(r, "import module"); err != nil {
			err = fmt.Errorf("import[%d] error decoding module: %w", i, err)
			return
		}
		if imp.Name, _, err = decodeUTF8(r, "import name"); err != nil {
			err = fmt.Errorf("import[%d] error decoding name: %w", i, err)
			return
		}
		b, readErr := r.ReadByte()
		if readErr != nil {
			err = fmt.Errorf("import[%d] error decoding type: %w", i, readErr)
			return
		}
		imp.Type = b
		switch imp.Type {
		case wasm.ExternTypeFunc:
			imp.DescFunc, _, err = leb128.DecodeUint32(r)
		case wasm.ExternTypeTable:
			err = decodeTable(r, enabledFeatures, &imp.DescTable)
		case wasm.ExternTypeMemory:
			var decl wasm.MemoryDeclaration
			decl, err = decodeMemoryDeclarationInspection(r, memoryCount, true, imp.Module, imp.Name)
			if err == nil {
				memoryDeclarations = append(memoryDeclarations, decl)
			}
		case wasm.ExternTypeGlobal:
			imp.DescGlobal, err = decodeGlobalType(r)
		default:
			err = fmt.Errorf("%w: invalid byte for importdesc: %#x", ErrInvalidByte, b)
		}
		if err != nil {
			err = fmt.Errorf("import[%d] %s[%s.%s]: %w", i, wasm.ExternTypeName(imp.Type), imp.Module, imp.Name, err)
			return
		}
		switch imp.Type {
		case wasm.ExternTypeFunc:
			imp.IndexPerType = funcCount
			funcCount++
		case wasm.ExternTypeGlobal:
			imp.IndexPerType = globalCount
			globalCount++
		case wasm.ExternTypeMemory:
			imp.IndexPerType = memoryCount
			memoryCount++
		case wasm.ExternTypeTable:
			imp.IndexPerType = tableCount
			tableCount++
		}
		perModule[imp.Module] = append(perModule[imp.Module], imp)
	}
	return
}

// decodeDataSegmentInspection reads a data segment retaining the explicit
// memory index of active segments (prefix 0x2). Passive segments stay unbound.
func decodeDataSegmentInspection(r *bytes.Reader, enabledFeatures api.CoreFeatures, ret *wasm.DataSegment) error {
	dataSegmentPrefix, _, err := leb128.DecodeUint32(r)
	if err != nil {
		return fmt.Errorf("read data segment prefix: %w", err)
	}
	if dataSegmentPrefix != dataSegmentPrefixActive {
		if err = enabledFeatures.RequireEnabled(api.CoreFeatureBulkMemoryOperations); err != nil {
			return fmt.Errorf("non-zero prefix for data segment is invalid as %w", err)
		}
	}

	switch dataSegmentPrefix {
	case dataSegmentPrefixActive:
		ret.MemoryIndex = 0
		if err = decodeConstantExpression(r, enabledFeatures, &ret.OffsetExpression); err != nil {
			return fmt.Errorf("read offset expression: %v", err)
		}
	case dataSegmentPrefixActiveWithMemoryIndex:
		d, _, err := leb128.DecodeUint32(r)
		if err != nil {
			return fmt.Errorf("read memory index: %v", err)
		}
		ret.MemoryIndex = d
		if err = decodeConstantExpression(r, enabledFeatures, &ret.OffsetExpression); err != nil {
			return fmt.Errorf("read offset expression: %v", err)
		}
	case dataSegmentPrefixPassive:
		// Passive segments are not bound to a memory: no index, no offset.
		ret.Passive = true
	default:
		return fmt.Errorf("invalid data segment prefix: 0x%x", dataSegmentPrefix)
	}

	vs, _, err := leb128.DecodeUint32(r)
	if err != nil {
		return fmt.Errorf("get the size of vector: %v", err)
	}
	ret.Init = make([]byte, vs)
	if _, err = io.ReadFull(r, ret.Init); err != nil {
		return fmt.Errorf("read bytes for init: %v", err)
	}
	return nil
}

func decodeDataSectionInspection(r *bytes.Reader, enabledFeatures api.CoreFeatures) ([]wasm.DataSegment, error) {
	vs, _, err := leb128.DecodeUint32(r)
	if err != nil {
		return nil, fmt.Errorf("get size of vector: %w", err)
	}
	result := make([]wasm.DataSegment, vs)
	for i := uint32(0); i < vs; i++ {
		if err = decodeDataSegmentInspection(r, enabledFeatures, &result[i]); err != nil {
			return nil, fmt.Errorf("read data segment: %w", err)
		}
	}
	return result, nil
}
