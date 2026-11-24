/* fmp_contract.cpp

Contract tests for read-only UEFI FMP capsule parsing.
Built fixtures cover FMP image header versions 1/2/3, non-aligned items,
two VTF images of different lengths, a sibling image without VTF, vendor
code containing a complete FV, malformed item tables, and plain firmware.
*/

#include "task_support/check.h"

#include "common/ffsparser.h"
#include "common/ffs.h"
#include "common/parsingdata.h"
#include "common/peimage.h"

#include <cstring>
#include <vector>
#include <fstream>

typedef std::vector<unsigned char> Bytes;

// ---------------------------------------------------------------------------
// Fixture builders
// ---------------------------------------------------------------------------

static void put(Bytes& b, size_t p, unsigned long long v, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        b.at(p + i) = static_cast<unsigned char>(v >> (8 * i));
}

static void setGuid(Bytes& b, size_t p, const UByteArray& g)
{
    std::memcpy(b.data() + p, g.constData(), 16);
}

static UByteArray makeGuid(unsigned seed)
{
    UByteArray g(16, '\0');
    for (int i = 0; i < 16; i++)
        g[i] = (char)(seed + i);
    return g;
}

// FFS file with a valid header checksum, attributes and alignment of 1
static Bytes ffsFile(const UByteArray& guid, unsigned type, const Bytes& body)
{
    Bytes f(24 + body.size(), 0);
    setGuid(f, 0, guid);
    f[17] = 0xaa; // Data checksum, not verified by the parser for raw files
    f[18] = (unsigned char)type;
    put(f, 20, f.size(), 3);
    f[23] = 0xf8; // State
    if (!body.empty())
        std::memcpy(f.data() + 24, body.data(), body.size());
    unsigned sum = 0;
    for (size_t i = 0; i < 24; ++i)
        if (i != 17 && i != 23)
            sum += f[i];
    f[16] = (unsigned char)(0u - sum);
    return f;
}

static Bytes vtfFile(size_t bodySize)
{
    return ffsFile(EFI_FFS_VOLUME_TOP_FILE_GUID, EFI_FV_FILETYPE_RAW, Bytes(bodySize, 0));
}

static Bytes rawFile(size_t bodySize)
{
    return ffsFile(makeGuid(0x75), EFI_FV_FILETYPE_RAW, Bytes(bodySize, 0));
}

// FFSv2 volume with the given files packed right after the 72-byte header
static Bytes volumeWithFiles(const std::vector<Bytes>& files)
{
    size_t body = 0;
    for (size_t i = 0; i < files.size(); ++i)
        body += files[i].size();
    Bytes b(72 + body, 0);
    size_t n = b.size();
    setGuid(b, 16, EFI_FIRMWARE_FILE_SYSTEM2_GUID);
    put(b, 32, n, 8);
    put(b, 40, 0x4856465f, 4); // '_FVH'
    put(b, 44, 0x800, 4);      // EFI_FVB2_ERASE_POLARITY
    put(b, 48, 72, 2);
    b[55] = 2;                 // Revision
    put(b, 56, 1, 4);          // NumBlocks
    put(b, 60, n, 4);          // BlockMap[0].Length
    unsigned sum = 0;
    for (size_t i = 0; i < 72; i += 2)
        sum += b[i] + (b[i + 1] << 8);
    put(b, 50, (0u - sum) & 0xffff, 2);
    size_t p = 72;
    for (size_t i = 0; i < files.size(); ++i) {
        std::memcpy(b.data() + p, files[i].data(), files[i].size());
        p += files[i].size();
    }
    return b;
}

// Volume holding a single VTF that ends at the volume end
static Bytes vtfVolume(size_t n)
{
    return volumeWithFiles({vtfFile(n - 72 - 24)});
}

// Volume holding a single non-VTF raw file
static Bytes rawVolume(size_t n)
{
    return volumeWithFiles({rawFile(n - 72 - 24)});
}

struct PayloadSpec {
    unsigned version;
    UByteArray imageTypeId;
    unsigned imageIndex;
    unsigned long long hwInstance;
    unsigned long long support;
    Bytes image;    // Update image bytes; raw item bytes for unknown header versions
    Bytes vendor;   // Vendor code bytes
    unsigned padAfter; // Extra padding inside the item, after the vendor code
    PayloadSpec() : version(1), imageTypeId(16, '\0'), imageIndex(1), hwInstance(0), support(0), padAfter(0) {}
};

static unsigned payloadHeaderSize(unsigned version)
{
    if (version == 1) return 32;
    if (version == 2) return 40;
    if (version == 3) return 48;
    return 0;
}

struct BuiltFmp {
    Bytes bytes;
    size_t fmpBase;
    std::vector<size_t> itemOffsets; // Absolute item offsets
};

