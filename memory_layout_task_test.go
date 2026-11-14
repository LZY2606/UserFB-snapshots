// Copyright 2018 Saferwall. All rights reserved.
// Use of this source code is governed by Apache v2 license
// license that can be found in the LICENSE file.

package pe

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"testing"

	"github.com/saferwall/pe/task_support/taskcheck"
)

// Synthetic fixture coordinates. The model describes one .rdata section whose
// raw-backed part covers RVA [0x1000, 0x1600); the virtual zero fill extends
// it to 0x1700 and SizeOfImage is 0x2000. The same content is emitted twice:
// once laid out as an on-disk PE and once as a loader-mapped image.
const (
	fxSizeOfHeaders = 0x200
	fxSectionVA     = 0x1000
	fxRawSize       = 0x600
	fxVirtualSize   = 0x700
	fxSizeOfImage   = 0x2000
	fxDiskLength    = 0x810
	fxOverlayLen    = 0x10
)

// fxPE carries the two paired representations of the same synthetic image.
type fxPE struct {
	bits       int
	headerBase uint64
	loadedBase uint64
	psize      uint32
	disk       []byte
	image      []byte
}

// fxBuild produces the paired disk/image samples. VA-shaped fields are written
// with headerBase in the disk file and with loadedBase in the mapped image,
// mirroring what a loader fix-up would produce without performing any here.
func fxBuild(bits int, headerBase, loadedBase uint64) *fxPE {
	fx := &fxPE{bits: bits, headerBase: headerBase, loadedBase: loadedBase}
	if bits == 64 {
		fx.psize = 8
	} else {
		fx.psize = 4
	}

	disk := make([]byte, fxDiskLength)
	image := make([]byte, fxSizeOfImage)

	u16 := func(b []byte, o int, v uint16) { binary.LittleEndian.PutUint16(b[o:], v) }
	u32 := func(b []byte, o int, v uint32) { binary.LittleEndian.PutUint32(b[o:], v) }
	u64 := func(b []byte, o int, v uint64) { binary.LittleEndian.PutUint64(b[o:], v) }
	putStr := func(b []byte, o int, s string) { copy(b[o:], s) }

	// RVA -> (disk file offset, image index).
	atRVA := func(rva uint32) (int, int) {
		return int(rva - fxSectionVA + fxSizeOfHeaders), int(rva)
	}
	// Write the same bytes at an RVA into both representations.
	putRVA := func(rva uint32, write func(b []byte, o int)) {
		do, io := atRVA(rva)
		write(disk, do)
		write(image, io)
	}
	putRVABytes := func(rva uint32, p []byte) {
		putRVA(rva, func(b []byte, o int) { copy(b[o:], p) })
	}
	putRVAU16 := func(rva uint32, v uint16) { putRVA(rva, func(b []byte, o int) { u16(b, o, v) }) }
	putRVAU32 := func(rva uint32, v uint32) { putRVA(rva, func(b []byte, o int) { u32(b, o, v) }) }
	putRVAString := func(rva uint32, s string) { putRVABytes(rva, []byte(s)) }

	// VA writer: PE32 stores a uint32, PE32+ a uint64.
	putVA := func(b []byte, o int, rva uint32, base uint64) {
		if fx.psize == 8 {
			u64(b, o, base+uint64(rva))
		} else {
			u32(b, o, uint32(base+uint64(rva)))
		}
	}
	// Write a VA pointing at targetRVA into the slot at slotRVA, using the
	// preferred base on disk and the loaded base in the mapped image.
	putFixedVAField := func(slotRVA, targetRVA uint32) {
		do, io := atRVA(slotRVA)
		putVA(disk, do, targetRVA, headerBase)
		putVA(image, io, targetRVA, loadedBase)
	}

	// ---- Headers (identical coordinates in both layouts) ----
	for _, b := range [][]byte{disk, image} {
		copy(b, "MZ")
		u32(b, 0x3c, 0x80)
		copy(b[0x80:], "PE\x00\x00")
		u16(b, 0x86, 1)
		u16(b, 0x96, 0x102)
	}

	opt := 0x98
	var dirs, sec int
	if bits == 64 {
		dirs = opt + 112
		sec = opt + 240
		for _, b := range [][]byte{disk, image} {
			u16(b, 0x84, uint16(ImageFileMachineAMD64))
			u16(b, 0x94, 240)
			u16(b, opt, ImageNtOptionalHeader64Magic)
			u64(b, opt+24, headerBase)
			u32(b, opt+108, 16)
		}
	} else {
		dirs = opt + 96
		sec = opt + 224
		for _, b := range [][]byte{disk, image} {
			u16(b, 0x84, uint16(ImageFileMachineI386))
			u16(b, 0x94, 224)
			u16(b, opt, ImageNtOptionalHeader32Magic)
			u32(b, opt+28, uint32(headerBase))
			u32(b, opt+92, 16)
		}
	}

	for _, b := range [][]byte{disk, image} {
		u32(b, opt+32, 0x1000) // SectionAlignment
		u32(b, opt+36, 0x200)  // FileAlignment
		u32(b, opt+56, fxSizeOfImage)
		u32(b, opt+60, fxSizeOfHeaders)
		u16(b, opt+40, 6)
		u16(b, opt+48, 6)
		u16(b, opt+68, 3)

		// Data directories.
		dir := func(idx int, rva, size uint32) {
			u32(b, dirs+idx*8, rva)
			u32(b, dirs+idx*8+4, size)
		}
		dir(1, 0x1000, 40)         // ordinary RVA import
		dir(4, 0x580, 20)          // security: the VA is a raw file offset
		dir(6, 0x10A0, 28)         // debug
		dir(9, 0x1100, fx.psize*6) // TLS (24 for PE32, 40 for PE32+)
		if bits == 64 {
			dir(13, 0x1300, 32) // one RVA-form delay descriptor
		} else {
			dir(13, 0x1300, 64) // VA-form + RVA-form descriptors
		}

		// COFF symbol table pointer: a file coordinate ignored in images.
		u32(b, 0x8C, 0x2D0) // PointerToSymbolTable
		u32(b, 0x90, 1)     // NumberOfSymbols

		// Section header: PointerToRawData keeps its file value in both.
		putStr(b, sec, ".rdata")
		u32(b, sec+8, fxVirtualSize)
		u32(b, sec+12, fxSectionVA)
		u32(b, sec+16, fxRawSize)
		u32(b, sec+20, 0x200)
		u32(b, sec+36, 0x40000040)
	}

	// Marker used by the coordinate checks, echoes the baseline fixture.
	putRVAString(0x1050, "DATA")

	// ---- Ordinary RVA import: descriptor, thunks, names ----
	putRVAU32(0x1000, 0x1200)    // OriginalFirstThunk (INT)
	putRVAU32(0x1000+12, 0x1260) // Name
	putRVAU32(0x1000+16, 0x1220) // FirstThunk (IAT)
	putRVA(0x1200, func(b []byte, o int) {
		if fx.psize == 8 {
			u64(b, o, 0x1280)
		} else {
			u32(b, o, 0x1280)
		}
	})
	putRVA(0x1220, func(b []byte, o int) {
		if fx.psize == 8 {
			u64(b, o, 0x1280)
		} else {
			u32(b, o, 0x1280)
		}
	})
	putRVAString(0x1260, "normal.dll\x00")
	putRVAU16(0x1280, 9)
	putRVAString(0x1282, "NormalFunc\x00")

	// ---- COFF symbol (18 bytes) at file offset 0x2D0 (RVA 0x10D0) ----
	putRVABytes(0x10D0, append([]byte("SYM"), make([]byte, 15)...))
	// The COFF string table immediately follows with size 4.
	putRVAU32(0x10E2, 4)

	// ---- Debug directory (28 bytes) at RVA 0x10A0, CodeView at 0x1240 ----
	putRVAU32(0x10A0+12, ImageDebugTypeCodeView)
	putRVAU32(0x10A0+16, 30)     // SizeOfData = 24 + 1 terminator + 5-name
	putRVAU32(0x10A0+20, 0x1240) // AddressOfRawData (image coordinate)
	putRVAU32(0x10A0+24, 0x440)  // PointerToRawData (file coordinate)
	putRVAString(0x1240, "RSDS")
	for i := 0; i < 16; i++ {
		putRVABytes(0x1244+uint32(i), []byte{byte(i + 1)})
	}
	putRVAU32(0x1254, 7) // Age follows the 16-byte GUID
	putRVAString(0x1258, "t.pdb\x00")

	// ---- TLS directory at RVA 0x1100 with VA-shaped fields ----
	tlsDO, tlsIO := atRVA(0x1100)
	cbDO, cbIO := atRVA(0x1160)
	writeTLS := func(b []byte, base uint64, dirOff, cbOff int) {
		putVA(b, dirOff+0, 0x1140, base) // StartAddressOfRawData
		putVA(b, dirOff+int(fx.psize), 0x1148, base)
		putVA(b, dirOff+int(2*fx.psize), 0x1150, base)
		putVA(b, dirOff+int(3*fx.psize), 0x1160, base) // AddressOfCallBacks
		u32(b, dirOff+int(4*fx.psize), 0)              // SizeOfZeroFill

		// Callback table: one callback VA then a zero entry.
		putVA(b, cbOff, 0x1180, base)
		u32(b, cbOff+int(fx.psize), 0)
	}
	writeTLS(disk, headerBase, tlsDO, cbDO)
	writeTLS(image, loadedBase, tlsIO, cbIO)

	// ---- Delay imports ----
	if bits == 32 {
		// Descriptor A at 0x1300: legacy VA form (Attributes == 0). Every
		// address field is a full VA pointing at the named target RVA.
		putRVAU32(0x1300+0, 0)
		putFixedVAField(0x1300+4, 0x15A0)  // Name
		putFixedVAField(0x1300+8, 0x13A8)  // ModuleHandle
		putFixedVAField(0x1300+12, 0x1400) // IAT
		putFixedVAField(0x1300+16, 0x1420) // INT

		// Descriptor B at 0x1320: modern RVA form (Attributes == 1).
		putRVAU32(0x1320+0, 1)
		putRVAU32(0x1320+4, 0x15B0)
		putRVAU32(0x1320+12, 0x1500)
		putRVAU32(0x1320+16, 0x1520)

		// VA-form thunk tables: both slots point at the hint/name at 0x1480.
		putFixedVAField(0x1400, 0x1480)
		putFixedVAField(0x1420, 0x1480)
		putRVAU16(0x1480, 7)
		putRVAString(0x1482, "VaDelayFunc\x00")
		putRVAString(0x15A0, "va.dll\x00")
	} else {
		// PE32+ only carries an RVA-form delay descriptor.
		putRVAU32(0x1300+0, 1)
		putRVAU32(0x1300+4, 0x15B0)
		putRVAU32(0x1300+12, 0x1500)
		putRVAU32(0x1300+16, 0x1520)
	}

	// RVA-form thunk tables (width specific).
	putRVA(0x1500, func(b []byte, o int) {
		if fx.psize == 8 {
			u64(b, o, 0x1580)
		} else {
			u32(b, o, 0x1580)
		}
	})
	putRVA(0x1520, func(b []byte, o int) {
		if fx.psize == 8 {
			u64(b, o, 0x1580)
		} else {
			u32(b, o, 0x1580)
		}
	})
	putRVAU16(0x1580, 8)
	putRVAString(0x1582, "RvaDelayFunc\x00")
	putRVAString(0x15B0, "rva.dll\x00")

	// ---- WIN_CERTIFICATE header at raw file offset 0x580 (RVA 0x1380) ----
	// The PKCS#7 body is deliberately invalid; the directory only needs the
	// header to prove disk parsing reaches file-only data and images skip it.
	u32(disk, 0x580, 20)
	u16(disk, 0x584, WinCertRevision2_0)
	u16(disk, 0x586, WinCertTypePKCSSignedData)
	for i := 0; i < 12; i++ {
		disk[0x588+i] = 0xAB
	}

	// Overlay exists only in the disk representation.
	copy(disk[0x800:], "OVERLAY_DATA!!!!")

	// Non-mapped ranges of the image carry sentinels so gap bytes can never
	// be accepted as header or section data.
	for i := fxSizeOfHeaders; i < fxSectionVA; i++ {
		image[i] = 'G'
	}
	for i := 0x1700; i < fxSizeOfImage; i++ {
		image[i] = 'X'
	}

	fx.disk = disk
	fx.image = image
	return fx
}

