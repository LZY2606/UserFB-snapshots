/* fmp.cpp

Copyright (c) 2026, LongSoft. All rights reserved.
This program and the accompanying materials
are licensed and made available under the terms and conditions of the BSD License
which accompanies this distribution.  The full text of the license may be found at
http://opensource.org/licenses/bsd-license.php

THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
WITHWARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

Read-only parsing of UEFI FMP (Firmware Management Protocol) capsules.
The item table is taken apart into driver and payload items, each payload's
update image is parsed with the normal FV/FFS/section machinery, and every
parsed image with its own Volume Top File gets an individual second pass.
Nothing here executes embedded drivers, verifies signatures, or modifies
the input in any way.
*/

#include "ffsparser.h"

#include <cstring>

#include "ffs.h"
#include "types.h"
#include "utility.h"

// EFI_FIRMWARE_MANAGEMENT_CAPSULE_HEADER, version 1
#define FMP_HEADER_SIZE              8

// EFI_FIRMWARE_MANAGEMENT_CAPSULE_IMAGE_HEADER sizes per header version
#define FMP_PAYLOAD_HEADER_SIZE_V1   32
#define FMP_PAYLOAD_HEADER_SIZE_V2   40
#define FMP_PAYLOAD_HEADER_SIZE_V3   48

// ImageCapsuleSupport bits that make the whole update image opaque for a read-only parser
#define FMP_IMAGE_CAPSULE_SUPPORT_AUTHENTICATION 0x1ULL
#define FMP_IMAGE_CAPSULE_SUPPORT_DEPENDENCY     0x2ULL

namespace {

// Unaligned little-endian field readers, no native structure padding is assumed
UINT16 fmpReadUint16(const char* p) { UINT16 v; std::memcpy(&v, p, sizeof(v)); return v; }
UINT32 fmpReadUint32(const char* p) { UINT32 v; std::memcpy(&v, p, sizeof(v)); return v; }
UINT64 fmpReadUint64(const char* p) { UINT64 v; std::memcpy(&v, p, sizeof(v)); return v; }

// Check that ancestor is on the parent chain of index, index itself included
bool fmpIsWithin(const UModelIndex & index, const UModelIndex & ancestor)
{
    for (UModelIndex current = index; current.isValid(); current = current.parent()) {
        if (current == ancestor)
            return true;
    }
    return false;
}

} // anonymous namespace