static BuiltFmp buildFmpCapsule(unsigned capsuleHeaderSize,
                                const std::vector<Bytes>& drivers,
                                const std::vector<PayloadSpec>& payloads)
{
    BuiltFmp out;
    size_t itemCount = drivers.size() + payloads.size();
    size_t tableSize = 8 + 8 * itemCount;
    size_t fmpBase = capsuleHeaderSize;

    std::vector<Bytes> itemContents = drivers;
    for (size_t i = 0; i < payloads.size(); ++i) {
        const PayloadSpec& p = payloads[i];
        unsigned hs = payloadHeaderSize(p.version);
        Bytes item;
        if (hs == 0) {
            item = p.image; // Unknown header version: raw opaque item bytes
        }
        else {
            item.resize(hs, 0);
            put(item, 0, p.version, 4);
            std::memcpy(item.data() + 4, p.imageTypeId.constData(), 16);
            item[20] = (unsigned char)p.imageIndex;
            put(item, 24, p.image.size(), 4);
            put(item, 28, p.vendor.size(), 4);
            if (p.version >= 2) put(item, 32, p.hwInstance, 8);
            if (p.version >= 3) put(item, 40, p.support, 8);
            item.insert(item.end(), p.image.begin(), p.image.end());
            item.insert(item.end(), p.vendor.begin(), p.vendor.end());
        }
        item.resize(item.size() + p.padAfter, 0);
        itemContents.push_back(item);
    }

    size_t total = fmpBase + tableSize;
    for (size_t i = 0; i < itemContents.size(); ++i)
        total += itemContents[i].size();

    out.bytes.assign(capsuleHeaderSize, 0);
    setGuid(out.bytes, 0, EFI_FMP_CAPSULE_GUID);
    put(out.bytes, 16, capsuleHeaderSize, 4);
    put(out.bytes, 20, 0, 4);     // Flags
    put(out.bytes, 24, total, 4); // CapsuleImageSize

    out.bytes.resize(fmpBase + tableSize, 0);
    put(out.bytes, fmpBase, 1, 4);                       // FMP header Version
    put(out.bytes, fmpBase + 4, (unsigned)drivers.size(), 2);
    put(out.bytes, fmpBase + 6, (unsigned)payloads.size(), 2);

    size_t pos = fmpBase + tableSize;
    for (size_t i = 0; i < itemContents.size(); ++i) {
        put(out.bytes, fmpBase + 8 + 8 * i, pos - fmpBase, 8);
        out.itemOffsets.push_back(pos);
        out.bytes.insert(out.bytes.end(), itemContents[i].begin(), itemContents[i].end());
        pos += itemContents[i].size();
    }
    out.fmpBase = fmpBase;
    return out;
}

// The deterministic fixture also stored in task_support/samples/two-images.cap:
// capsule header 32, FMP table 24, item1 at 56, image1 at 88 (512 bytes),
// item2 at 600, image2 at 640 (768 bytes), header versions 1 and 2
static Bytes fixtureCapsule()
{
    std::vector<PayloadSpec> payloads(2);
    payloads[0].version = 1;
    payloads[0].imageIndex = 1;
    payloads[0].image = vtfVolume(512);
    payloads[1].version = 2;
    payloads[1].imageIndex = 2;
    payloads[1].hwInstance = 0x1234;
    payloads[1].image = vtfVolume(768);
    return buildFmpCapsule(32, std::vector<Bytes>(), payloads).bytes;
}

// ---------------------------------------------------------------------------
// Tree helpers
// ---------------------------------------------------------------------------

static USTATUS parseBytes(TreeModel& m, FfsParser& p, const Bytes& b)
{
    return p.parse(UByteArray(reinterpret_cast<const char*>(b.data()), (int32_t)b.size()));
}

static void collectType(TreeModel& m, const UModelIndex& parent, unsigned type, std::vector<UModelIndex>& out)
{
    for (int i = 0; i < m.rowCount(parent); ++i) {
        UModelIndex x = m.index(i, 0, parent);
        if (m.type(x) == type)
            out.push_back(x);
        collectType(m, x, type, out);
    }
}

static unsigned countType(TreeModel& m, const UModelIndex& parent, unsigned type)
{
    unsigned n = 0;
    for (int i = 0; i < m.rowCount(parent); ++i) {
        UModelIndex x = m.index(i, 0, parent);
        if (m.type(x) == type)
            ++n;
        n += countType(m, x, type);
    }
    return n;
}

static bool infoHas(TreeModel& m, const UModelIndex& idx, const char* text)
{
    return m.info(idx).find(text) >= 0;
}

static bool hasFmpErrorAt(FfsParser& p, const char* offsetText)
{
    std::vector<std::pair<UString, UModelIndex> > msgs = p.getMessages();
    for (size_t i = 0; i < msgs.size(); ++i) {
        if (msgs[i].first.find("FMP") >= 0 && msgs[i].first.find(offsetText) >= 0)
            return true;
    }
    return false;
}

static UString hex8(unsigned long long v)
{
    return usprintf("%08Xh", (UINT32)v);
}

// ---------------------------------------------------------------------------
// items group
// ---------------------------------------------------------------------------

