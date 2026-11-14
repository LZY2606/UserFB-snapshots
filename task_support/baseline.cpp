#include "rtp.hh"
#include "uvgrtp/frame.hh"
#include <cstdio>
#include <cstring>
#include <memory>
int main() {
    auto ssrc = std::make_shared<std::atomic<uint32_t>>(0x12345678);
    uvgrtp::rtp rtp(RTP_FORMAT_GENERIC, ssrc, false);
    uint8_t p[] = {0x90,96,0,7,0,0,0,9,0x12,0x34,0x56,0x78,
                   0xbe,0xde,0,1,0x31,0xff,0xfe,0,0x65,0x11};
    uvgrtp::frame::rtp_frame* f = nullptr;
    auto ret = rtp.packet_handler(nullptr, 0, p, sizeof(p), &f);
    if (ret != RTP_PKT_MODIFIED || !f || !f->ext || f->ext->type != 0xbede ||
        f->ext->len != 4 || f->payload_len != 2 || f->payload[0] != 0x65 ||
        f->ext->data[1] != 0xff || f->ext->data[2] != 0xfe) return 1;
    std::printf("BASELINE_OK seq=%u extension=%04x transport_raw=%02x%02x payload=%zu\n",
                f->header.seq, f->ext->type, f->ext->data[1],f->ext->data[2],f->payload_len);
    uvgrtp::frame::dealloc_frame(f);
    return 0;
}