USTATUS FfsParser::parseFmpCapsuleBody(const UByteArray & capsule, const UINT32 localOffset,
                                       const UINT32 capsuleHeaderSize, const UINT32 capsuleImageSize,
                                       const UModelIndex & capsuleIndex)
{
    const UINT64 fmpBase = (UINT64)localOffset + capsuleHeaderSize;   // Absolute offset of the FMP header
    const UINT64 capsuleEnd = (UINT64)localOffset + capsuleImageSize; // Absolute declared end of the capsule
    const char* data = capsule.constData();

    // FMP header: Version u32, EmbeddedDriverCount u16, PayloadItemCount u16
    if (capsuleEnd < fmpBase + FMP_HEADER_SIZE) {
        msg(usprintf("%s: FMP header at %Xh is truncated by the declared capsule end", __FUNCTION__, (UINT32)fmpBase), capsuleIndex);
        return U_INVALID_CAPSULE;
    }

    const UINT32 fmpVersion = fmpReadUint32(data + capsuleHeaderSize);
    const UINT16 driverCount = fmpReadUint16(data + capsuleHeaderSize + 4);
    const UINT16 payloadCount = fmpReadUint16(data + capsuleHeaderSize + 6);
    const UINT32 itemCount = (UINT32)driverCount + payloadCount;
    const UINT64 tableSize = (UINT64)FMP_HEADER_SIZE + (UINT64)itemCount * sizeof(UINT64);

    // The whole item offset table must fit before the declared capsule end
    if (capsuleEnd < fmpBase + tableSize) {
        msg(usprintf("%s: FMP item count fields at %Xh declare a table that crosses the declared capsule end", __FUNCTION__, (UINT32)(fmpBase + 4)), capsuleIndex);
        return U_INVALID_CAPSULE;
    }

    // Read and validate item offsets, they are relative to the FMP header,
    // strictly increasing, at or beyond the end of the table, and before the declared capsule end
    std::vector<UINT64> itemOffsets(itemCount);
    for (UINT32 i = 0; i < itemCount; i++) {
        const UINT64 fieldOffset = fmpBase + FMP_HEADER_SIZE + (UINT64)i * sizeof(UINT64);
        const UINT64 itemOffset = fmpReadUint64(data + capsuleHeaderSize + FMP_HEADER_SIZE + (UINT64)i * sizeof(UINT64));
        if (itemOffset < tableSize) {
            msg(usprintf("%s: FMP item offset table entry at %Xh points into the item table itself", __FUNCTION__, (UINT32)fieldOffset), capsuleIndex);
            return U_INVALID_CAPSULE;
        }
        if (i > 0 && itemOffset <= itemOffsets[i - 1]) {
            msg(usprintf("%s: FMP item offset table entry at %Xh is not strictly increasing", __FUNCTION__, (UINT32)fieldOffset), capsuleIndex);
            return U_INVALID_CAPSULE;
        }
        if (fmpBase + itemOffset >= capsuleEnd) {
            msg(usprintf("%s: FMP item offset table entry at %Xh points at or beyond the declared capsule end", __FUNCTION__, (UINT32)fieldOffset), capsuleIndex);
            return U_INVALID_CAPSULE;
        }
        itemOffsets[i] = itemOffset;
    }

    // Add the FMP header tree item, covering the header and the whole item offset table
    {
        UString info = usprintf("Version: %u\nEmbedded drivers: %u\nPayload items: %u",
                                fmpVersion, driverCount, payloadCount);
        model->addItem(capsuleHeaderSize, Types::FmpItem, Subtypes::FmpHeaderItem,
                       UString("FMP header"), UString(), info,
                       capsule.mid(capsuleHeaderSize, (INT32)tableSize), UByteArray(), UByteArray(),
                       Fixed, capsuleIndex);
    }

    USTATUS firstImageError = U_SUCCESS;

    // Parse all items, drivers first, then payloads
    for (UINT32 i = 0; i < itemCount; i++) {
        const UINT64 itemStart = fmpBase + itemOffsets[i];
        const UINT64 itemEnd = (i + 1 < itemCount) ? fmpBase + itemOffsets[i + 1] : capsuleEnd;
        const UINT64 itemSize = itemEnd - itemStart;
        const UINT32 itemOffset = (UINT32)(itemStart - localOffset); // Offset relative to the capsule buffer

        // Embedded driver item, its bytes extend to the next item or to the declared capsule end
        if (i < driverCount) {
            model->addItem(itemOffset, Types::FmpItem, Subtypes::FmpDriverItem,
                           UString("FMP driver"), usprintf("Embedded driver %u", i + 1),
                           usprintf("Driver index: %u", i + 1),
                           UByteArray(), capsule.mid(itemOffset, (INT32)itemSize), UByteArray(),
                           Fixed, capsuleIndex);
            continue;
        }

        // Payload item, starts with a versioned header
        FmpPayloadInfo payload = {};
        payload.itemBase = (UINT32)itemStart;
        fmpPayloadVtfs.push_back(UModelIndex());

        // Even the Version field must fit
        if (itemSize < sizeof(UINT32)) {
            msg(usprintf("%s: FMP payload header at %Xh is too small to contain the Version field", __FUNCTION__, (UINT32)itemStart), capsuleIndex);
            fmpPayloads.push_back(payload);
            return U_INVALID_CAPSULE;
        }

        const UINT32 payloadVersion = fmpReadUint32(data + itemOffset);
        payload.version = payloadVersion;

        UINT32 payloadHeaderSize = 0;
        if (payloadVersion == 1)      payloadHeaderSize = FMP_PAYLOAD_HEADER_SIZE_V1;
        else if (payloadVersion == 2) payloadHeaderSize = FMP_PAYLOAD_HEADER_SIZE_V2;
        else if (payloadVersion == 3) payloadHeaderSize = FMP_PAYLOAD_HEADER_SIZE_V3;

        // Unknown header version: keep the entire item as opaque bytes, no guessed field reads
        if (payloadHeaderSize == 0) {
            payload.imageTypeId = UByteArray(16, '\0'); // Absent metadata is represented as zero
            payload.imageSize = (UINT32)itemSize;
            payload.imageBase = (UINT32)itemStart;
            payload.vendorBase = (UINT32)itemEnd;
            payload.item = model->addItem(itemOffset, Types::FmpItem, Subtypes::FmpPayloadItem,
                                          UString("FMP payload"), usprintf("Opaque, unsupported header version %u", payloadVersion),
                                          usprintf("Version: %u", payloadVersion),
                                          UByteArray(), capsule.mid(itemOffset, (INT32)itemSize), UByteArray(),
                                          Fixed, capsuleIndex);
            fmpPayloads.push_back(payload);
            continue;
        }

        if (itemSize < payloadHeaderSize) {
            msg(usprintf("%s: FMP payload header at %Xh crosses the boundary of its item", __FUNCTION__, (UINT32)itemStart), capsuleIndex);
            fmpPayloads.push_back(payload);
            return U_INVALID_CAPSULE;
        }

        // Read the versioned header fields
        payload.headerSize = payloadHeaderSize;
        payload.imageTypeId = UByteArray(data + itemOffset + 4, 16);
        payload.imageIndex = (UINT8)data[itemOffset + 20];
        const UINT32 updateImageSize = fmpReadUint32(data + itemOffset + 24);
        const UINT32 updateVendorCodeSize = fmpReadUint32(data + itemOffset + 28);
        if (payloadVersion >= 2)
            payload.hardwareInstance = fmpReadUint64(data + itemOffset + 32);
        if (payloadVersion >= 3)
            payload.imageCapsuleSupport = fmpReadUint64(data + itemOffset + 40);

        // Image and vendor bytes immediately follow the header in that order,
        // and must both fit in this item's physical boundary
        if ((UINT64)payloadHeaderSize + updateImageSize > itemSize) {
            msg(usprintf("%s: FMP payload UpdateImageSize field at %Xh declares an image that crosses the boundary of its item", __FUNCTION__, (UINT32)(itemStart + 24)), capsuleIndex);
            fmpPayloads.push_back(payload);
            return U_INVALID_CAPSULE;
        }
        if ((UINT64)payloadHeaderSize + updateImageSize + updateVendorCodeSize > itemSize) {
            msg(usprintf("%s: FMP payload UpdateVendorCodeSize field at %Xh declares vendor code that crosses the boundary of its item", __FUNCTION__, (UINT32)(itemStart + 28)), capsuleIndex);
            fmpPayloads.push_back(payload);
            return U_INVALID_CAPSULE;
        }

        payload.imageBase = (UINT32)(itemStart + payloadHeaderSize);
        payload.imageSize = updateImageSize;
        payload.vendorBase = payload.imageBase + updateImageSize;
        payload.vendorSize = updateVendorCodeSize;

        // Authentication or dependency support bits make the entire update image opaque,
        // known header metadata is preserved, but the image contents are not parsed
        const bool opaque = (payloadVersion == 3)
            && (payload.imageCapsuleSupport & (FMP_IMAGE_CAPSULE_SUPPORT_AUTHENTICATION | FMP_IMAGE_CAPSULE_SUPPORT_DEPENDENCY));

        // Compose item info
        EFI_GUID imageTypeGuid;
        std::memcpy(&imageTypeGuid, payload.imageTypeId.constData(), sizeof(EFI_GUID));
        UString info = usprintf("Version: %u\n", payloadVersion)
            + UString("Update image type ID: ") + guidToUString(imageTypeGuid, false)
            + usprintf("\nUpdate image index: %u", payload.imageIndex)
            + usprintf("\nUpdate image size: %Xh (%u)", updateImageSize, updateImageSize)
            + usprintf("\nUpdate vendor code size: %Xh (%u)", updateVendorCodeSize, updateVendorCodeSize);
        if (payloadVersion >= 2)
            info += usprintf("\nUpdate hardware instance: %" PRIX64 "h", payload.hardwareInstance);
        if (payloadVersion >= 3)
            info += usprintf("\nImage capsule support: %" PRIX64 "h", payload.imageCapsuleSupport);
        UString text = usprintf("Image index %u", payload.imageIndex);
        if (opaque)
            text = UString("Opaque, authentication or dependency processing required");

        // Add the payload item tree node, containing exactly header + image + vendor, excluding padding
        payload.item = model->addItem(itemOffset, Types::FmpItem, Subtypes::FmpPayloadItem,
                                      UString("FMP payload"), text, info,
                                      capsule.mid(itemOffset, payloadHeaderSize),
                                      capsule.mid(itemOffset + payloadHeaderSize, (INT32)(updateImageSize + updateVendorCodeSize)),
                                      UByteArray(), Fixed, capsuleIndex);

        if (!opaque && updateImageSize > 0) {
            // Parse the update image with the normal image parsing machinery
            const UModelIndex vtfBefore = lastVtf;
            UModelIndex imageIndex;
            USTATUS imageResult = parseImage(capsule.mid(itemOffset + payloadHeaderSize, (INT32)updateImageSize),
                                             payloadHeaderSize, payload.item, imageIndex);
            if (imageIndex.isValid()) {
                payload.image = imageIndex;
                // The last VTF found inside this very image belongs to this payload
                if (lastVtf.isValid() && lastVtf != vtfBefore && fmpIsWithin(lastVtf, imageIndex)) {
                    fmpPayloadVtfs.back() = lastVtf;
                }
            }
            if (imageResult != U_SUCCESS) {
                msg(usprintf("%s: FMP payload image at %Xh was not fully parsed", __FUNCTION__, payload.imageBase), payload.item);
                if (firstImageError == U_SUCCESS)
                    firstImageError = imageResult;
            }
        }

        // Vendor code bytes are kept as-is, never parsed as firmware
        if (updateVendorCodeSize > 0) {
            model->addItem(payloadHeaderSize + updateImageSize, Types::FmpItem, Subtypes::FmpVendorCodeItem,
                           UString("FMP vendor code"), UString(), UString(),
                           UByteArray(), capsule.mid(itemOffset + payloadHeaderSize + updateImageSize, (INT32)updateVendorCodeSize),
                           UByteArray(), Fixed, payload.item);
        }

        // Remaining item bytes are padding, they do not enlarge the image
        const UINT64 paddingSize = itemSize - payloadHeaderSize - updateImageSize - updateVendorCodeSize;
        if (paddingSize > 0) {
            const UINT32 paddingOffset = itemOffset + payloadHeaderSize + updateImageSize + updateVendorCodeSize;
            UByteArray padding = capsule.mid(paddingOffset, (INT32)paddingSize);
            model->addItem(paddingOffset, Types::Padding, getPaddingType(padding),
                           UString("Padding"), UString(), UString(),
                           UByteArray(), padding, UByteArray(), Fixed, capsuleIndex);
        }

        fmpPayloads.push_back(payload);
    }

    return firstImageError;
}