static int case_fixture_two_images(void)
{
    Bytes bytes = fixtureCapsule();
    TASK_CHECK(bytes.size() == 1408);

    TreeModel m;
    FfsParser p(&m);
    TASK_CHECK(parseBytes(m, p, bytes) == U_SUCCESS);

    // One capsule root, no fallback image
    TASK_CHECK(m.rowCount(UModelIndex()) == 1);
    UModelIndex root = m.index(0, 0, UModelIndex());
    TASK_CHECK(m.type(root) == Types::Capsule);
    TASK_CHECK(m.subtype(root) == Subtypes::FmpCapsule);

    // Two payload entries in table order, same zero ImageTypeId not merged
    const std::vector<FmpPayloadInfo>& payloads = p.getFmpPayloads();
    TASK_CHECK(payloads.size() == 2);

    TASK_CHECK(payloads[0].version == 1);
    TASK_CHECK(payloads[0].itemBase == 56);
    TASK_CHECK(payloads[0].headerSize == 32);
    TASK_CHECK(payloads[0].imageBase == 88);
    TASK_CHECK(payloads[0].imageSize == 512);
    TASK_CHECK(payloads[0].vendorBase == 600);
    TASK_CHECK(payloads[0].vendorSize == 0);
    TASK_CHECK(payloads[0].imageIndex == 1);
    TASK_CHECK(payloads[0].hardwareInstance == 0);
    TASK_CHECK(payloads[0].imageCapsuleSupport == 0);
    TASK_CHECK(payloads[0].imageTypeId.size() == 16);
    TASK_CHECK(payloads[0].imageTypeId == UByteArray(16, '\0'));
    TASK_CHECK(payloads[0].hasCpuMapping);
    TASK_CHECK(payloads[0].addressDiff == 0xfffffda8ULL);

    TASK_CHECK(payloads[1].version == 2);
    TASK_CHECK(payloads[1].itemBase == 600);
    TASK_CHECK(payloads[1].headerSize == 40);
    TASK_CHECK(payloads[1].imageBase == 640);
    TASK_CHECK(payloads[1].imageSize == 768);
    TASK_CHECK(payloads[1].vendorBase == 1408);
    TASK_CHECK(payloads[1].vendorSize == 0);
    TASK_CHECK(payloads[1].imageIndex == 2);
    TASK_CHECK(payloads[1].hardwareInstance == 0x1234);
    TASK_CHECK(payloads[1].imageCapsuleSupport == 0);
    TASK_CHECK(payloads[1].imageTypeId == payloads[0].imageTypeId);
    TASK_CHECK(payloads[1].hasCpuMapping);
    TASK_CHECK(payloads[1].addressDiff == 0xfffffa80ULL);

    // Item and image nodes are real tree nodes over the real input bytes
    TASK_CHECK(payloads[0].item.isValid());
    TASK_CHECK(payloads[0].image.isValid());
    TASK_CHECK(m.base(payloads[0].item) == 56);
    TASK_CHECK(m.fullSize(payloads[0].item) == 32 + 512);
    TASK_CHECK(m.type(payloads[0].image) == Types::Image);
    TASK_CHECK(m.base(payloads[0].image) == 88);
    TASK_CHECK(m.fullSize(payloads[0].image) == 512);
    TASK_CHECK(m.base(payloads[1].item) == 600);
    TASK_CHECK(m.fullSize(payloads[1].item) == 40 + 768);
    TASK_CHECK(m.base(payloads[1].image) == 640);
    TASK_CHECK(m.fullSize(payloads[1].image) == 768);
    TASK_CHECK(payloads[0].image != payloads[1].image);

    // Each payload image holds its own volume and VTF file
    TASK_CHECK(countType(m, UModelIndex(), Types::Image) == 2);
    TASK_CHECK(countType(m, UModelIndex(), Types::Volume) == 2);
    TASK_CHECK(countType(m, UModelIndex(), Types::File) == 2);
    TASK_CHECK(countType(m, payloads[0].image, Types::Volume) == 1);
    TASK_CHECK(countType(m, payloads[1].image, Types::Volume) == 1);
    return 0;
}

