/*
 * Tests for IPv6::pseudoheader_addresses().
 *
 * The transport layer pseudoheader checksum does not always use the
 * addresses found in the basic IPv6 header:
 *
 * - The source address is replaced by the Home Address Option (Mobile
 *   IPv6) when a Destination Options header carries one.
 * - The destination address in the basic header is only the *current*
 *   destination (next routing waypoint); the pseudoheader needs the
 *   *final* destination: the address carried by an active Type 2
 *   routing header, or Segment List[0] of an active SRH.
 *
 * These tests build packets programmatically, serialize them with
 * libtins and then verify the full transport checksum with an
 * independent network-order byte-wise sum.
 */

#include <tins/ipv6.h>
#include <tins/udp.h>
#include <tins/tcp.h>
#include <tins/icmpv6.h>
#include <tins/rawpdu.h>
#include <tins/exceptions.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <utility>
#include <vector>

using namespace Tins;

namespace {

unsigned* g_checks = 0;
unsigned g_line = 0;

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            g_line = __LINE__;                                              \
            fprintf(stderr, "check failed at line %u: %s\n",                \
                    g_line, #cond);                                         \
            return 1;                                                       \
        }                                                                   \
        ++*g_checks;                                                        \
    } while (0)

// Protocol numbers used in the IPv6 pseudoheader
const uint8_t PROTO_TCP_NUM = 6;
const uint8_t PROTO_UDP_NUM = 17;
const uint8_t PROTO_ICMPV6_NUM = 58;

const uint8_t HOME_ADDRESS_OPTION = 201;

// Independent one's complement sum over network-order bytes.
uint32_t sum_words(const uint8_t* ptr, size_t size) {
    uint32_t sum = 0;
    for (size_t i = 0; i < size; i += 2) {
        sum += (uint32_t(ptr[i]) << 8) | (i + 1 < size ? ptr[i + 1] : 0);
    }
    return sum;
}

// Folded residual of pseudoheader + transport bytes. A packet whose
// checksum was computed over (src, dst) yields 0xffff.
uint16_t transport_residual(const std::vector<uint8_t>& packet, size_t transport_offset,
                            const IPv6Address& src, const IPv6Address& dst,
                            uint8_t protocol) {
    uint8_t pseudoheader[40] = {0};
    src.copy(pseudoheader);
    dst.copy(pseudoheader + 16);
    const size_t length = packet.size() - transport_offset;
    pseudoheader[32] = (length >> 24) & 0xff;
    pseudoheader[33] = (length >> 16) & 0xff;
    pseudoheader[34] = (length >> 8) & 0xff;
    pseudoheader[35] = length & 0xff;
    pseudoheader[39] = protocol;
    uint32_t sum = sum_words(pseudoheader, sizeof(pseudoheader))
                 + sum_words(&packet[transport_offset], length);
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return static_cast<uint16_t>(sum);
}

// Type 2 routing header: type, segments left, 4 reserved bytes, one address
std::vector<uint8_t> make_type2_header(const IPv6Address& home, uint8_t segments_left) {
    std::vector<uint8_t> data(22, 0);
    data[0] = 2;
    data[1] = segments_left;
    home.copy(&data[6]);
    return data;
}

// Segment Routing Header: type 4, segments left, last entry, flags, tag,
// segment list (wire order: Segment List[0] first), then TLVs
std::vector<uint8_t> make_srh_header(uint8_t segments_left, uint8_t last_entry,
                                     const std::vector<IPv6Address>& segments,
                                     const std::vector<uint8_t>& tlvs) {
    std::vector<uint8_t> data;
    data.push_back(4);
    data.push_back(segments_left);
    data.push_back(last_entry);
    data.push_back(0); // flags
    data.push_back(0); // tag
    data.push_back(0);
    for (size_t i = 0; i < segments.size(); ++i) {
        uint8_t buffer[16];
        segments[i].copy(buffer);
        data.insert(data.end(), buffer, buffer + 16);
    }
    data.insert(data.end(), tlvs.begin(), tlvs.end());
    return data;
}