USTATUS FfsParser::performFmpSecondPass()
{
    // Every parsed payload image with an uncompressed last VTF of its own
    // gets an individual second pass and an individual CPU address mapping
    for (size_t i = 0; i < fmpPayloads.size(); i++) {
        const UModelIndex vtf = fmpPayloadVtfs[i];
        if (!vtf.isValid())
            continue;

        // Check for compressed VTF
        if (model->compressed(vtf)) {
            msg(usprintf("%s: the last VTF appears inside compressed item", __FUNCTION__), vtf);
            continue;
        }

        FmpPayloadInfo & payload = fmpPayloads[i];
        if (!payload.image.isValid())
            continue;

        // Calculate the address difference from this payload's own VTF
        lastVtf = vtf;
        addressDiff = 0x100000000ULL - model->base(vtf) - model->fullSize(vtf);

        // Parse reset vector data of this payload's VTF
        parseResetVectorData();

        // Check TE images of this payload against its own mapping
        checkTeImageBase(payload.image);

        payload.hasCpuMapping = true;
        payload.addressDiff = addressDiff;
        indexesAddressDiffs.push_back(std::pair<UModelIndex, UINT64>(payload.image, addressDiff));
    }

    // The capsule as a whole has no single CPU address mapping,
    // so unmapped payload images and non-image nodes get no CPU address text
    addressDiff = 0x100000000ULL;

    return U_SUCCESS;
}