static int case_header_versions_and_alignment(void)
{
    // Layout, all offsets absolute and deliberately not naturally aligned:
    //   capsule header [0, 0x20), FMP header + 4-entry table [0x20, 0x48)
    //   driver          [0x48, 0x69)   33 raw bytes
    //   payload v1      [0x69, 0x289)  32-byte header + 0x200 image
    //   payload v2      [0x289, 0x634) 40-byte header + 0x300 image + 0x80 vendor + 3 padding
    //   payload v3      [0x634, 0x7E4) 48-byte header + 0x180 image
    std::vector<Bytes> drivers(1, Bytes(0x21, 0x5A));

    Bytes vendorFv = vtfVolume(0x80); // A complete FV hiding inside vendor code

    std::vector<PayloadSpec> payloads(3);
    payloads[0].version = 1;
    payloads[0].imageIndex = 1;
    payloads[0].imageTypeId = makeGuid(0x10);
    payloads[0].image = vtfVolume(0x200);
    payloads[1].version = 2;
    payloads[1].imageIndex = 2;
    payloads[1].imageTypeId = makeGuid(0x20);
    payloads[1].hwInstance = 0xABCD;
    payloads[1].image = vtfVolume(0x300);
    payloads[1].vendor = vendorFv;
    payloads[1].padAfter = 3;
    payloads[2].version = 3;
    payloads[2].imageIndex = 3;
    payloads[2].imageTypeId = makeGuid(0x10); // Same ImageTypeId as the first payload
    payloads[2].hwInstance = 0x777;
    payloads[2].support = 0; // No authentication/dependency: parsed normally
    payloads[2].image = rawVolume(0x180);

    BuiltFmp cap = buildFmpCapsule(0x20, drivers, payloads);
    TASK_CHECK(cap.bytes.size() == 0x7E4);
    TASK_CHECK(cap.itemOffsets.size() == 4);
    TASK_CHECK(cap.itemOffsets[0] == 0x48);
    TASK_CHECK(cap.itemOffsets[1] == 0x69);
    TASK_CHECK(cap.itemOffsets[2] == 0x289);
    TASK_CHECK(cap.itemOffsets[3] == 0x634);

    TreeModel m;
    FfsParser p(&m);
    TASK_CHECK(parseBytes(m, p, cap.bytes) == U_SUCCESS);

    // Driver node covers everything up to the next item
    std::vector<UModelIndex> fmpItems;
    collectType(m, UModelIndex(), Types::FmpItem, fmpItems);
    UModelIndex capsule = m.index(0, 0, UModelIndex());
    UModelIndex fmpHeader = m.index(0, 0, capsule);
    UModelIndex driver = m.index(1, 0, capsule);
    TASK_CHECK(m.type(fmpHeader) == Types::FmpItem);
    TASK_CHECK(m.subtype(fmpHeader) == Subtypes::FmpHeaderItem);
    TASK_CHECK(m.base(fmpHeader) == 0x20);
    TASK_CHECK(m.fullSize(fmpHeader) == 0x28);
    TASK_CHECK(m.type(driver) == Types::FmpItem);
    TASK_CHECK(m.subtype(driver) == Subtypes::FmpDriverItem);
    TASK_CHECK(m.base(driver) == 0x48);
    TASK_CHECK(m.fullSize(driver) == 0x21);
    TASK_CHECK(m.full(driver) == UByteArray((const char*)cap.bytes.data() + 0x48, 0x21));

    const std::vector<FmpPayloadInfo>& info = p.getFmpPayloads();
    TASK_CHECK(info.size() == 3);

    // v1 payload
    TASK_CHECK(info[0].version == 1);
    TASK_CHECK(info[0].itemBase == 0x69);
    TASK_CHECK(info[0].headerSize == 32);
    TASK_CHECK(info[0].imageBase == 0x89);
    TASK_CHECK(info[0].imageSize == 0x200);
    TASK_CHECK(info[0].vendorBase == 0x289);
    TASK_CHECK(info[0].vendorSize == 0);
    TASK_CHECK(info[0].hardwareInstance == 0);
    TASK_CHECK(info[0].imageCapsuleSupport == 0);
    TASK_CHECK(info[0].imageTypeId == makeGuid(0x10));

    // v2 payload with vendor code and padding
    TASK_CHECK(info[1].version == 2);
    TASK_CHECK(info[1].itemBase == 0x289);
    TASK_CHECK(info[1].headerSize == 40);
    TASK_CHECK(info[1].imageBase == 0x2B1);
    TASK_CHECK(info[1].imageSize == 0x300);
    TASK_CHECK(info[1].vendorBase == 0x5B1);
    TASK_CHECK(info[1].vendorSize == 0x80);
    TASK_CHECK(info[1].hardwareInstance == 0xABCD);
    TASK_CHECK(info[1].imageCapsuleSupport == 0);
    TASK_CHECK(info[1].imageTypeId == makeGuid(0x20));

    // v3 payload, same ImageTypeId as the first one, both preserved
    TASK_CHECK(info[2].version == 3);
    TASK_CHECK(info[2].itemBase == 0x634);
    TASK_CHECK(info[2].headerSize == 48);
    TASK_CHECK(info[2].imageBase == 0x664);
    TASK_CHECK(info[2].imageSize == 0x180);
    TASK_CHECK(info[2].vendorBase == 0x7E4);
    TASK_CHECK(info[2].vendorSize == 0);
    TASK_CHECK(info[2].hardwareInstance == 0x777);
    TASK_CHECK(info[2].imageCapsuleSupport == 0);
    TASK_CHECK(info[2].imageTypeId == makeGuid(0x10));
    TASK_CHECK(info[2].item != info[0].item);

    // The item node holds exactly header + image + vendor, excluding padding
    TASK_CHECK(m.fullSize(info[1].item) == 40 + 0x300 + 0x80);
    TASK_CHECK(m.headerSize(info[1].item) == 40);

    // Vendor code node keeps the original FV bytes, unparsed
    std::vector<UModelIndex> vendors;
    for (int i = 0; i < m.rowCount(info[1].item); ++i) {
        UModelIndex x = m.index(i, 0, info[1].item);
        if (m.subtype(x) == Subtypes::FmpVendorCodeItem)
            vendors.push_back(x);
    }
    TASK_CHECK(vendors.size() == 1);
    TASK_CHECK(m.base(vendors[0]) == 0x5B1);
    TASK_CHECK(m.fullSize(vendors[0]) == 0x80);
    TASK_CHECK(m.full(vendors[0]) == UByteArray((const char*)vendorFv.data(), (int32_t)vendorFv.size()));
    TASK_CHECK(countType(m, vendors[0], Types::Volume) == 0);
    TASK_CHECK(m.rowCount(vendors[0]) == 0);

    // Padding after the vendor code is kept as padding, not as image bytes
    std::vector<UModelIndex> paddings;
    collectType(m, capsule, Types::Padding, paddings);
    TASK_CHECK(paddings.size() == 1);
    TASK_CHECK(m.base(paddings[0]) == 0x631);
    TASK_CHECK(m.fullSize(paddings[0]) == 3);

    // Only the three real update images were parsed, the vendor FV stayed opaque
    TASK_CHECK(countType(m, UModelIndex(), Types::Volume) == 3);
    TASK_CHECK(countType(m, info[0].image, Types::Volume) == 1);
    TASK_CHECK(countType(m, info[1].image, Types::Volume) == 1);
    TASK_CHECK(countType(m, info[2].image, Types::Volume) == 1);
    return 0;
}