// fxParse parses a copy of the given buffer so the test can afterwards prove
// the caller's input was not modified.
func fxParse(t *testing.T, input []byte, opts *Options) (*File, []byte) {
	t.Helper()
	original := append([]byte(nil), input...)
	f, err := NewBytes(input, opts)
	if err != nil {
		t.Fatalf("NewBytes: %v", err)
	}
	if err := f.Parse(); err != nil {
		t.Fatalf("Parse: %v", err)
	}
	return f, original
}

func fxDiskOpts() *Options {
	return &Options{DisableCertValidation: true, DisableSignatureValidation: true}
}

func fxImageOpts(base uint64) *Options {
	return &Options{MemoryLayout: true, LoadedImageBase: base}
}

// TestMemoryLayoutCoordinates verifies that RVA/offset translation and data
// reads follow the section table on disk and the identity mapping in images.
func TestMemoryLayoutCoordinates(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		for _, bits := range []int{32, 64} {
			headerBase := uint64(0x400000)
			loadedBase := uint64(0x500000)
			if bits == 64 {
				headerBase = 0x140000000
				loadedBase = 0x150000000
			}
			fx := fxBuild(bits, headerBase, loadedBase)

			disk, _ := fxParse(t, fx.disk, fxDiskOpts())
			image, _ := fxParse(t, fx.image, fxImageOpts(loadedBase))

			// Disk: RVA 0x1050 lives at file offset 0x250.
			c.Check(disk.GetOffsetFromRva(0x1050) == 0x250, "disk RVA->offset PE%d", bits)
			c.Check(disk.GetRVAFromOffset(0x250) == 0x1050, "disk offset->RVA PE%d", bits)
			d, err := disk.GetData(0x1050, 4)
			c.Check(err == nil && string(d) == "DATA", "disk GetData marker PE%d", bits)

			// Image: valid RVAs are identity indices.
			c.Check(image.GetOffsetFromRva(0x1050) == 0x1050, "image RVA->index PE%d", bits)
			c.Check(image.GetRVAFromOffset(0x1050) == 0x1050, "image index->RVA PE%d", bits)
			d, err = image.GetData(0x1050, 4)
			c.Check(err == nil && string(d) == "DATA", "image GetData marker PE%d", bits)

			// Headers occupy [0, SizeOfHeaders) in both layouts.
			c.Check(image.GetOffsetFromRva(0x80) == 0x80, "image header index PE%d", bits)
			d, err = image.GetData(0x80, 4)
			c.Check(err == nil && string(d) == "PE\x00\x00", "image header bytes PE%d", bits)

			// The header-to-section gap is not mapped data.
			c.Check(image.GetOffsetFromRva(0x500) == ^uint32(0), "image gap offset invalid PE%d", bits)
			c.Check(image.GetRVAFromOffset(0x500) == ^uint32(0), "image gap RVA invalid PE%d", bits)
			_, err = image.GetData(0x500, 4)
			c.Check(err != nil, "image gap GetData error PE%d", bits)

			// The same numeric offset is a valid section file offset on disk.
			c.Check(disk.GetRVAFromOffset(0x500) == 0x1300, "disk offset 0x500 is section data PE%d", bits)

			// Virtual zero fill: present and zero in the image, inside the span.
			d, err = image.GetData(0x1680, 8)
			c.Check(err == nil && len(d) == 8 && bytes.Equal(d, make([]byte, 8)),
				"image zero fill PE%d", bits)

			// Trailing gap inside SizeOfImage, and anything past SizeOfImage.
			c.Check(image.GetOffsetFromRva(0x1F00) == ^uint32(0), "image trailing gap PE%d", bits)
			c.Check(image.GetOffsetFromRva(0x3000) == ^uint32(0), "image beyond SizeOfImage PE%d", bits)
			_, err = image.GetData(0x1F00, 4)
			c.Check(err != nil, "image trailing gap GetData error PE%d", bits)

			// Section.Data honors the same coordinates.
			c.Check(len(image.Sections) == 1, "image section count PE%d", bits)
			sd := image.Sections[0].Data(0x1050, 4, image)
			c.Check(string(sd) == "DATA", "image Section.Data marker PE%d", bits)
			sd = disk.Sections[0].Data(0x1050, 4, disk)
			c.Check(string(sd) == "DATA", "disk Section.Data marker PE%d", bits)

			// Raw header fields are preserved in the mapped image.
			c.Check(image.Sections[0].Header.PointerToRawData == 0x200,
				"image PointerToRawData preserved PE%d", bits)
			c.Check(image.Sections[0].Header.VirtualAddress == 0x1000,
				"image VirtualAddress preserved PE%d", bits)

			// A truncated image clamps readable ranges to the input length.
			trunc := append([]byte(nil), fx.image[:0x1500]...)
			tf, terr := NewBytes(trunc, fxImageOpts(loadedBase))
			c.Check(terr == nil, "truncated NewBytes PE%d", bits)
			c.Check(tf.Parse() == nil, "truncated Parse PE%d", bits)
			c.Check(tf.GetOffsetFromRva(0x14F0) == 0x14F0, "truncated readable edge PE%d", bits)
			c.Check(tf.GetOffsetFromRva(0x1500) == ^uint32(0), "truncated cut point PE%d", bits)
			_, err = tf.GetData(0x1680, 8)
			c.Check(err != nil, "truncated zero fill unavailable PE%d", bits)

			// With LoadedImageBase left at zero the optional header ImageBase
			// is used, so an image fixed up to its preferred base parses too.
			fx0 := fxBuild(bits, headerBase, headerBase)
			f0, f0err := NewBytes(fx0.image, &Options{MemoryLayout: true})
			c.Check(f0err == nil && f0.Parse() == nil, "zero LoadedImageBase falls back to ImageBase PE%d", bits)
			if bits == 64 {
				cb0 := f0.TLS.Callbacks.([]uint64)
				c.Check(cb0[0] == headerBase+0x1180, "default-base TLS64 callback PE%d", bits)
				c.Check(f0.TLS.Struct.(ImageTLSDirectory64).AddressOfCallBacks == 0x1160,
					"default-base TLS64 RVA PE%d", bits)
			} else {
				cb0 := f0.TLS.Callbacks.([]uint32)
				c.Check(cb0[0] == uint32(headerBase+0x1180), "default-base TLS32 callback PE%d", bits)
				c.Check(f0.TLS.Struct.(ImageTLSDirectory32).AddressOfCallBacks == 0x1160,
					"default-base TLS32 RVA PE%d", bits)
			}
		}
	})
}

