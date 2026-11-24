#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
int main(void) {
    int sv[2] = {-1,-1};
    int e = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    printf("socketpair AF_UNIX: e=%d errno=%d (%s)\n", e, errno, strerror(errno));
    if (e == 0) {
        const char *m = "hello";
        write(sv[0], m, 5);
        char b[8] = {0};
        ssize_t n = read(sv[1], b, sizeof(b));
        printf("pair data n=%zd b=%s\n", n, b);
        close(sv[0]); close(sv[1]);
    }
    int t = socket(AF_INET, SOCK_STREAM, 0);
    printf("tcp socket fd=%d\n", t);
    struct sockaddr_un sun = {0};
    sun.sun_family = AF_UNIX;
    const char *td = getenv("TMPDIR");
    snprintf(sun.sun_path, sizeof(sun.sun_path), "%sprobe.sock", td ? td : "/tmp/");
    int u = socket(AF_UNIX, SOCK_STREAM, 0);
    e = bind(u, (struct sockaddr *)&sun, sizeof(sun));
    printf("unix path bind: e=%d errno=%d (%s) path=%s\n", e, errno, strerror(errno), sun.sun_path);
    if (e == 0) { e = listen(u, 2); printf("unix listen: e=%d errno=%d\n", e, errno); }
    close(u); close(t);
    return 0;
}