static int case_opaque_items(void)
{
    // Payloads: unknown header version, v3 with authentication bit, v3 with
    // dependency bit carrying a valid FV, and one normal v1 payload
    Bytes opaqueRaw(0x40, 0);
    put(opaqueRaw, 0, 7, 4); // Unsupported header version 7

    std::vector<PayloadSpec> payloads(4);
    payloads[0].version = 7;
    payloads[0].image = opaqueRaw;
    payloads[1].version = 3;
    payloads[1].imageIndex = 1;
    payloads[1].support = 1; // IMAGE_CAPSULE_SUPPORT_AUTHENTICATION
    payloads[1].image = Bytes(0x100, 0xA5);
    payloads[1].vendor = Bytes(0x20, 0x11);
    payloads[1].hwInstance = 0x55;
    payloads[2].version = 3;
    payloads[2].imageIndex = 2;
    payloads[2].support = 2; // IMAGE_CAPSULE_SUPPORT_DEPENDENCY
    payloads[2].image = vtfVolume(0x200); // A valid FV that must stay opaque
    payloads[3].version = 1;
    payloads[3].imageIndex = 3;
    payloads[3].image = vtfVolume(0x200);

    BuiltFmp cap = buildFmpCapsule(0x20, std::vector<Bytes>(), payloads);
    // Item layout: table 8 + 4*8 = 0x28, items at 0x48, 0x88, 0x1D8, 0x408
    TASK_CHECK(cap.itemOffsets[0] == 0x48);
    TASK_CHECK(cap.itemOffsets[1] == 0x88);
    TASK_CHECK(cap.itemOffsets[2] == 0x1D8);
    TASK_CHECK(cap.itemOffsets[3] == 0x408);

    TreeModel m;
    FfsParser p(&m);
    TASK_CHECK(parseBytes(m, p, cap.bytes) == U_SUCCESS);

    const std::vector<FmpPayloadInfo>& info = p.getFmpPayloads();
    TASK_CHECK(info.size() == 4);

    // Unknown header version: the entire item is one opaque range
    TASK_CHECK(info[0].version == 7);
    TASK_CHECK(info[0].itemBase == 0x48);
    TASK_CHECK(info[0].headerSize == 0);
    TASK_CHECK(info[0].imageBase == 0x48);
    TASK_CHECK(info[0].imageSize == 0x40);
    TASK_CHECK(info[0].vendorBase == 0x88);
    TASK_CHECK(info[0].vendorSize == 0);
    TASK_CHECK(!info[0].image.isValid());
    TASK_CHECK(info[0].imageIndex == 0);
    TASK_CHECK(info[0].hardwareInstance == 0);
    TASK_CHECK(info[0].imageCapsuleSupport == 0);
    TASK_CHECK(info[0].imageTypeId == UByteArray(16, '\0'));
    TASK_CHECK(!info[0].hasCpuMapping);
    TASK_CHECK(info[0].addressDiff == 0);
    TASK_CHECK(m.fullSize(info[0].item) == 0x40);
    TASK_CHECK(m.full(info[0].item) == UByteArray((const char*)opaqueRaw.data(), (int32_t)opaqueRaw.size()));

    // v3 with authentication support: known metadata, opaque contents
    TASK_CHECK(info[1].version == 3);
    TASK_CHECK(info[1].headerSize == 48);
    TASK_CHECK(info[1].itemBase == 0x88);
    TASK_CHECK(info[1].imageBase == 0xB8);
    TASK_CHECK(info[1].imageSize == 0x100);
    TASK_CHECK(info[1].vendorBase == 0x1B8);
    TASK_CHECK(info[1].vendorSize == 0x20);
    TASK_CHECK(info[1].imageCapsuleSupport == 1);
    TASK_CHECK(info[1].hardwareInstance == 0x55);
    TASK_CHECK(!info[1].image.isValid());
    TASK_CHECK(!info[1].hasCpuMapping);
    TASK_CHECK(m.fullSize(info[1].item) == 48 + 0x100 + 0x20);
    TASK_CHECK(countType(m, info[1].item, Types::Volume) == 0);

    // v3 with dependency support: a valid FV inside is not parsed either
    TASK_CHECK(info[2].version == 3);
    TASK_CHECK(info[2].headerSize == 48);
    TASK_CHECK(info[2].imageSize == 0x200);
    TASK_CHECK(info[2].imageCapsuleSupport == 2);
    TASK_CHECK(!info[2].image.isValid());
    TASK_CHECK(!info[2].hasCpuMapping);
    TASK_CHECK(countType(m, info[2].item, Types::Volume) == 0);
    TASK_CHECK(countType(m, info[2].item, Types::File) == 0);

    // The normal payload is parsed and mapped
    TASK_CHECK(info[3].image.isValid());
    TASK_CHECK(info[3].hasCpuMapping);
    TASK_CHECK(countType(m, info[3].image, Types::Volume) == 1);

    // Only the parsed payload contributed volumes to the tree
    TASK_CHECK(countType(m, UModelIndex(), Types::Volume) == 1);
    return 0;
}

static int checkErrorCase(Bytes bytes, size_t patchOffset, unsigned long long patchValue, size_t patchSize,
                          const char* offsetText)
{
    if (patchSize)
        put(bytes, patchOffset, patchValue, patchSize);
    TreeModel m;
    FfsParser p(&m);
    TASK_CHECK(parseBytes(m, p, bytes) == U_INVALID_CAPSULE);
    // A recognized FMP capsule is never reinterpreted as a bare image
    TASK_CHECK(m.rowCount(UModelIndex()) == 1);
    TASK_CHECK(m.type(m.index(0, 0, UModelIndex())) == Types::Capsule);
    // The parser message identifies the absolute offending field offset
    TASK_CHECK(hasFmpErrorAt(p, offsetText));
    return 0;
}

