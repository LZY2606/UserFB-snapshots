// Local-only shim for task_support/baseline.cpp under the bind-blocked sandbox.
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
static int g_ux_pair[2] = {-1, -1};
static int sandbox_bind(int sockfd, const struct sockaddr*, socklen_t) {
  if (socketpair(AF_UNIX, SOCK_DGRAM, 0, g_ux_pair) != 0) return -1;
  fcntl(g_ux_pair[0], F_SETFL, fcntl(g_ux_pair[0], F_GETFL, 0) | O_NONBLOCK);
  return dup2(g_ux_pair[0], sockfd) >= 0 ? 0 : -1;
}
static ssize_t sandbox_sendto(int, const void* buf, size_t len, int,
			      const struct sockaddr*, socklen_t) {
  return write(g_ux_pair[1], buf, len);
}
#define bind(...) sandbox_bind(__VA_ARGS__)
#define sendto(...) sandbox_sendto(__VA_ARGS__)
#include "../task_support/baseline.cpp"
