package objcreader

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"strings"
	"unsafe"

	"github.com/blacktop/go-macho/types"
	"github.com/blacktop/go-macho/types/objc"
)

// GetObjCCategories returns the Objective-C categories in __objc_catlist.
func (f *Reader) GetObjCCategories() ([]objc.Category, error) {
	if err := f.ensureObjCNonFragileRuntime("GetObjCCategories"); err != nil {
		return nil, err
	}
	var categories []objc.Category
	for _, name := range f.SegmentNames() {
		if !strings.HasPrefix(name, "__DATA") {
			continue
		}
		sec := f.Section(name, "__objc_catlist")
		if sec == nil {
			continue
		}
		ptrs, err := f.readSectionPointers(sec)
		if err != nil {
			return nil, err
		}
		for _, ptr := range ptrs {
			// Fixup lookups inside the category use the field slot addresses,
			// never the encoded value stored in this list slot.
			category, err := f.readObjCCategory(ptr)
			if err != nil {
				return nil, err
			}
			categories = append(categories, *category)
		}
	}
	return categories, nil
}

// readSectionPointers reads a section that holds an array of raw pointers.
func (f *Reader) readSectionPointers(sec *types.Section) ([]uint64, error) {
	if err := f.DataReader().SeekToAddr(sec.Addr); err != nil {
		return nil, fmt.Errorf("failed to seek to %s addr %#x: %v", sec.Name, sec.Addr, err)
	}
	data := make([]byte, sec.Size)
	if err := binary.Read(f.DataReader(), f.ByteOrder(), data); err != nil {
		return nil, fmt.Errorf("failed to read %s.%s data: %v", sec.Seg, sec.Name, err)
	}
	ptrs := make([]uint64, sec.Size/f.PointerSize())
	if err := binary.Read(bytes.NewReader(data), f.ByteOrder(), &ptrs); err != nil {
		return nil, fmt.Errorf("failed to read %s.%s pointers: %v", sec.Seg, sec.Name, err)
	}
	return ptrs, nil
}

// readObjCCategory reads the category_t selected by rawPtr, which is the raw
// content of a __objc_catlist slot. Fixup lookups use the slot source
// locations rather than the encoded slot values.
func (f *Reader) readObjCCategory(rawPtr uint64) (*objc.Category, error) {
	vmaddr := f.ConvertPointer(rawPtr)
	offset, err := f.GetOffset(vmaddr)
	if err != nil {
		return nil, fmt.Errorf("failed to convert vmaddr: %v", err)
	}
	if _, err := f.DataReader().Seek(int64(offset), io.SeekStart); err != nil {
		return nil, fmt.Errorf("failed to seek to category_t at %#x: %v", vmaddr, err)
	}
	var ptr objc.CategoryT
	if err := binary.Read(f.DataReader(), f.ByteOrder(), &ptr); err != nil {
		return nil, fmt.Errorf("failed to read %T: %v", ptr, err)
	}
	category := &objc.Category{VMAddr: vmaddr}
	ptr.NameVMAddr = f.ConvertPointer(ptr.NameVMAddr)
	category.Name, err = f.GetCString(ptr.NameVMAddr)
	if err != nil {
		return nil, fmt.Errorf("failed to read cstring: %v", err)
	}
	hasClass := ptr.ClsVMAddr != 0
	classSlotAddr := category.VMAddr + uint64(unsafe.Offsetof(ptr.ClsVMAddr))
	classRaw := ptr.ClsVMAddr
	if hasClass {
		ptr.ClsVMAddr = f.ConvertPointer(ptr.ClsVMAddr)
	}
	category.Class, err = f.objcCategoryClass(classSlotAddr, classRaw, ptr.ClsVMAddr, hasClass)
	if err != nil {
		return nil, err
	}
	if err := f.readObjCCategoryContents(category, &ptr); err != nil {
		return nil, err
	}
	category.CategoryT = ptr
	return category, nil
}

func (f *Reader) readObjCCategoryContents(category *objc.Category, ptr *objc.CategoryT) error {
	if err := f.readObjCCategoryMethods(category, ptr); err != nil {
		return err
	}
	var err error
	if ptr.ProtocolsVMAddr > 0 {
		ptr.ProtocolsVMAddr = f.ConvertPointer(ptr.ProtocolsVMAddr)
		category.Protocols, err = f.parseObjcProtocolList(ptr.ProtocolsVMAddr)
		if err != nil {
			return fmt.Errorf("failed to read protocols vmaddr: %v", err)
		}
	}
	if ptr.InstancePropertiesVMAddr > 0 {
		ptr.InstancePropertiesVMAddr = f.ConvertPointer(ptr.InstancePropertiesVMAddr)
		allowSwift := category.Class != nil && category.Class.IsSwift()
		category.Properties, err = f.getObjCPropertiesWithSwift(
			ptr.InstancePropertiesVMAddr, allowSwift)
		if err != nil {
			return fmt.Errorf("failed to get instance properties at vmaddr: %#x; %v",
				ptr.InstancePropertiesVMAddr, err)
		}
	}
	return nil
}

