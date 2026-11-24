#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#define conn_destructor probe_tcp_conn_destructor
#define conn_close probe_tcp_conn_close
#define conn_alloc probe_tcp_conn_alloc
#define tcp_recv_handler probe_tcp_recv_handler
#define sort_handler probe_sort_handler
#include "src/tcp/tcp.c"
#undef conn_destructor
#undef conn_close
#undef conn_alloc
#undef tcp_recv_handler
#undef sort_handler
#include <stdio.h>

static unsigned got_bytes;
static void my_recv(struct mbuf *mb, void *arg) {
    (void)arg;
    got_bytes += mbuf_get_left(mb);
    printf("recvh: %zu bytes\n", mbuf_get_left(mb));
}
static void my_close(int err, void *arg) { (void)err;(void)arg; printf("closeh err=%d\n", err); }

int main(void) {
    libre_init();
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) { perror("socketpair"); return 1; }
    fcntl(sv[0], F_SETFL, O_NONBLOCK);
    fcntl(sv[1], F_SETFL, O_NONBLOCK);

    struct tcp_conn *tc = mem_zalloc(sizeof(*tc), NULL);
    list_init(&tc->helpers);
    list_init(&tc->sendq);
    tc->fdc = sv[0];
    tc->rxsz = 8192;
    tc->txqsz_max = 524288;
    tc->connected = true;
    tc->recvh = my_recv;
    tc->closeh = my_close;

    struct mbuf *mb = mbuf_alloc(16);
    mbuf_write_str(mb, "hello tcp");
    mb->pos = 0;
    int err = tcp_send(tc, mb);
    printf("tcp_send err=%d txqsz=%zu\n", err, tc->txqsz);
    mem_deref(mb);

    write(sv[1], "ping", 4);
    probe_tcp_recv_handler(FD_READ, tc);
    printf("got_bytes=%u\n", got_bytes);

    close(sv[1]);
    probe_tcp_recv_handler(FD_READ, tc);
    printf("after peer close, fdc=%d\n", (int)tc->fdc);
    mem_deref(tc);
    close(sv[0]);
    libre_close();
    printf("PROBE3_OK\n");
    return 0;
}