static int case_error_tables(void)
{
    // Good base: capsule header 0x20, FMP table 0x18, items at 0x38 and 0x258
    std::vector<PayloadSpec> payloads(2);
    payloads[0].version = 1;
    payloads[0].image = vtfVolume(0x200);
    payloads[1].version = 1;
    payloads[1].image = vtfVolume(0x200);
    BuiltFmp good = buildFmpCapsule(0x20, std::vector<Bytes>(), payloads);
    TASK_CHECK(good.itemOffsets[0] == 0x38);
    TASK_CHECK(good.itemOffsets[1] == 0x258);

    // Second item offset not strictly increasing, offending entry at 0x30
    TASK_CHECK(checkErrorCase(good.bytes, 0x30, 0x18, 8, "30h") == 0);
    // First item offset points into the item table, offending entry at 0x28
    TASK_CHECK(checkErrorCase(good.bytes, 0x28, 0x10, 8, "28h") == 0);
    // Second item offset beyond the declared capsule end, offending entry at 0x30
    TASK_CHECK(checkErrorCase(good.bytes, 0x30, 0x100000, 8, "30h") == 0);
    // Declared capsule end cuts into the item table, count fields at 0x24
    TASK_CHECK(checkErrorCase(good.bytes, 0x18, 0x30, 4, "24h") == 0);
    // Declared capsule end cuts into the FMP header itself, header at 0x20
    TASK_CHECK(checkErrorCase(good.bytes, 0x18, 0x25, 4, "20h") == 0);
    // First item too small for its payload header, header at 0x38
    TASK_CHECK(checkErrorCase(good.bytes, 0x30, 0x28, 8, "38h") == 0);
    // UpdateImageSize crosses the item boundary, field at 0x50
    TASK_CHECK(checkErrorCase(good.bytes, 0x50, 0x1000, 4, "50h") == 0);
    // UpdateVendorCodeSize crosses the item boundary, field at 0x54
    TASK_CHECK(checkErrorCase(good.bytes, 0x54, 0x1000, 4, "54h") == 0);
    return 0;
}

// ---------------------------------------------------------------------------
// addresses group
// ---------------------------------------------------------------------------

static int case_per_image_mapping(void)
{
    // Layout:
    //   capsule header [0, 0x20), FMP header + 3-entry table [0x20, 0x40)
    //   payload v1 [0x40, 0x260)  image at 0x60, 0x200 bytes, VTF
    //   payload v2 [0x260, 0x588) image at 0x288, 0x300 bytes, VTF
    //   payload v1 [0x588, 0x728) image at 0x5A8, 0x180 bytes, no VTF
    std::vector<PayloadSpec> payloads(3);
    payloads[0].version = 1;
    payloads[0].imageIndex = 1;
    payloads[0].image = vtfVolume(0x200);
    payloads[1].version = 2;
    payloads[1].imageIndex = 2;
    payloads[1].image = vtfVolume(0x300);
    payloads[2].version = 1;
    payloads[2].imageIndex = 3;
    payloads[2].image = rawVolume(0x180);

    BuiltFmp cap = buildFmpCapsule(0x20, std::vector<Bytes>(), payloads);
    TASK_CHECK(cap.itemOffsets.size() == 3);
    TASK_CHECK(cap.itemOffsets[0] == 0x40);
    TASK_CHECK(cap.itemOffsets[1] == 0x260);
    TASK_CHECK(cap.itemOffsets[2] == 0x588);

    TreeModel m;
    FfsParser p(&m);
    TASK_CHECK(parseBytes(m, p, cap.bytes) == U_SUCCESS);

    const std::vector<FmpPayloadInfo>& info = p.getFmpPayloads();
    TASK_CHECK(info.size() == 3);

    // Per-image CPU address mappings from each image's own VTF
    TASK_CHECK(info[0].hasCpuMapping);
    TASK_CHECK(info[0].addressDiff == 0xfffffda0ULL);
    TASK_CHECK(info[1].hasCpuMapping);
    TASK_CHECK(info[1].addressDiff == 0xfffffa78ULL);
    TASK_CHECK(!info[2].hasCpuMapping);
    TASK_CHECK(info[2].addressDiff == 0);

    // getIndexesAddressDiffs pairs each mapped image with its own difference
    std::vector<std::pair<UModelIndex, UINT64> > diffs = p.getIndexesAddressDiffs();
    TASK_CHECK(diffs.size() == 2);
    TASK_CHECK(diffs[0].first == info[0].image);
    TASK_CHECK(diffs[0].second == 0xfffffda0ULL);
    TASK_CHECK(diffs[1].first == info[1].image);
    TASK_CHECK(diffs[1].second == 0xfffffa78ULL);

    // Address text of uncompressed descendants uses the owning image's difference
    std::vector<UModelIndex> volumes;
    collectType(m, UModelIndex(), Types::Volume, volumes);
    TASK_CHECK(volumes.size() == 3);
    TASK_CHECK(m.base(volumes[0]) == 0x60);
    TASK_CHECK(m.base(volumes[1]) == 0x288);
    TASK_CHECK(m.base(volumes[2]) == 0x5A8);
    TASK_CHECK(infoHas(m, volumes[0], "Header address: FFFFFE00h"));
    TASK_CHECK(infoHas(m, volumes[1], "Header address: FFFFFD00h"));
    // The sibling without VTF borrows no CPU address from the other images
    TASK_CHECK(!infoHas(m, volumes[2], "address"));

    // Both VTF files got their own reset vector info
    std::vector<UModelIndex> files;
    collectType(m, UModelIndex(), Types::File, files);
    TASK_CHECK(files.size() == 3);
    TASK_CHECK(infoHas(m, files[0], "Reset vector:"));
    TASK_CHECK(infoHas(m, files[1], "Reset vector:"));
    TASK_CHECK(!infoHas(m, files[2], "Reset vector:"));
    // VTF file addresses belong to their own images
    TASK_CHECK(infoHas(m, files[0], "Header address: FFFFFE48h"));
    TASK_CHECK(infoHas(m, files[1], "Header address: FFFFFD48h"));

    // Nodes outside the images get no CPU address text
    UModelIndex capsule = m.index(0, 0, UModelIndex());
    TASK_CHECK(!infoHas(m, capsule, "address"));
    TASK_CHECK(!infoHas(m, info[0].item, "address"));
    TASK_CHECK(!infoHas(m, info[1].item, "address"));
    TASK_CHECK(!infoHas(m, info[2].item, "address"));
    return 0;
}

