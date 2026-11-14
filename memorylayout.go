// Copyright 2018 Saferwall. All rights reserved.
// Use of this source code is governed by Apache v2 license
// license that can be found in the LICENSE file.

package pe

import (
	"errors"
	"math"
)

// Memory layout parsing errors.
var (
	// ErrMemoryLayoutBase is returned when the loaded image base cannot be
	// expressed with the optional header pointer width (PE32 vs PE32+).
	ErrMemoryLayoutBase = errors.New(
		"loaded image base cannot be represented with the image pointer width")

	// ErrMemoryLayoutVA is returned when a virtual address is below the loaded
	// image base or encodes an RVA that does not fit in 32 bits.
	ErrMemoryLayoutVA = errors.New(
		"virtual address is outside of the loaded image")

	// ErrMemoryLayoutRVA is returned when an RVA points outside of the readable
	// image ranges: a section gap, beyond SizeOfImage or past the input length.
	ErrMemoryLayoutRVA = errors.New(
		"RVA is not within a readable image range")
)

// loadedImageBase returns the base address the synthesized image was loaded
// at. When Options.LoadedImageBase is left at zero the optional header's
// preferred ImageBase is used, matching the on-disk interpretation.
func (pe *File) loadedImageBase() uint64 {
	if pe.opts != nil && pe.opts.LoadedImageBase != 0 {
		return pe.opts.LoadedImageBase
	}
	switch pe.Is64 {
	case true:
		return pe.NtHeader.OptionalHeader.(ImageOptionalHeader64).ImageBase
	default:
		return uint64(pe.NtHeader.OptionalHeader.(ImageOptionalHeader32).ImageBase)
	}
}

// sizeOfImageFields returns SizeOfHeaders and SizeOfImage from the optional
// header without assuming a particular pointer width.
func (pe *File) sizeOfImageFields() (sizeOfHeaders, sizeOfImage uint32) {
	switch pe.Is64 {
	case true:
		oh := pe.NtHeader.OptionalHeader.(ImageOptionalHeader64)
		return oh.SizeOfHeaders, oh.SizeOfImage
	default:
		oh := pe.NtHeader.OptionalHeader.(ImageOptionalHeader32)
		return oh.SizeOfHeaders, oh.SizeOfImage
	}
}

// validateLoadedImageBase makes sure every VA that can appear in the image
// (base + an RVA below SizeOfImage) can be expressed with the current pointer
// width. A base that would force truncation into another legal-looking
// address is rejected up front instead.
func (pe *File) validateLoadedImageBase() error {
	if !pe.opts.MemoryLayout {
		return nil
	}

	base := pe.loadedImageBase()
	_, sizeOfImage := pe.sizeOfImageFields()

	if pe.Is64 {
		if base > math.MaxUint64-uint64(sizeOfImage) {
			return ErrMemoryLayoutBase
		}
		return nil
	}

	// PE32: every stored VA is a 32-bit quantity.
	if base > math.MaxUint32 {
		return ErrMemoryLayoutBase
	}
	if uint64(sizeOfImage) > math.MaxUint32+1-base {
		return ErrMemoryLayoutBase
	}
	return nil
}

// vaToRVA32 converts a 32-bit virtual address to an RVA using the effective
// loaded image base. It never silently wraps around.
func (pe *File) vaToRVA32(va uint32) (uint32, error) {
	base := pe.loadedImageBase()
	if uint64(va) < base {
		return 0, ErrMemoryLayoutVA
	}
	rva := uint64(va) - base
	if rva > math.MaxUint32 {
		return 0, ErrMemoryLayoutVA
	}
	return uint32(rva), nil
}

// vaToRVA64 converts a 64-bit virtual address to an RVA using the effective
// loaded image base.
func (pe *File) vaToRVA64(va uint64) (uint32, error) {
	base := pe.loadedImageBase()
	if va < base {
		return 0, ErrMemoryLayoutVA
	}
	rva := va - base
	if rva > math.MaxUint32 {
		return 0, ErrMemoryLayoutVA
	}
	return uint32(rva), nil
}

// sectionMemorySpan returns the readable half-open interval [va, end) of a
// section inside a loaded image: the section starts at its VirtualAddress and
// spans max(VirtualSize, SizeOfRawData) bytes, clamped to SizeOfImage and the
// actual input length.
func (pe *File) sectionMemorySpan(section *Section) (uint32, uint32) {
	va := section.Header.VirtualAddress
	size := Max(section.Header.VirtualSize, section.Header.SizeOfRawData)

	_, sizeOfImage := pe.sizeOfImageFields()
	limit := pe.size
	if sizeOfImage != 0 && sizeOfImage < limit {
		limit = sizeOfImage
	}

	end := va + size
	if end < va || end > limit {
		end = limit
	}
	if va >= end {
		return va, va
	}
	return va, end
}

// rvaReadable reports whether rva falls inside the headers or one of the
// section spans of a loaded image. Section gaps are deliberately not readable.
func (pe *File) rvaReadable(rva uint32) bool {
	sizeOfHeaders, _ := pe.sizeOfImageFields()
	if sizeOfHeaders > pe.size {
		sizeOfHeaders = pe.size
	}
	if rva < sizeOfHeaders {
		return true
	}

	for i := range pe.Sections {
		va, end := pe.sectionMemorySpan(&pe.Sections[i])
		if va <= rva && rva < end {
			return true
		}
	}
	return false
}

// resetParseState clears every field populated by Parse so that parsing the
// same File again cannot accumulate stale directory entries.
func (pe *File) resetParseState() {
	pe.DOSHeader = ImageDOSHeader{}
	pe.RichHeader = RichHeader{}
	pe.NtHeader = ImageNtHeader{}
	pe.COFF = COFF{}
	pe.Sections = nil
	pe.Imports = nil
	pe.Export = Export{}
	pe.Debugs = nil
	pe.Relocations = nil
	pe.Resources = ResourceDirectory{}
	pe.TLS = TLSDirectory{}
	pe.LoadConfig = LoadConfig{}
	pe.Exceptions = nil
	pe.Certificates = CertificateSection{}
	pe.DelayImports = nil
	pe.BoundImports = nil
	pe.GlobalPtr = 0
	pe.CLR = CLRData{}
	pe.IAT = nil
	pe.Anomalies = nil
	pe.Header = nil
	pe.FileInfo = FileInfo{}
	pe.OverlayOffset = 0
}