// Destination Options header holding a Home Address Option aligned 8n+6
// (offset 4 within this data, 6 from the start of the extension header)
std::vector<uint8_t> make_home_address_dest_opts(const IPv6Address& home) {
    std::vector<uint8_t> data;
    data.push_back(IPv6::PAD_N); // 2 padding bytes to align the next option
    data.push_back(2);
    data.push_back(0);
    data.push_back(0);
    data.push_back(HOME_ADDRESS_OPTION);
    data.push_back(16);
    uint8_t buffer[16];
    home.copy(buffer);
    data.insert(data.end(), buffer, buffer + 16);
    return data;
}

IPv6::ext_header make_ext_header(IPv6::ExtensionHeader id, const std::vector<uint8_t>& data) {
    return IPv6::ext_header(id, data.size(), data.empty() ? 0 : &data[0]);
}

bool throws_invalid_extension_header(const IPv6& packet) {
    try {
        packet.pseudoheader_addresses();
    }
    catch (const invalid_ipv6_extension_header&) {
        return true;
    }
    catch (...) {
        return false;
    }
    return false;
}

const IPv6Address SRC("2001:db8::1");
const IPv6Address HOME("2001:db8::5");
const IPv6Address NEXT_HOP("2001:db8::10");
const IPv6Address FINAL_DEST("2001:db8::20");
const IPv6Address SEG0("2001:db8::30");
const IPv6Address SEG1("2001:db8::31");
const IPv6Address SEG2("2001:db8::32");