// TestMemoryLayoutDirectories covers TLS, both delay import forms, the debug
// payload coordinate switch and the file-only directories.
func TestMemoryLayoutDirectories(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		for _, bits := range []int{32, 64} {
			headerBase := uint64(0x400000)
			loadedBase := uint64(0x500000)
			if bits == 64 {
				headerBase = 0x140000000
				loadedBase = 0x150000000
			}
			fx := fxBuild(bits, headerBase, loadedBase)

			disk, diskInput := fxParse(t, fx.disk, fxDiskOpts())
			image, imageInput := fxParse(t, fx.image, fxImageOpts(loadedBase))

			// CodeView: disk follows PointerToRawData, image AddressOfRawData.
			c.Check(len(disk.Debugs) == 1 && len(image.Debugs) == 1, "debug count PE%d", bits)
			dCV, dOK := disk.Debugs[0].Info.(CVInfoPDB70)
			iCV, iOK := image.Debugs[0].Info.(CVInfoPDB70)
			c.Check(dOK && iOK, "CodeView type PE%d", bits)
			c.Check(dCV.Age == 7 && iCV.Age == 7, "CodeView age PE%d", bits)
			c.Check(dCV.PDBFileName == "t.pdb" && iCV.PDBFileName == "t.pdb",
				"CodeView pdb PE%d", bits)
			c.Check(image.Debugs[0].Struct.AddressOfRawData == 0x1240 &&
				image.Debugs[0].Struct.PointerToRawData == 0x440,
				"image debug directory fields preserved PE%d", bits)

			// A file coordinate pointing into a gap must not distract the image
			// parser, which addresses the payload through AddressOfRawData.
			mutated := append([]byte(nil), fx.image...)
			binary.LittleEndian.PutUint32(mutated[0x10A0+24:], 0x500)
			mf, merr := NewBytes(mutated, fxImageOpts(loadedBase))
			c.Check(merr == nil && mf.Parse() == nil, "mutated debug image parse PE%d", bits)
			mCV, mOK := mf.Debugs[0].Info.(CVInfoPDB70)
			c.Check(mOK && mCV.PDBFileName == "t.pdb", "image ignores file debug pointer PE%d", bits)

			// TLS: callbacks keep their VA output form, the mapped struct is
			// reduced to RVAs against the effective loaded base.
			c.Check(image.HasTLS && disk.HasTLS, "TLS present PE%d", bits)
			if bits == 64 {
				iCB, ok := image.TLS.Callbacks.([]uint64)
				dCB, dok := disk.TLS.Callbacks.([]uint64)
				c.Check(ok && dok && len(iCB) == 1 && len(dCB) == 1, "TLS64 callbacks PE%d", bits)
				c.Check(iCB[0] == loadedBase+0x1180, "image TLS64 callback VA PE%d", bits)
				c.Check(dCB[0] == headerBase+0x1180, "disk TLS64 callback VA PE%d", bits)
				iTLS := image.TLS.Struct.(ImageTLSDirectory64)
				c.Check(iTLS.StartAddressOfRawData == 0x1140 &&
					iTLS.EndAddressOfRawData == 0x1148 &&
					iTLS.AddressOfIndex == 0x1150 &&
					iTLS.AddressOfCallBacks == 0x1160, "image TLS64 struct RVAs PE%d", bits)
				dTLS := disk.TLS.Struct.(ImageTLSDirectory64)
				c.Check(dTLS.AddressOfCallBacks == headerBase+0x1160,
					"disk TLS64 struct keeps VA PE%d", bits)
			} else {
				iCB, ok := image.TLS.Callbacks.([]uint32)
				dCB, dok := disk.TLS.Callbacks.([]uint32)
				c.Check(ok && dok && len(iCB) == 1 && len(dCB) == 1, "TLS32 callbacks PE%d", bits)
				c.Check(iCB[0] == uint32(loadedBase+0x1180), "image TLS32 callback VA PE%d", bits)
				c.Check(dCB[0] == uint32(headerBase+0x1180), "disk TLS32 callback VA PE%d", bits)
				iTLS := image.TLS.Struct.(ImageTLSDirectory32)
				c.Check(iTLS.StartAddressOfRawData == 0x1140 &&
					iTLS.EndAddressOfRawData == 0x1148 &&
					iTLS.AddressOfCallBacks == 0x1160, "image TLS32 struct RVAs PE%d", bits)
			}

			// Ordinary RVA imports are independent of the loaded base.
			c.Check(len(disk.Imports) == 1 && len(image.Imports) == 1, "import count PE%d", bits)
			c.Check(disk.Imports[0].Name == "normal.dll" && image.Imports[0].Name == "normal.dll",
				"RVA import dll PE%d", bits)
			c.Check(len(image.Imports[0].Functions) == 1 &&
				image.Imports[0].Functions[0].Name == "NormalFunc", "RVA import func PE%d", bits)

			// Delay imports.
			if bits == 32 {
				c.Check(len(image.DelayImports) == 2 && len(disk.DelayImports) == 2,
					"PE32 delay import count")
				functions := map[string]ImportFunction{}
				for _, di := range image.DelayImports {
					c.Check(len(di.Functions) == 1, "PE32 delay %s one func", di.Name)
					functions[di.Name] = di.Functions[0]
				}
				fn, ok := functions["va.dll"]
				c.Check(ok && fn.Name == "VaDelayFunc", "PE32 VA delay import resolved")
				fn, ok = functions["rva.dll"]
				c.Check(ok && fn.Name == "RvaDelayFunc", "PE32 RVA delay import resolved")
				// The raw descriptor still carries the fixed-up VA value.
				c.Check(image.DelayImports[0].Descriptor.Name == uint32(loadedBase+0x15A0),
					"PE32 VA descriptor field preserved")
			} else {
				c.Check(len(image.DelayImports) == 1 && len(disk.DelayImports) == 1,
					"PE32+ delay import count")
				c.Check(image.DelayImports[0].Name == "rva.dll", "PE32+ RVA delay dll")
				c.Check(image.DelayImports[0].Functions[0].Name == "RvaDelayFunc",
					"PE32+ RVA delay func")
			}

			// Certificates, COFF symbols and overlay only exist on disk.
			c.Check(disk.HasCertificate, "disk reaches certificate header PE%d", bits)
			c.Check(!image.HasCertificate && image.Certificates.Header.Length == 0,
				"image skips certificate directory PE%d", bits)
			c.Check(disk.HasCOFF && len(disk.COFF.SymbolTable) == 1, "disk COFF symbols PE%d", bits)
			c.Check(!image.HasCOFF, "image has no COFF symbols PE%d", bits)

			ov, oerr := disk.Overlay()
			c.Check(oerr == nil && len(ov) == fxOverlayLen && string(ov) == "OVERLAY_DATA!!!!",
				"disk overlay PE%d", bits)
			c.Check(disk.OverlayLength() == fxOverlayLen, "disk overlay length PE%d", bits)
			_, ierr := image.Overlay()
			c.Check(errors.Is(ierr, ErrNoOverlayFound), "image overlay rejected PE%d", bits)
			c.Check(image.OverlayLength() == 0, "image overlay length PE%d", bits)

			// Read-only: neither input buffer changed.
			c.Check(bytes.Equal(fx.disk, diskInput), "disk input unchanged PE%d", bits)
			c.Check(bytes.Equal(fx.image, imageInput), "image input unchanged PE%d", bits)
			if bits == 64 {
				c.Check(image.NtHeader.OptionalHeader.(ImageOptionalHeader64).ImageBase == headerBase,
					"image ImageBase field preserved PE%d", bits)
			} else {
				c.Check(uint64(image.NtHeader.OptionalHeader.(ImageOptionalHeader32).ImageBase) == headerBase,
					"image ImageBase field preserved PE%d", bits)
			}
		}
	})
}

