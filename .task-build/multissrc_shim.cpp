// LOCAL-ONLY sandbox shim: the grading environment allows loopback UDP binds;
// this macOS sandbox does not (EPERM).  To still execute the REAL receiver
// through the REAL event loop here, redirect the test's bind()/sendto() onto
// an AF_UNIX datagram socketpair that has been dup2()'d over the Groupsock's
// socket.  No production or test source is changed by this file.
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

static int g_ux_pair[2] = {-1, -1};

static int sandbox_bind(int sockfd, const struct sockaddr* /*addr*/,
			socklen_t /*addrlen*/) {
  // Each harness binds once; give every bind its own fresh socketpair so
  // packets left unread at teardown cannot leak into the next harness
  // (real UDP gets the same isolation from a new ephemeral port).
  if (g_ux_pair[0] >= 0) {
    close(g_ux_pair[0]);
    close(g_ux_pair[1]);
    g_ux_pair[0] = g_ux_pair[1] = -1;
  }
  if (socketpair(AF_UNIX, SOCK_DGRAM, 0, g_ux_pair) != 0) return -1;
  fcntl(g_ux_pair[0], F_SETFL,
	fcntl(g_ux_pair[0], F_GETFL, 0) | O_NONBLOCK);
  return dup2(g_ux_pair[0], sockfd) >= 0 ? 0 : -1;
}

static ssize_t sandbox_sendto(int /*sockfd*/, const void* buf, size_t len,
			      int /*flags*/,
			      const struct sockaddr* /*destaddr*/,
			      socklen_t /*addrlen*/) {
  return write(g_ux_pair[1], buf, len);
}

#define bind(...) sandbox_bind(__VA_ARGS__)
#define sendto(...) sandbox_sendto(__VA_ARGS__)

#include "../task_tests/multissrc.cpp"