int test_routing() {
    // No extension headers: the basic header addresses are used
    {
        IPv6 packet(NEXT_HOP, SRC);
        packet /= UDP(2, 1) / RawPDU("odd");
        CHECK(packet.pseudoheader_addresses() == std::make_pair(SRC, NEXT_HOP));
        std::vector<uint8_t> bytes = packet.serialize();
        CHECK(transport_residual(bytes, packet.header_size(), SRC, NEXT_HOP,
                                 PROTO_UDP_NUM) == 0xffff);
    }
    // Active Type 2 routing header: final destination comes from the header
    {
        std::vector<uint8_t> routing = make_type2_header(FINAL_DEST, 1);
        IPv6 packet(NEXT_HOP, SRC);
        packet.add_header(make_ext_header(IPv6::ROUTING, routing));
        packet /= UDP(2, 1) / RawPDU("odd");
        CHECK(packet.pseudoheader_addresses() == std::make_pair(SRC, FINAL_DEST));
        std::vector<uint8_t> bytes = packet.serialize();
        CHECK(transport_residual(bytes, packet.header_size(), SRC, FINAL_DEST,
                                 PROTO_UDP_NUM) == 0xffff);
        CHECK(transport_residual(bytes, packet.header_size(), SRC, NEXT_HOP,
                                 PROTO_UDP_NUM) != 0xffff);
        // The basic header still holds the current destination
        CHECK(packet.dst_addr() == NEXT_HOP);
    }
    // Type 2 with Segments Left == 0: already processed, the address in
    // the header may have been swapped, so the basic header stands
    {
        std::vector<uint8_t> routing = make_type2_header(FINAL_DEST, 0);
        IPv6 packet(NEXT_HOP, SRC);
        packet.add_header(make_ext_header(IPv6::ROUTING, routing));
        packet /= TCP(80, 1234) / RawPDU("x");
        CHECK(packet.pseudoheader_addresses() == std::make_pair(SRC, NEXT_HOP));
        std::vector<uint8_t> bytes = packet.serialize();
        CHECK(transport_residual(bytes, packet.header_size(), SRC, NEXT_HOP,
                                 PROTO_TCP_NUM) == 0xffff);
        CHECK(transport_residual(bytes, packet.header_size(), SRC, FINAL_DEST,
                                 PROTO_TCP_NUM) != 0xffff);
    }
    // Active Type 2 routing header with ICMPv6
    {
        std::vector<uint8_t> routing = make_type2_header(FINAL_DEST, 1);
        ICMPv6 echo(ICMPv6::ECHO_REQUEST);
        echo.identifier(0x1234);
        echo.sequence(7);
        IPv6 packet(NEXT_HOP, SRC);
        packet.add_header(make_ext_header(IPv6::ROUTING, routing));
        packet /= echo / RawPDU("abcde");
        CHECK(packet.pseudoheader_addresses() == std::make_pair(SRC, FINAL_DEST));
        std::vector<uint8_t> bytes = packet.serialize();
        CHECK(transport_residual(bytes, packet.header_size(), SRC, FINAL_DEST,
                                 PROTO_ICMPV6_NUM) == 0xffff);
        CHECK(transport_residual(bytes, packet.header_size(), SRC, NEXT_HOP,
                                 PROTO_ICMPV6_NUM) != 0xffff);
    }
    // Active SRH with several segments and trailing TLVs: the final
    // destination is Segment List[0] (stored first on the wire), not the
    // current segment and not the current destination
    {
        std::vector<IPv6Address> segments;
        segments.push_back(SEG0);
        segments.push_back(SEG1);
        segments.push_back(SEG2);
        std::vector<uint8_t> tlvs;
        tlvs.push_back(IPv6::PAD_N); // PadN, 2 bytes
        tlvs.push_back(2);
        tlvs.push_back(0xaa);
        tlvs.push_back(0xbb);
        tlvs.push_back(30); // unknown TLV, 1 byte
        tlvs.push_back(1);
        tlvs.push_back(0x5a);
        tlvs.push_back(IPv6::PAD_1);
        std::vector<uint8_t> srh = make_srh_header(2, 2, segments, tlvs);
        IPv6 packet(NEXT_HOP, SRC);
        packet.add_header(make_ext_header(IPv6::ROUTING, srh));
        packet /= UDP(9, 8) / RawPDU("odd");
        CHECK(packet.pseudoheader_addresses() == std::make_pair(SRC, SEG0));
        std::vector<uint8_t> bytes = packet.serialize();
        CHECK(transport_residual(bytes, packet.header_size(), SRC, SEG0,
                                 PROTO_UDP_NUM) == 0xffff);
        CHECK(transport_residual(bytes, packet.header_size(), SRC, NEXT_HOP,
                                 PROTO_UDP_NUM) != 0xffff);
        // The active segment (Segment List[2]) is not the final destination
        CHECK(transport_residual(bytes, packet.header_size(), SRC, SEG2,
                                 PROTO_UDP_NUM) != 0xffff);
    }
    // Reduced segment list: Segments Left == Last Entry + 1 is legal
    {
        std::vector<IPv6Address> segments;
        segments.push_back(SEG0);
        segments.push_back(SEG1);
        std::vector<uint8_t> srh = make_srh_header(2, 1, segments,
                                                   std::vector<uint8_t>());
        IPv6 packet(NEXT_HOP, SRC);
        packet.add_header(make_ext_header(IPv6::ROUTING, srh));
        CHECK(packet.pseudoheader_addresses() == std::make_pair(SRC, SEG0));
    }
    // SRH with Segments Left == 0: basic header destination stands
    {
        std::vector<IPv6Address> segments;
        segments.push_back(SEG0);
        segments.push_back(SEG1);
        std::vector<uint8_t> srh = make_srh_header(0, 1, segments,
                                                   std::vector<uint8_t>());
        IPv6 packet(NEXT_HOP, SRC);
        packet.add_header(make_ext_header(IPv6::ROUTING, srh));
        CHECK(packet.pseudoheader_addresses() == std::make_pair(SRC, NEXT_HOP));
    }
    // Unknown routing type and (unimplemented) type 0 are tolerated while
    // fully processed
    {
        std::vector<uint8_t> unknown(8, 0);
        unknown[0] = 99;
        unknown[1] = 0;
        IPv6 packet(NEXT_HOP, SRC);
        packet.add_header(make_ext_header(IPv6::ROUTING, unknown));
        CHECK(packet.pseudoheader_addresses() == std::make_pair(SRC, NEXT_HOP));

        std::vector<uint8_t> type0(8, 0);
        type0[0] = 0;
        type0[1] = 0;
        IPv6 packet2(NEXT_HOP, SRC);
        packet2.add_header(make_ext_header(IPv6::ROUTING, type0));
        CHECK(packet2.pseudoheader_addresses() == std::make_pair(SRC, NEXT_HOP));
    }
    return 0;
}