// TestMemoryLayoutReparse makes sure repeated Parse calls start from a clean
// state and never accumulate directory entries or touch the input.
func TestMemoryLayoutReparse(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		for _, bits := range []int{32, 64} {
			headerBase := uint64(0x400000)
			loadedBase := uint64(0x500000)
			if bits == 64 {
				headerBase = 0x140000000
				loadedBase = 0x150000000
			}
			fx := fxBuild(bits, headerBase, loadedBase)

			diskOriginal := append([]byte(nil), fx.disk...)
			df, err := NewBytes(fx.disk, fxDiskOpts())
			c.Check(err == nil, "disk NewBytes PE%d", bits)
			c.Check(df.Parse() == nil, "disk parse #1 PE%d", bits)
			firstDebugs, firstImports, firstDelay := len(df.Debugs), len(df.Imports), len(df.DelayImports)
			c.Check(df.Parse() == nil, "disk parse #2 PE%d", bits)
			c.Check(len(df.Debugs) == firstDebugs, "disk debugs no accumulation PE%d", bits)
			c.Check(len(df.Imports) == firstImports, "disk imports no accumulation PE%d", bits)
			c.Check(len(df.DelayImports) == firstDelay, "disk delay no accumulation PE%d", bits)
			c.Check(df.HasCOFF && len(df.COFF.SymbolTable) == 1, "disk COFF re-parsed PE%d", bits)
			c.Check(df.OverlayLength() == fxOverlayLen, "disk overlay re-parsed PE%d", bits)

			imageOriginal := append([]byte(nil), fx.image...)
			imf, err := NewBytes(fx.image, fxImageOpts(loadedBase))
			c.Check(err == nil, "image NewBytes PE%d", bits)
			c.Check(imf.Parse() == nil, "image parse #1 PE%d", bits)
			iDebugs, iImports, iDelay := len(imf.Debugs), len(imf.Imports), len(imf.DelayImports)
			c.Check(imf.Parse() == nil, "image parse #2 PE%d", bits)
			c.Check(len(imf.Debugs) == iDebugs, "image debugs no accumulation PE%d", bits)
			c.Check(len(imf.Imports) == iImports, "image imports no accumulation PE%d", bits)
			c.Check(len(imf.DelayImports) == iDelay, "image delay no accumulation PE%d", bits)
			c.Check(!imf.HasCOFF && !imf.HasCertificate, "image file-only dirs stay empty PE%d", bits)
			c.Check(imf.OverlayLength() == 0, "image overlay stays empty PE%d", bits)

			c.Check(bytes.Equal(fx.disk, diskOriginal), "disk input stable PE%d", bits)
			c.Check(bytes.Equal(fx.image, imageOriginal), "image input stable PE%d", bits)
		}
	})
}

