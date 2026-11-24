#include "rist-private.h"
#include "udp-private.h"
#include "proto/rist_time.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned sends[2];
static ssize_t capture(void *arg, int fd, const void *data, size_t n,
                       int flags, const struct sockaddr *addr, socklen_t alen)
{
    (void)arg; (void)flags; (void)addr; (void)alen;
    const unsigned char *p = data;
    if (fd < 10 || fd > 11 || n < 12 || p[0] != 0x80 || p[1] != 33 || p[2] != 0 || p[3] != 7 || n != 200 || p[12] != 0x47) return -1;
    ++sends[fd-10];
    return (ssize_t)n;
}
int main(void)
{
    struct rist_ctx *api = NULL;
    if (rist_sender_create(&api, RIST_PROFILE_SIMPLE, 200, NULL) != 0) return 2;
    struct rist_sender *s = api->sender_ctx;
    struct rist_peer peers[2];
    memset(peers, 0, sizeof(peers));
    s->common.transport.sendto = capture;
    s->common.transport_active = true;
    s->peer_lst_len = 2;
    for (unsigned i=0; i<2; ++i) {
        struct rist_peer *p = &peers[i];
        p->sender_ctx = s; p->peer_data = p; p->sd = 10+i;
        p->is_data = true; p->is_rtcp = true; p->adv_flow_id = 200;
        p->config.max_retries = 1;
        p->config.recovery_mode = RIST_RECOVERY_MODE_TIME;
        p->config.congestion_control_mode = RIST_CONGESTION_CONTROL_MODE_OFF;
        p->config.recovery_length_max = 10000;
        p->config.recovery_maxbitrate = 1000000;
        p->config.virt_dst_port = 1968;
    }
    unsigned char ts[188]; memset(ts, 0x34, sizeof(ts)); ts[0] = 0x47;
    struct rist_buffer *b = rist_new_buffer(&s->common, ts, sizeof(ts), RIST_PAYLOAD_TYPE_DATA_RAW, 7, timestampNTP_u64(), 1968, 1968);
    if (!b) return 3;
    b->seq = 7; b->seq_rtp = 7;
    s->sender_queue[7] = b; s->seq_index[7] = 7;
    rist_retry_enqueue(s, 7, &peers[0]);
    ssize_t a = rist_retry_dequeue(s);
    rist_retry_enqueue(s, 7, &peers[1]);
    ssize_t c = rist_retry_dequeue(s);
    printf("BASELINE A_ret=%zd B_ret=%zd A_datagrams=%u B_datagrams=%u shared_transmit_count=%u\n", a,c,sends[0],sends[1],b->transmit_count);
    int ok = a > 0 && c < 0 && sends[0] == 1 && sends[1] == 0;
    s->sender_queue[7] = NULL;
    free_rist_buffer(&s->common,b);
    s->peer_lst_len = 0;
    rist_destroy(api);
    if (!ok) { fputs("unexpected baseline\n",stderr); return 4; }
    puts("BASELINE_REPRODUCED_FANOUT_STARVATION");
    return 0;
}
