#include <re.h>
#include <stdio.h>
static void estab(void *arg) { (void)arg; printf("estab\n"); }
static void recv_h(struct mbuf *mb, void *arg) { (void)mb;(void)arg; }
static void close_h(int err, void *arg) { (void)err;(void)arg; printf("close %d\n", err); }
int main(void) {
    libre_init();
    struct tcp_sock *ts = NULL;
    struct sa laddr;
    sa_set_in(&laddr, 0x7f000001, 0);
    int err = tcp_sock_alloc(&ts, &laddr, NULL, NULL);
    printf("tcp_sock_alloc err=%d\n", err);
    if (!err) err = tcp_sock_bind(ts, &laddr);
    printf("tcp_sock_bind err=%d\n", err);
    if (!err) err = tcp_sock_listen(ts, 4);
    printf("tcp_sock_listen err=%d\n", err);
    if (!err) {
        struct sa got;
        tcp_sock_local_get(ts, &got);
        uint32_t port = sa_port(&got);
        printf("bound port=%u\n", port);
        struct tcp_conn *tc = NULL;
        sa_set_in(&got, 0x7f000001, port);
        err = tcp_connect(&tc, &got, estab, recv_h, close_h, NULL);
        printf("tcp_connect err=%d\n", err);
        mem_deref(tc);
    }
    mem_deref(ts);
    libre_close();
    return 0;
}