// TestMemoryLayoutErrors covers unrepresentable bases, out-of-range addresses
// and file-only resources on a mapped image.
func TestMemoryLayoutErrors(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// PE32: base beyond the 32-bit VA width.
		fx32 := fxBuild(32, 0x400000, 0x500000)
		f, err := NewBytes(fx32.image, fxImageOpts(0x100000000))
		c.Check(err == nil, "PE32 NewBytes with wide base")
		c.Check(errors.Is(f.Parse(), ErrMemoryLayoutBase), "PE32 base > uint32 rejected")

		// PE32: base that overflows the 32-bit address space when SizeOfImage
		// is added.
		f, _ = NewBytes(fx32.image, fxImageOpts(0xFFFFF000))
		c.Check(errors.Is(f.Parse(), ErrMemoryLayoutBase), "PE32 base+image overflow rejected")

		// PE32+: base that overflows the 64-bit address space.
		fx64 := fxBuild(64, 0x140000000, 0x150000000)
		f, _ = NewBytes(fx64.image, fxImageOpts(0xFFFFFFFFFFFFF000))
		c.Check(errors.Is(f.Parse(), ErrMemoryLayoutBase), "PE32+ overflow base rejected")

		// Valid image for coordinate and VA conversion errors.
		image, _ := fxParse(t, fx32.image, fxImageOpts(0x500000))
		for _, rva := range []uint32{0x3000, 0x1F00, 0x500, 0x900} {
			c.Check(image.GetOffsetFromRva(rva) == ^uint32(0), "invalid RVA 0x%x marker", rva)
			c.Check(image.GetRVAFromOffset(rva) == ^uint32(0), "invalid offset 0x%x marker", rva)
			_, gerr := image.GetData(rva, 4)
			c.Check(gerr != nil, "GetData error at 0x%x", rva)
		}

		// VA below the loaded base and VA whose RVA exceeds 32 bits.
		_, vaErr := image.vaToRVA32(0x100000)
		c.Check(errors.Is(vaErr, ErrMemoryLayoutVA), "PE32 VA below base rejected")
		_, vaErr = image.vaToRVA64(0x400000)
		c.Check(errors.Is(vaErr, ErrMemoryLayoutVA), "VA below 64-bit base rejected")
		_, vaErr = image.vaToRVA64(0x500000 + 0x1_0000_0000)
		c.Check(errors.Is(vaErr, ErrMemoryLayoutVA), "RVA exceeding uint32 rejected")

		// A TLS callback VA fixed up against the wrong base cannot be reduced.
		// AddressOfCallBacks sits at TLS directory offset 12 (RVA 0x110C).
		wrong := append([]byte(nil), fx32.image...)
		binary.LittleEndian.PutUint32(wrong[0x110C:], 0x400000+0x1160)
		wf, _ := NewBytes(wrong, fxImageOpts(0x500000))
		c.Check(wf.Parse() == nil, "wrong-base image still parses")
		c.Check(errors.Is(wf.parseTLSDirectory(0x1100, 24), ErrMemoryLayoutVA),
			"mismatched TLS VA rejected")

		// Overlay and COFF remain unavailable in the mapped image.
		_, oerr := image.Overlay()
		c.Check(errors.Is(oerr, ErrNoOverlayFound), "overlay unavailable on image")
		c.Check(image.OverlayLength() == 0, "overlay length zero on image")
		c.Check(!image.HasCOFF, "COFF unavailable on image")

		// Truncated input: reads past the end fail without panicking.
		tf, _ := NewBytes(append([]byte(nil), fx32.image[:0x1200]...), fxImageOpts(0x500000))
		c.Check(tf.Parse() == nil, "truncated image parses")
		_, terr := tf.GetData(0x1240, 4)
		c.Check(terr != nil, "truncated payload read rejected")

		// Disk mode keeps its historic sentinel behavior.
		disk, _ := fxParse(t, fx32.disk, fxDiskOpts())
		c.Check(disk.GetOffsetFromRva(0x9000) == ^uint32(0), "disk unknown RVA still invalid")
	})
}