static int case_te_image_bases(void)
{
    // Three payloads, each with one TE section; the first two images have a
    // VTF, the third has none. TE ImageBase values are patched after layout
    // so that each TE matches only its own image's mapping (the third one is
    // patched to match a wrong borrow from the second image, as a trap).
    const size_t teBodySize = 4 + 40 + 20; // Section header + TE header + code
    Bytes teTemplate(teBodySize, 0);
    put(teTemplate, 0, teBodySize, 3);
    teTemplate[3] = EFI_SECTION_TE;
    put(teTemplate, 4, 0x5A56, 2);   // 'VZ'
    put(teTemplate, 6, 0x8664, 2);   // Machine: x64
    teTemplate[8] = 1;               // NumberOfSections
    teTemplate[9] = 0;               // Subsystem
    put(teTemplate, 10, 40, 2);      // StrippedSize

    Bytes teFile = ffsFile(makeGuid(0x40), EFI_FV_FILETYPE_PEIM, teTemplate);
    Bytes vol0 = volumeWithFiles({teFile, vtfFile(0x40)});  // 0xF8 bytes
    Bytes vol1 = volumeWithFiles({teFile, vtfFile(0x60)});  // 0x118 bytes
    Bytes vol2 = volumeWithFiles({teFile, rawFile(0x40)});  // 0xF8 bytes, no VTF
    TASK_CHECK(vol0.size() == 0xF8);
    TASK_CHECK(vol1.size() == 0x118);
    TASK_CHECK(vol2.size() == 0xF8);

    std::vector<PayloadSpec> payloads(3);
    for (int i = 0; i < 3; ++i) {
        payloads[i].version = 1;
        payloads[i].imageIndex = (unsigned)(i + 1);
    }
    payloads[0].image = vol0;
    payloads[1].image = vol1;
    payloads[2].image = vol2;

    BuiltFmp cap = buildFmpCapsule(0x20, std::vector<Bytes>(), payloads);
    TASK_CHECK(cap.itemOffsets[0] == 0x40);
    TASK_CHECK(cap.itemOffsets[1] == 0x158);
    TASK_CHECK(cap.itemOffsets[2] == 0x290);

    // Locate the two VTF files and the three TE headers in the built image
    std::vector<size_t> vtfPos, tePos;
    for (size_t i = 0; i + 16 <= cap.bytes.size(); ++i) {
        if (0 == std::memcmp(cap.bytes.data() + i, EFI_FFS_VOLUME_TOP_FILE_GUID.constData(), 16))
            vtfPos.push_back(i);
        if (i + 6 <= cap.bytes.size() && 0 == std::memcmp(cap.bytes.data() + i, "\x56\x5A\x64\x86\x01\x00", 6))
            tePos.push_back(i);
    }
    TASK_CHECK(vtfPos.size() == 2);
    TASK_CHECK(tePos.size() == 3);

    // Patch TE ImageBase values: each TE matches its own image's mapping,
    // except the third one, which would only match a borrowed difference
    unsigned long long diff0 = 0x100000000ULL - (vtfPos[0] + 0x58);
    unsigned long long diff1 = 0x100000000ULL - (vtfPos[1] + 0x78);
    put(cap.bytes, tePos[0] + 16, diff0 + tePos[0], 8);
    put(cap.bytes, tePos[1] + 16, diff1 + tePos[1], 8);
    put(cap.bytes, tePos[2] + 16, diff1 + tePos[2], 8); // Trap: borrowed from image 2

    TreeModel m;
    FfsParser p(&m);
    TASK_CHECK(parseBytes(m, p, cap.bytes) == U_SUCCESS);

    const std::vector<FmpPayloadInfo>& info = p.getFmpPayloads();
    TASK_CHECK(info.size() == 3);
    TASK_CHECK(info[0].hasCpuMapping);
    TASK_CHECK(info[0].addressDiff == diff0);
    TASK_CHECK(info[1].hasCpuMapping);
    TASK_CHECK(info[1].addressDiff == diff1);
    TASK_CHECK(!info[2].hasCpuMapping);

    // Collect TE sections in base order and check their image base types
    std::vector<UModelIndex> sections;
    collectType(m, UModelIndex(), Types::Section, sections);
    std::vector<UModelIndex> teSections;
    for (size_t i = 0; i < sections.size(); ++i) {
        if (m.subtype(sections[i]) == EFI_SECTION_TE)
            teSections.push_back(sections[i]);
    }
    TASK_CHECK(teSections.size() == 3);
    TASK_CHECK(m.base(teSections[0]) + 4 == tePos[0]);
    TASK_CHECK(m.base(teSections[1]) + 4 == tePos[1]);
    TASK_CHECK(m.base(teSections[2]) + 4 == tePos[2]);

    for (size_t i = 0; i < 3; ++i) {
        TASK_CHECK(!m.hasEmptyParsingData(teSections[i]));
        UByteArray data = m.parsingData(teSections[i]);
        const TE_IMAGE_SECTION_PARSING_DATA* pdata = (const TE_IMAGE_SECTION_PARSING_DATA*)data.constData();
        // Both mapped images accepted their own TE bases...
        TASK_CHECK(pdata->imageBaseType == (i < 2 ? EFI_IMAGE_TE_BASE_ORIGINAL : EFI_IMAGE_TE_BASE_OTHER));
    }

    // ...and no TE was reported as unmapped
    std::vector<std::pair<UString, UModelIndex> > msgs = p.getMessages();
    for (size_t i = 0; i < msgs.size(); ++i)
        TASK_CHECK(msgs[i].first.find("TE image base is neither") < 0);
    return 0;
}

