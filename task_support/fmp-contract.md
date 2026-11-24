FMP read-only model contract

Reference: UEFI 2.10, section 23.3, https://uefi.org/specs/UEFI/2.10/23_Firmware_Update_and_Reporting.html . This task supports ordinary uncompressed generic FV images as update images, not an update dispatcher.

The outer EFI_CAPSULE_HEADER is GUID[16], HeaderSize u32, Flags u32, CapsuleImageSize u32. Its header may exceed 28 bytes. Only the declared image belongs to the capsule. The FMP header begins at HeaderSize: Version u32 (1), EmbeddedDriverCount u16, PayloadItemCount u16, followed by that many u64 item offsets, all little endian. Offsets are relative to this FMP header, strictly increasing, at or beyond the end of the table, and before the declared capsule end. Natural alignment is not guaranteed. Drivers precede payloads. Driver bytes extend to the next item (or capsule end).

A payload header is Version u32, UpdateImageTypeId GUID[16], UpdateImageIndex u8, reserved[3], UpdateImageSize u32, UpdateVendorCodeSize u32, then UpdateHardwareInstance u64 for v2+, ImageCapsuleSupport u64 for v3. Header sizes are 32/40/48. No native structure padding may be assumed. Fields absent in an older version are represented as zero. Index is nonzero. Image and vendor bytes immediately follow the header in that order and must both fit in this item's physical boundary. Remaining item bytes are padding; they do not enlarge the image. Unsupported versions or support bits are identifiable opaque items with their declared bytes, not inferred firmware. For v3, support bits 1 (authentication) or 2 (dependency) make the entire update image opaque. No authentication processing is required.

Public API to add in common/ffsparser.h:

struct FmpPayloadInfo {
    UModelIndex item;       // node containing exactly header + image + vendor, excluding padding
    UModelIndex image;      // child Types::Image for a parsed update image, invalid for opaque data
    UINT32 version;
    UByteArray imageTypeId; // exactly the 16 on-disk bytes
    UINT8 imageIndex;
    UINT64 hardwareInstance, imageCapsuleSupport;
    UINT32 itemBase, headerSize, imageBase, imageSize, vendorBase, vendorSize;
    bool hasCpuMapping;
    UINT64 addressDiff;
};
const std::vector<FmpPayloadInfo>& FfsParser::getFmpPayloads() const;

Return entries in payload table order, preserving duplicate identities. Base fields are absolute offsets into the outer input. item/image are actual nodes in the existing TreeModel, not a second synthesized tree. Nodes remain subject to the normal model lifetime; the vector is refreshed at each parse. New item type/subtype names are implementation choices, but report names must distinguish driver, payload and vendor code. Every represented node's full()/base() must describe its real contiguous input bytes. item holds its versioned header; child parsed image contains image bytes only. vendor bytes and padding have no firmware descendants.

For a parsed payload with an uncompressed last VTF, addressDiff is 2^32 - (absolute base of that VTF + its full size); hasCpuMapping=true. The original reset-vector and TE second-pass interpretation must use that payload's VTF and image base. TreeModel info addresses of uncompressed image descendants use this difference. Nodes outside that image, including its item wrapper, vendor and unrelated siblings, do not inherit this mapping. For no VTF, hasCpuMapping=false and addressDiff=0, and no CPU address text is emitted for that payload's descendants. getAddressDiff() keeps its legacy meaning for bare single images. getIndexesAddressDiffs() includes each mapped FMP image paired with its difference; its consumer must use image ancestry, not just a lower-bound base comparison. Existing compressed-node suppression still applies.

Recognized FMP table/size failures return U_INVALID_CAPSULE with a parser message identifying the absolute offending table/header field offset. Do not silently reinterpret the input as a bare generic image. Unknown header versions preserve the entire item as opaque (headerSize=0, image points invalid, all bytes as opaque image range, absent metadata zero); no guessed version-dependent header reads. v3 support flags preserve known header metadata but do not parse image contents. Existing non-FMP parsing behavior remains compatible.

The fixture samples/two-images.cap is deterministic output from baseline.cpp: capsule header 32; FMP table 24; first item at 56, first image at 88 (512 bytes), second item at 600, second image at 640 (768 bytes), all offsets absolute. First header v1, second v2 (hardwareInstance 0x1234); both volumes end with a VTF. Expected image-specific differences are 0xfffffda8 and 0xfffffa80. The current parser creates only one image and applies the latter difference to both volumes. baseline prints this observation without claiming the future behavior already exists.