// ExampleNewBytes_memoryLayout shows a read-only parse of a loader-mapped
// image after rebasing: TLS VAs are reduced with LoadedImageBase while the
// header ImageBase and the section raw pointers stay untouched.
func ExampleNewBytes_memoryLayout() {
	fx := fxBuild(64, 0x140000000, 0x150000000)
	f, err := NewBytes(fx.image, &Options{MemoryLayout: true, LoadedImageBase: 0x150000000})
	if err != nil {
		panic(err)
	}
	if err := f.Parse(); err != nil {
		panic(err)
	}

	callbacks := f.TLS.Callbacks.([]uint64)
	tls := f.TLS.Struct.(ImageTLSDirectory64)
	fmt.Printf("callback=%#x rva=%#x pdb=%s delay=%s\n",
		callbacks[0], tls.AddressOfCallBacks,
		f.Debugs[0].Info.(CVInfoPDB70).PDBFileName, f.DelayImports[0].Name)
	// Output: callback=0x150001180 rva=0x1160 pdb=t.pdb delay=rva.dll
}

// ExampleNewBytes_diskAndImage shows the same content read through the two
// coordinate systems.
func ExampleNewBytes_diskAndImage() {
	fx := fxBuild(32, 0x400000, 0x500000)

	disk, _ := NewBytes(fx.disk, fxDiskOpts())
	if err := disk.Parse(); err != nil {
		panic(err)
	}
	image, _ := NewBytes(fx.image, fxImageOpts(0x500000))
	if err := image.Parse(); err != nil {
		panic(err)
	}

	d, _ := disk.GetData(0x1050, 4)
	i, _ := image.GetData(0x1050, 4)
	fmt.Printf("disk-offset=%#x image-index=%#x marker=%s/%s\n",
		disk.GetOffsetFromRva(0x1050), image.GetOffsetFromRva(0x1050), d, i)
	// Output: disk-offset=0x250 image-index=0x1050 marker=DATA/DATA
}