int test_home() {
    // Home Address Option replaces the source address
    {
        std::vector<uint8_t> dest_opts = make_home_address_dest_opts(HOME);
        IPv6 packet(NEXT_HOP, SRC);
        packet.add_header(make_ext_header(IPv6::DESTINATION_OPTIONS, dest_opts));
        packet /= UDP(2, 1) / RawPDU("odd");
        CHECK(packet.pseudoheader_addresses() == std::make_pair(HOME, NEXT_HOP));
        std::vector<uint8_t> bytes = packet.serialize();
        CHECK(transport_residual(bytes, packet.header_size(), HOME, NEXT_HOP,
                                 PROTO_UDP_NUM) == 0xffff);
        CHECK(transport_residual(bytes, packet.header_size(), SRC, NEXT_HOP,
                                 PROTO_UDP_NUM) != 0xffff);
        // The basic header source address is untouched
        CHECK(packet.src_addr() == SRC);
    }
    // Padding and unknown options are skipped without effect
    {
        std::vector<uint8_t> dest_opts;
        dest_opts.push_back(IPv6::PAD_1);
        dest_opts.push_back(IPv6::PAD_1);
        dest_opts.push_back(IPv6::PAD_N); // PadN, 2 bytes
        dest_opts.push_back(2);
        dest_opts.push_back(0xaa);
        dest_opts.push_back(0xbb);
        dest_opts.push_back(0x1e); // unknown option, 3 bytes
        dest_opts.push_back(3);
        dest_opts.push_back(1);
        dest_opts.push_back(2);
        dest_opts.push_back(3);
        IPv6 packet(NEXT_HOP, SRC);
        packet.add_header(make_ext_header(IPv6::DESTINATION_OPTIONS, dest_opts));
        CHECK(packet.pseudoheader_addresses() == std::make_pair(SRC, NEXT_HOP));
    }
    // Home Address Option with TCP
    {
        std::vector<uint8_t> dest_opts = make_home_address_dest_opts(HOME);
        IPv6 packet(NEXT_HOP, SRC);
        packet.add_header(make_ext_header(IPv6::DESTINATION_OPTIONS, dest_opts));
        packet /= TCP(443, 1000) / RawPDU("ab");
        CHECK(packet.pseudoheader_addresses() == std::make_pair(HOME, NEXT_HOP));
        std::vector<uint8_t> bytes = packet.serialize();
        CHECK(transport_residual(bytes, packet.header_size(), HOME, NEXT_HOP,
                                 PROTO_TCP_NUM) == 0xffff);
        CHECK(transport_residual(bytes, packet.header_size(), SRC, NEXT_HOP,
                                 PROTO_TCP_NUM) != 0xffff);
    }
    // Home Address Option with ICMPv6
    {
        std::vector<uint8_t> dest_opts = make_home_address_dest_opts(HOME);
        ICMPv6 echo(ICMPv6::ECHO_REQUEST);
        echo.identifier(1);
        echo.sequence(2);
        IPv6 packet(NEXT_HOP, SRC);
        packet.add_header(make_ext_header(IPv6::DESTINATION_OPTIONS, dest_opts));
        packet /= echo / RawPDU("abc");
        CHECK(packet.pseudoheader_addresses() == std::make_pair(HOME, NEXT_HOP));
        std::vector<uint8_t> bytes = packet.serialize();
        CHECK(transport_residual(bytes, packet.header_size(), HOME, NEXT_HOP,
                                 PROTO_ICMPV6_NUM) == 0xffff);
    }
    return 0;
}

