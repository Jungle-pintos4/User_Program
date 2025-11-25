#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H
#include "threads/thread.h"

void syscall_init (void);
extern struct lock filesys_lock; /* 파일 시스템에 접근할 때 동기화를 보장하기 위한 락*/

struct file_descriptor *create_fd_wrapper(struct file *f, enum fd_type f_type);
void close_fd(struct file_descriptor *fd_wrapper);
struct file_descriptor *get_fd_wrapper(int fd);


#endif /* userprog/syscall.h */
