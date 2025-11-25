#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#define STDIN_FILENO_MARKER  ((struct file *) 1)
#define STDOUT_FILENO_MARKER ((struct file *) 2)

void syscall_init (void);
extern struct lock filesys_lock; /* 파일 시스템에 접근할 때 동기화를 보장하기 위한 락*/

#endif /* userprog/syscall.h */