// ---------------------------------------------------------------------------
// compatibility group
// ---------------------------------------------------------------------------

static int case_bare_firmware(void)
{
    Bytes fv = vtfVolume(0x200);
    TreeModel m;
    FfsParser p(&m);
    TASK_CHECK(parseBytes(m, p, fv) == U_SUCCESS);

    // Bare firmware keeps its legacy reading
    TASK_CHECK(p.getFmpPayloads().empty());
    TASK_CHECK(p.getAddressDiff() == 0xfffffe00ULL);
    TASK_CHECK(p.getIndexesAddressDiffs().empty());
    TASK_CHECK(countType(m, UModelIndex(), Types::Image) == 1);
    TASK_CHECK(countType(m, UModelIndex(), Types::Volume) == 1);
    TASK_CHECK(countType(m, UModelIndex(), Types::File) == 1);

    std::vector<UModelIndex> volumes;
    collectType(m, UModelIndex(), Types::Volume, volumes);
    TASK_CHECK(volumes.size() == 1);
    TASK_CHECK(infoHas(m, volumes[0], "Header address: FFFFFE00h"));

    std::vector<UModelIndex> files;
    collectType(m, UModelIndex(), Types::File, files);
    TASK_CHECK(files.size() == 1);
    TASK_CHECK(infoHas(m, files[0], "Reset vector:"));
    return 0;
}

static int case_plain_uefi_capsule(void)
{
    // A non-FMP UEFI capsule keeps the original single-image reading
    Bytes fv = vtfVolume(0x200);
    Bytes cap(0x20, 0);
    setGuid(cap, 0, EFI_CAPSULE_GUID);
    put(cap, 16, 0x20, 4);
    put(cap, 20, 0, 4);
    put(cap, 24, 0x20 + fv.size(), 4);
    cap.insert(cap.end(), fv.begin(), fv.end());

    TreeModel m;
    FfsParser p(&m);
    TASK_CHECK(parseBytes(m, p, cap) == U_SUCCESS);

    TASK_CHECK(p.getFmpPayloads().empty());
    TASK_CHECK(m.type(m.index(0, 0, UModelIndex())) == Types::Capsule);
    TASK_CHECK(m.subtype(m.index(0, 0, UModelIndex())) == Subtypes::UefiCapsule);
    TASK_CHECK(countType(m, UModelIndex(), Types::Image) == 1);
    TASK_CHECK(countType(m, UModelIndex(), Types::Volume) == 1);
    // The single VTF still drives the legacy address difference
    TASK_CHECK(p.getAddressDiff() == 0x100000000ULL - (0x20 + 0x200));

    std::vector<UModelIndex> volumes;
    collectType(m, UModelIndex(), Types::Volume, volumes);
    TASK_CHECK(volumes.size() == 1);
    TASK_CHECK(infoHas(m, volumes[0], "Header address: FFFFFE00h"));
    return 0;
}

static int case_sample_fixture_file(void)
{
    // The checked-in fixture is the deterministic output of baseline.cpp
    std::ifstream file("task_support/samples/two-images.cap", std::ios::binary);
    TASK_CHECK(file.good());
    Bytes bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    TASK_CHECK(bytes.size() == 1408);

    // Byte-identical to the in-memory fixture
    TASK_CHECK(bytes == fixtureCapsule());

    TreeModel m;
    FfsParser p(&m);
    TASK_CHECK(parseBytes(m, p, bytes) == U_SUCCESS);

    const std::vector<FmpPayloadInfo>& payloads = p.getFmpPayloads();
    TASK_CHECK(payloads.size() == 2);
    // Contract fixture layout: capsule header 32, FMP table 24
    TASK_CHECK(payloads[0].itemBase == 56);
    TASK_CHECK(payloads[0].imageBase == 88);
    TASK_CHECK(payloads[0].imageSize == 512);
    TASK_CHECK(payloads[0].version == 1);
    TASK_CHECK(payloads[0].addressDiff == 0xfffffda8ULL);
    TASK_CHECK(payloads[1].itemBase == 600);
    TASK_CHECK(payloads[1].imageBase == 640);
    TASK_CHECK(payloads[1].imageSize == 768);
    TASK_CHECK(payloads[1].version == 2);
    TASK_CHECK(payloads[1].hardwareInstance == 0x1234);
    TASK_CHECK(payloads[1].addressDiff == 0xfffffa80ULL);
    TASK_CHECK(countType(m, UModelIndex(), Types::Volume) == 2);
    return 0;
}

// ---------------------------------------------------------------------------
// Case registry
// ---------------------------------------------------------------------------

const struct task_case task_cases[] = {
    { "items", "fixture_two_images", case_fixture_two_images },
    { "items", "header_versions_and_alignment", case_header_versions_and_alignment },
    { "items", "opaque_items", case_opaque_items },
    { "items", "error_tables", case_error_tables },
    { "addresses", "per_image_mapping", case_per_image_mapping },
    { "addresses", "te_image_bases", case_te_image_bases },
    { "compatibility", "bare_firmware", case_bare_firmware },
    { "compatibility", "plain_uefi_capsule", case_plain_uefi_capsule },
    { "compatibility", "sample_fixture_file", case_sample_fixture_file },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
