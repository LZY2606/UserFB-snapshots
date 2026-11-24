/* fmp_example.cpp

Example: listing the payload items of a UEFI FMP capsule with
FfsParser::getFmpPayloads(), read-only, without executing embedded
drivers or flashing anything.

Build from the repository root (the static library is produced by
`bash task_support/run.sh target`):

    c++ -std=c++11 -I. -Icommon task_support/fmp_example.cpp .task-build/libfwcore.a -o .task-build/fmp_example

Run against the checked-in fixture:

    .task-build/fmp_example task_support/samples/two-images.cap

Expected output for the fixture:

    payload[0] item=0x38 header=0x20 image=[0x58+0x200) vendor=[0x258+0x0) v1 idx=1 hw=0x0 support=0x0 mapped diff=0xfffffda8
      image node has 1 direct child node(s)
    payload[1] item=0x258 header=0x28 image=[0x280+0x300) vendor=[0x580+0x0) v2 idx=2 hw=0x1234 support=0x0 mapped diff=0xfffffa80
      image node has 1 direct child node(s)

Each FmpPayloadInfo entry refers to real nodes of the parser's TreeModel
(item and image), so the firmware volumes, FFS files and TE/PE modules of
every update image can be inspected through the usual model API. Base
fields are absolute offsets into the capsule file. Opaque payloads
(unknown header version, or v3 authentication/dependency support bits)
have an invalid image index and no CPU address mapping.
*/

#include "common/ffsparser.h"

#include <cstdio>
#include <fstream>
#include <vector>

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <fmp-capsule>\n", argv[0]);
        return 1;
    }

    // Read the capsule file
    std::ifstream file(argv[1], std::ios::binary);
    if (!file.good()) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }
    std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    // Parse it; the TreeModel receives the whole capsule/item/image hierarchy
    TreeModel model;
    FfsParser parser(&model);
    USTATUS status = parser.parse(UByteArray(bytes.empty() ? NULL : &bytes[0], (int32_t)bytes.size()));
    if (status != U_SUCCESS) {
        std::fprintf(stderr, "parse failed with status %d\n", (int)status);
        return 1;
    }

    // Walk the payload table in its original order; duplicate ImageTypeId
    // values are preserved as separate entries
    const std::vector<FmpPayloadInfo>& payloads = parser.getFmpPayloads();
    if (payloads.empty()) {
        std::printf("not an FMP capsule, or no payload items\n");
        return 0;
    }
    for (size_t i = 0; i < payloads.size(); ++i) {
        const FmpPayloadInfo& p = payloads[i];
        std::printf("payload[%zu] item=0x%x header=0x%x image=[0x%x+0x%x) vendor=[0x%x+0x%x)"
                    " v%u idx=%u hw=0x%llx support=0x%llx",
                    i,
                    (unsigned)p.itemBase, (unsigned)p.headerSize,
                    (unsigned)p.imageBase, (unsigned)p.imageSize,
                    (unsigned)p.vendorBase, (unsigned)p.vendorSize,
                    (unsigned)p.version, (unsigned)p.imageIndex,
                    (unsigned long long)p.hardwareInstance,
                    (unsigned long long)p.imageCapsuleSupport);
        if (p.hasCpuMapping)
            std::printf(" mapped diff=0x%llx", (unsigned long long)p.addressDiff);
        else
            std::printf(" opaque-or-unmapped");
        std::printf("\n");

        // The parsed update image and its firmware volumes are reachable
        // through the tree, e.g. model.rowCount(p.image) > 0 when the image
        // contained a firmware volume
        if (p.image.isValid())
            std::printf("  image node has %d direct child node(s)\n", model.rowCount(p.image));
    }
    return 0;
}
