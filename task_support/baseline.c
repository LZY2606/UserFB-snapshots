/* Calls the production control dispatcher directly to avoid a handshake in
 * this initial-state observation. No replacement dispatcher or chunker. */
#include "src/rtmp/conn.c"
#include <re_main.h>
#include <stdio.h>
struct capture { size_t bytes; unsigned writes; };
static bool capture_send(int *err, struct mbuf *mb, void *arg) {
    struct capture *c = arg;
    c->bytes += mbuf_get_left(mb); ++c->writes;
    *err = 0; return true;
}
int main(int argc, char **argv) {
    struct rtmp_conn conn = {0};
    struct tcp_helper *helper = NULL;
    struct sa peer;
    struct capture cap = {0};
    struct mbuf *ctl = NULL;
    struct rtmp_header hdr = {0};
    uint8_t media[300] = {0x17,1,0,0,0};
    int err = libre_init();
    if (err) return 1;
    sa_set_in(&peer, 0x7f000001, 9999);
    err = tcp_conn_alloc(&conn.tc, &peer, NULL, NULL, NULL, NULL);
    if (err) return 2;
    err = tcp_register_helper(&helper,conn.tc,0,NULL,capture_send,NULL,&cap);
    if (err) return 3;
    conn.state = RTMP_STATE_HANDSHAKE_DONE;
    conn.send_chunk_size = 128; conn.window_ack_size = 2500000;
    if (argc > 1 && !strcmp(argv[1], "target")) {
        err = rtmp_conn_send_msg(&conn,0,6,100,0,RTMP_TYPE_VIDEO,1,media,sizeof(media));
        if (err || cap.bytes != 314) return 7;
        printf("BASELINE_NORMAL_OK media_wire=%zu\n", cap.bytes);
        mem_deref(helper); mem_deref(conn.tc); libre_close(); return 0;
    }
    ctl = mbuf_alloc(5);
    if (!ctl) return 4;
    mbuf_write_u32(ctl,htonl(64)); mbuf_write_u8(ctl,0); ctl->pos=0;
    hdr.type_id = RTMP_TYPE_SET_PEER_BANDWIDTH;
    err = rtmp_dechunk_handler(&hdr,ctl,&conn);
    /* Set Peer Bandwidth drives the sender window only; the receive
     * acknowledgement threshold (window_ack_size) stays untouched and the
     * Window Acknowledgement Size reply still emits the same 16 wire bytes */
    if (err || cap.bytes != 16 || conn.window_ack_size != 2500000) return 5;
    err = rtmp_conn_send_msg(&conn,0,6,100,0,RTMP_TYPE_VIDEO,1,media,sizeof(media));
    /* 300 bytes at a 128-byte chunk size = 3 chunks; wire bytes are still
     * 16 (control) + 314 (media) = 330, now in 1 + 3 separate writes */
    if (err || cap.bytes != 330 || cap.writes != 4) return 6;
    printf("BASELINE_OK sender_window=64 receive_ack_window=%u media_wire=314 total_wire=%zu writes=%u\n",conn.window_ack_size,cap.bytes,cap.writes);
    mem_deref(ctl); mem_deref(helper); mem_deref(conn.tc); libre_close();
    return 0;
}
