#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"

struct child_status{
	int exit_status;                    // 종료 상태 (기본값 -1)
	tid_t tid; 							// 자식의 tid;
	bool waited;					    // double wait 차단을 위한 플래그
	bool exited;						// 자식의 종료 여부 
	bool fork_success;					// 자식의 성공 / 실패 상태 
	struct semaphore wait_sema;         // wait 동기화
	struct semaphore fork_sema;			// fork 동기화
	struct thread *parent;              // 부모 프로세스
	struct list_elem child_elem;        // 자식 리스트의 요소
};

tid_t process_create_initd (const char *file_name);
tid_t process_fork (const char *name, struct intr_frame *if_);
int process_exec (void *f_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (struct thread *next);

#endif /* userprog/process.h */