func (f *Reader) readObjCCategoryMethods(category *objc.Category, ptr *objc.CategoryT) error {
	var err error
	if ptr.InstanceMethodsVMAddr > 0 {
		ptr.InstanceMethodsVMAddr = f.ConvertPointer(ptr.InstanceMethodsVMAddr)
		category.InstanceMethods, err = f.GetObjCMethods(ptr.InstanceMethodsVMAddr)
		if err != nil {
			return fmt.Errorf("failed to get instance methods at vmaddr: %#x; %v",
				ptr.InstanceMethodsVMAddr, err)
		}
	}
	if ptr.ClassMethodsVMAddr > 0 {
		ptr.ClassMethodsVMAddr = f.ConvertPointer(ptr.ClassMethodsVMAddr)
		category.ClassMethods, err = f.GetObjCMethods(ptr.ClassMethodsVMAddr)
		if err != nil {
			return fmt.Errorf("failed to get class methods at vmaddr: %#x; %v",
				ptr.ClassMethodsVMAddr, err)
		}
	}
	return nil
}

// objcCategoryClass resolves the category's host class. classSlotAddr is the
// source address of the category_t Class field; an external class is a bind at
// that slot rather than a readable class record. classAddr is the converted
// field value and classRaw its raw, encoded value.
func (f *Reader) objcCategoryClass(
	classSlotAddr, classRaw, classAddr uint64, hasClass bool,
) (*objc.Class, error) {
	if !hasClass {
		return f.objcCategoryBind(classSlotAddr, nil)
	}
	if value, ok := f.GetObjC(classAddr); ok {
		return value.(*objc.Class), nil
	}
	class, err := f.GetObjCClass(classAddr)
	if err == nil {
		f.PutObjC(classAddr, class)
		return class, nil
	}
	if !f.HasFixups() {
		return &objc.Class{}, nil
	}
	// Never treat the encoded field value as a class address or bind slot:
	// the bind is located by the field's own source address.
	return f.objcCategoryBind(classSlotAddr, errors.Join(err, fmt.Errorf("raw class field %#x", classRaw)))
}

func (f *Reader) objcCategoryBind(addr uint64, classErr error) (*objc.Class, error) {
	name, err := f.GetBindName(addr)
	if err == nil {
		return &objc.Class{Name: strings.TrimPrefix(name, "_OBJC_CLASS_$_")}, nil
	}
	if f.BindInfoUnavailable(err) {
		return &objc.Class{}, nil
	}
	if classErr == nil {
		return nil, fmt.Errorf("failed to read super class objc_class_t at vmaddr: %#x; %v",
			addr, err)
	}
	return nil, fmt.Errorf("failed to read super class objc_class_t at vmaddr: %#x; %w",
		addr, errors.Join(classErr, err))
}

// GetObjCNonLazyCategories returns an array of Objective-C classes that implement +load
func (f *Reader) GetObjCNonLazyCategories() ([]objc.Category, error) {
	if err := f.ensureObjCNonFragileRuntime("GetObjCNonLazyCategories"); err != nil {
		return nil, err
	}

	var cats []objc.Category

	for _, s := range f.SegmentNames() {
		if strings.HasPrefix(s, "__DATA") {
			// An array of pointers to classes who implement +load
			if sec := f.Section(s, "__objc_nlcatlist"); sec != nil {
				ptrs, err := f.readSectionPointers(sec)
				if err != nil {
					return nil, err
				}
				for _, rawPtr := range ptrs {
					vmaddr := f.ConvertPointer(rawPtr)
					if c, ok := f.GetObjC(vmaddr); ok {
						cats = append(cats, *c.(*objc.Category))
						continue
					}
					cat, err := f.readObjCCategory(rawPtr)
					if err != nil {
						return nil, fmt.Errorf("failed to read non-lazy category_t at vmaddr %#x: %v", vmaddr, err)
					}
					cats = append(cats, *cat)
					f.PutObjC(vmaddr, cat)
				}
			}
		}
	}

	return cats, nil
}
