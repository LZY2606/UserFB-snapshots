#include "memory_io.h"
#include <errno.h>
#include <string.h>
static unsigned char wire[1024*1024];
static size_t used, cursor;
void task_io_reset(void) { used=cursor=0; }
void task_io_rewind(void) { cursor=0; }
const unsigned char *task_io_data(void) { return wire; }
size_t task_io_size(void) { return used; }
ssize_t task_memory_send(int fd,const void *data,size_t size,int flags) {
    (void)fd;(void)flags;
    if (size>sizeof(wire)-used) { errno=ENOBUFS;return -1; }
    memcpy(wire+used,data,size);used+=size;return (ssize_t)size;
}
ssize_t task_memory_recv(int fd,void *data,size_t size,int flags) {
    (void)fd;(void)flags;
    if(size>used-cursor)size=used-cursor;
    memcpy(data,wire+cursor,size);cursor+=size;return (ssize_t)size;
}
