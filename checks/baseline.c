#include <ogg/ogg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Existing generic framing only. This does not parse Opus or repaginate. */
int main(void) {
    ogg_stream_state en, de;
    unsigned char head[19] = "OpusHead", tags[16] = "OpusTags", audio[8192];
    unsigned char *data[3] = { head, tags, audio };
    long sizes[3] = { sizeof head, sizeof tags, sizeof audio };
    int n = 0;
    long largest = 0;
    head[8] = 1; head[9] = 1;
    for (int i = 0; i < 8192; ++i) audio[i] = (unsigned char)(i * 7 + 3);
    if (ogg_stream_init(&en, 73) || ogg_stream_init(&de, 73)) return 1;
    for (int i = 0; i < 3; ++i) {
        ogg_packet in = {0}, out;
        ogg_page page;
        in.packet = data[i]; in.bytes = sizes[i];
        in.b_o_s = i == 0; in.e_o_s = i == 2;
        in.granulepos = i == 2 ? 960 : 0;
        if (ogg_stream_packetin(&en, &in)) return 2;
        while (ogg_stream_flush_fill(&en, &page, 512)) {
            long size = page.header_len + page.body_len;
            if (size > largest) largest = size;
            if (ogg_stream_pagein(&de, &page)) return 3;
            int r;
            while ((r = ogg_stream_packetout(&de, &out)) != 0) {
                if (r != 1 || n >= 3 || out.bytes != sizes[n] ||
                    out.packetno != n || memcmp(out.packet, data[n], sizes[n])) return 4;
                ++n;
            }
        }
    }
    ogg_stream_clear(&en); ogg_stream_clear(&de);
    if (n != 3 || largest <= 512) return 5;
    printf("OGG_BASELINE packets=%d payload_exact=1 fill_hint=512 largest_page=%ld\n", n, largest);
    puts("OGG_FILL_IS_SOFT cap=512 exceeded=1");
    return 0;
}