int test_combined() {
    // Home Address Option plus active Type 2 routing header: both
    // addresses are substituted in a single query
    {
        std::vector<uint8_t> dest_opts = make_home_address_dest_opts(HOME);
        std::vector<uint8_t> routing = make_type2_header(FINAL_DEST, 1);
        IPv6 packet(NEXT_HOP, SRC);
        packet.add_header(make_ext_header(IPv6::DESTINATION_OPTIONS, dest_opts));
        packet.add_header(make_ext_header(IPv6::ROUTING, routing));
        packet /= UDP(2, 1) / RawPDU("odd");
        CHECK(packet.pseudoheader_addresses() == std::make_pair(HOME, FINAL_DEST));
        std::vector<uint8_t> bytes = packet.serialize();
        CHECK(transport_residual(bytes, packet.header_size(), HOME, FINAL_DEST,
                                 PROTO_UDP_NUM) == 0xffff);
        CHECK(transport_residual(bytes, packet.header_size(), SRC, NEXT_HOP,
                                 PROTO_UDP_NUM) != 0xffff);
        CHECK(transport_residual(bytes, packet.header_size(), HOME, NEXT_HOP,
                                 PROTO_UDP_NUM) != 0xffff);
        CHECK(transport_residual(bytes, packet.header_size(), SRC, FINAL_DEST,
                                 PROTO_UDP_NUM) != 0xffff);
    }
    // Home Address Option plus active SRH, verified over TCP
    {
        std::vector<uint8_t> dest_opts = make_home_address_dest_opts(HOME);
        std::vector<IPv6Address> segments;
        segments.push_back(SEG0);
        segments.push_back(SEG1);
        std::vector<uint8_t> srh = make_srh_header(1, 1, segments,
                                                   std::vector<uint8_t>());
        IPv6 packet(NEXT_HOP, SRC);
        packet.add_header(make_ext_header(IPv6::DESTINATION_OPTIONS, dest_opts));
        packet.add_header(make_ext_header(IPv6::ROUTING, srh));
        packet /= TCP(80, 8080) / RawPDU("odd");
        CHECK(packet.pseudoheader_addresses() == std::make_pair(HOME, SEG0));
        std::vector<uint8_t> bytes = packet.serialize();
        CHECK(transport_residual(bytes, packet.header_size(), HOME, SEG0,
                                 PROTO_TCP_NUM) == 0xffff);
        CHECK(transport_residual(bytes, packet.header_size(), SRC, NEXT_HOP,
                                 PROTO_TCP_NUM) != 0xffff);
    }
    // IPv6 in IPv6: each IPv6 PDU reports its own addresses and the
    // transport checksum only sees the innermost IPv6 header chain
    {
        const IPv6Address OUTER_SRC("2001:db8::100");
        const IPv6Address OUTER_DST("2001:db8::200");
        const IPv6Address OUTER_HOME("2001:db8::105");

        std::vector<uint8_t> inner_dest_opts = make_home_address_dest_opts(HOME);
        std::vector<uint8_t> inner_routing = make_type2_header(FINAL_DEST, 1);
        IPv6 inner(NEXT_HOP, SRC);
        inner.add_header(make_ext_header(IPv6::DESTINATION_OPTIONS, inner_dest_opts));
        inner.add_header(make_ext_header(IPv6::ROUTING, inner_routing));
        inner /= UDP(2, 1) / RawPDU("odd");

        // The outer header carries its own Home Address Option; it must
        // not leak into the inner transport checksum
        std::vector<uint8_t> outer_dest_opts = make_home_address_dest_opts(OUTER_HOME);
        IPv6 outer(OUTER_DST, OUTER_SRC);
        outer.add_header(make_ext_header(IPv6::DESTINATION_OPTIONS, outer_dest_opts));
        outer /= inner;

        CHECK(outer.pseudoheader_addresses() == std::make_pair(OUTER_HOME, OUTER_DST));
        CHECK(inner.pseudoheader_addresses() == std::make_pair(HOME, FINAL_DEST));
        std::vector<uint8_t> bytes = outer.serialize();
        const size_t transport_offset = outer.header_size() + inner.header_size();
        CHECK(transport_residual(bytes, transport_offset, HOME, FINAL_DEST,
                                 PROTO_UDP_NUM) == 0xffff);
        CHECK(transport_residual(bytes, transport_offset, OUTER_HOME, FINAL_DEST,
                                 PROTO_UDP_NUM) != 0xffff);
        CHECK(transport_residual(bytes, transport_offset, SRC, NEXT_HOP,
                                 PROTO_UDP_NUM) != 0xffff);
    }
    return 0;
}

