#ifndef TASK_MEMORY_IO_H
#define TASK_MEMORY_IO_H
#include <stddef.h>
#include <sys/types.h>
void task_io_reset(void);
void task_io_rewind(void);
const unsigned char *task_io_data(void);
size_t task_io_size(void);
ssize_t task_memory_send(int fd,const void *data,size_t size,int flags);
ssize_t task_memory_recv(int fd,void *data,size_t size,int flags);
#endif