int test_malformed() {
    // Truncated Type 2 routing header
    {
        std::vector<uint8_t> routing(10, 0);
        routing[0] = 2;
        routing[1] = 1;
        IPv6 packet(NEXT_HOP, SRC);
        packet.add_header(make_ext_header(IPv6::ROUTING, routing));
        CHECK(throws_invalid_extension_header(packet));
    }
    // Type 2 with an illegal Segments Left value
    {
        std::vector<uint8_t> routing = make_type2_header(FINAL_DEST, 2);
        IPv6 packet(NEXT_HOP, SRC);
        packet.add_header(make_ext_header(IPv6::ROUTING, routing));
        CHECK(throws_invalid_extension_header(packet));
    }
    // Type 2 carrying more than the single allowed address
    {
        std::vector<uint8_t> routing(38, 0);
        routing[0] = 2;
        routing[1] = 1;
        IPv6 packet(NEXT_HOP, SRC);
        packet.add_header(make_ext_header(IPv6::ROUTING, routing));
        CHECK(throws_invalid_extension_header(packet));
    }
    // Truncated SRH fixed fields
    {
        std::vector<uint8_t> srh(4, 0);
        srh[0] = 4;
        srh[1] = 1;
        IPv6 packet(NEXT_HOP, SRC);
        packet.add_header(make_ext_header(IPv6::ROUTING, srh));
        CHECK(throws_invalid_extension_header(packet));
    }
    // SRH with Segments Left beyond Last Entry + 1
    {
        std::vector<IPv6Address> segments;
        segments.push_back(SEG0);
        segments.push_back(SEG1);
        std::vector<uint8_t> srh = make_srh_header(3, 1, segments,
                                                   std::vector<uint8_t>());
        IPv6 packet(NEXT_HOP, SRC);
        packet.add_header(make_ext_header(IPv6::ROUTING, srh));
        CHECK(throws_invalid_extension_header(packet));
    }
    // SRH whose segment list is truncated
    {
        std::vector<IPv6Address> segments;
        segments.push_back(SEG0);
        segments.push_back(SEG1);
        std::vector<uint8_t> srh = make_srh_header(1, 2, segments,
                                                   std::vector<uint8_t>());
        IPv6 packet(NEXT_HOP, SRC);
        packet.add_header(make_ext_header(IPv6::ROUTING, srh));
        CHECK(throws_invalid_extension_header(packet));
    }
    // SRH with a TLV that overruns the end of the header
    {
        std::vector<IPv6Address> segments;
        segments.push_back(SEG0);
        std::vector<uint8_t> tlvs;
        tlvs.push_back(0x1e);
        tlvs.push_back(5); // claims 5 bytes, only 1 follows
        tlvs.push_back(0);
        std::vector<uint8_t> srh = make_srh_header(1, 0, segments, tlvs);
        IPv6 packet(NEXT_HOP, SRC);
        packet.add_header(make_ext_header(IPv6::ROUTING, srh));
        CHECK(throws_invalid_extension_header(packet));
    }
    // Active routing header of an unknown type
    {
        std::vector<uint8_t> unknown(8, 0);
        unknown[0] = 99;
        unknown[1] = 1;
        IPv6 packet(NEXT_HOP, SRC);
        packet.add_header(make_ext_header(IPv6::ROUTING, unknown));
        CHECK(throws_invalid_extension_header(packet));
    }
    // Active type 0 routing header (not implemented)
    {
        std::vector<uint8_t> type0(8, 0);
        type0[0] = 0;
        type0[1] = 1;
        IPv6 packet(NEXT_HOP, SRC);
        packet.add_header(make_ext_header(IPv6::ROUTING, type0));
        CHECK(throws_invalid_extension_header(packet));
    }
    // Two Home Address Options, even spread over separate Destination
    // Options headers
    {
        std::vector<uint8_t> dest_opts = make_home_address_dest_opts(HOME);
        IPv6 packet(NEXT_HOP, SRC);
        packet.add_header(make_ext_header(IPv6::DESTINATION_OPTIONS, dest_opts));
        packet.add_header(make_ext_header(IPv6::DESTINATION_OPTIONS, dest_opts));
        CHECK(throws_invalid_extension_header(packet));
    }
    // Home Address Option with a wrong content length
    {
        std::vector<uint8_t> dest_opts;
        dest_opts.push_back(IPv6::PAD_N);
        dest_opts.push_back(2);
        dest_opts.push_back(0);
        dest_opts.push_back(0);
        dest_opts.push_back(HOME_ADDRESS_OPTION);
        dest_opts.push_back(8); // must be 16
        dest_opts.insert(dest_opts.end(), 8, 0);
        IPv6 packet(NEXT_HOP, SRC);
        packet.add_header(make_ext_header(IPv6::DESTINATION_OPTIONS, dest_opts));
        CHECK(throws_invalid_extension_header(packet));
    }
    // Home Address Option at a misaligned offset
    {
        std::vector<uint8_t> dest_opts;
        dest_opts.push_back(IPv6::PAD_1);
        dest_opts.push_back(IPv6::PAD_1);
        dest_opts.push_back(IPv6::PAD_1);
        dest_opts.push_back(HOME_ADDRESS_OPTION); // offset 3, not 8n+6
        dest_opts.push_back(16);
        uint8_t buffer[16];
        HOME.copy(buffer);
        dest_opts.insert(dest_opts.end(), buffer, buffer + 16);
        IPv6 packet(NEXT_HOP, SRC);
        packet.add_header(make_ext_header(IPv6::DESTINATION_OPTIONS, dest_opts));
        CHECK(throws_invalid_extension_header(packet));
    }
    // Two active routing headers in the same IPv6 header chain
    {
        std::vector<uint8_t> routing = make_type2_header(FINAL_DEST, 1);
        IPv6 packet(NEXT_HOP, SRC);
        packet.add_header(make_ext_header(IPv6::ROUTING, routing));
        packet.add_header(make_ext_header(IPv6::ROUTING, routing));
        CHECK(throws_invalid_extension_header(packet));
    }
    // Destination Options TLV that overruns the header
    {
        std::vector<uint8_t> dest_opts;
        dest_opts.push_back(0x1e);
        dest_opts.push_back(10); // claims 10 bytes, only 1 follows
        dest_opts.push_back(0);
        IPv6 packet(NEXT_HOP, SRC);
        packet.add_header(make_ext_header(IPv6::DESTINATION_OPTIONS, dest_opts));
        CHECK(throws_invalid_extension_header(packet));
    }
    // Serialization of a packet with a malformed header fails loudly
    // instead of emitting a checksum over the wrong addresses
    {
        std::vector<uint8_t> routing = make_type2_header(FINAL_DEST, 2);
        IPv6 packet(NEXT_HOP, SRC);
        packet.add_header(make_ext_header(IPv6::ROUTING, routing));
        packet /= UDP(2, 1) / RawPDU("odd");
        bool threw = false;
        try {
            packet.serialize();
        }
        catch (const invalid_ipv6_extension_header&) {
            threw = true;
        }
        CHECK(threw);
    }
    return 0;
}

int test_roundtrip() {
    // A combined packet keeps reporting the same addresses no matter how
    // often it is queried or serialized, and re-parsing the wire bytes
    // yields the same view
    std::vector<uint8_t> dest_opts = make_home_address_dest_opts(HOME);
    std::vector<IPv6Address> segments;
    segments.push_back(SEG0);
    segments.push_back(SEG1);
    segments.push_back(SEG2);
    std::vector<uint8_t> tlvs;
    tlvs.push_back(IPv6::PAD_N);
    tlvs.push_back(2);
    tlvs.push_back(0xaa);
    tlvs.push_back(0xbb);
    tlvs.push_back(30);
    tlvs.push_back(1);
    tlvs.push_back(0x5a);
    tlvs.push_back(IPv6::PAD_1);
    std::vector<uint8_t> srh = make_srh_header(2, 2, segments, tlvs);

    IPv6 packet(NEXT_HOP, SRC);
    packet.add_header(make_ext_header(IPv6::DESTINATION_OPTIONS, dest_opts));
    packet.add_header(make_ext_header(IPv6::ROUTING, srh));
    packet /= UDP(2, 1) / RawPDU("odd");

    const std::pair<IPv6Address, IPv6Address> expected(HOME, SEG0);
    CHECK(packet.pseudoheader_addresses() == expected);
    std::vector<uint8_t> bytes = packet.serialize();
    // Querying does not modify the object
    CHECK(packet.pseudoheader_addresses() == expected);
    CHECK(packet.src_addr() == SRC);
    CHECK(packet.dst_addr() == NEXT_HOP);
    // Serializing does not change the answer either, and is stable
    CHECK(packet.pseudoheader_addresses() == expected);
    CHECK(packet.serialize() == bytes);
    CHECK(packet.pseudoheader_addresses() == expected);

    // The wire bytes hold the untouched basic header addresses
    IPv6Address wire_src(&bytes[8]);
    IPv6Address wire_dst(&bytes[24]);
    CHECK(wire_src == SRC);
    CHECK(wire_dst == NEXT_HOP);

    // Re-parse and query again
    IPv6 parsed(&bytes[0], bytes.size());
    CHECK(parsed.pseudoheader_addresses() == expected);
    CHECK(parsed.src_addr() == SRC);
    CHECK(parsed.dst_addr() == NEXT_HOP);

    // The routing header survived the round trip: segments and TLVs intact
    const IPv6::ext_header* parsed_srh = parsed.search_header(IPv6::ROUTING);
    CHECK(parsed_srh != 0);
    CHECK(parsed_srh->data_size() == srh.size());
    CHECK(memcmp(parsed_srh->data_ptr(), &srh[0], srh.size()) == 0);

    // The payload survived as well
    const RawPDU* raw = parsed.find_pdu<RawPDU>();
    CHECK(raw != 0);
    CHECK(raw->payload() == RawPDU("odd").payload());

    // Re-serializing the parsed packet reproduces the original bytes
    CHECK(parsed.serialize() == bytes);

    // The checksum on the wire validates against the reported addresses
    CHECK(transport_residual(bytes, parsed.header_size(), HOME, SEG0,
                             PROTO_UDP_NUM) == 0xffff);
    CHECK(transport_residual(bytes, parsed.header_size(), SRC, NEXT_HOP,
                             PROTO_UDP_NUM) != 0xffff);
    return 0;
}

} // namespace

int ipv6_pseudo_case(const char* group, unsigned* checks) {
    g_checks = checks;
    const std::string name(group);
    if (name == "routing") {
        return test_routing();
    }
    if (name == "home") {
        return test_home();
    }
    if (name == "combined") {
        return test_combined();
    }
    if (name == "malformed") {
        return test_malformed();
    }
    if (name == "roundtrip") {
        return test_roundtrip();
    }
    return 2;
}
