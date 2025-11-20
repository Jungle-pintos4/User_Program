#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "threads/synch.h"
#include "threads/interrupt.h"
#include "filesys/file.h"

#ifdef VM
#include "vm/vm.h"
#endif


/* 스레드의 생명 주기에서의 상태들. */
enum thread_status {
	THREAD_RUNNING,     /* 실행 중인 스레드. */
	THREAD_READY,       /* 실행 중이지 않지만 실행 준비된 상태. */
	THREAD_BLOCKED,     /* 이벤트가 발생하기를 기다리는 중. */
	THREAD_DYING        /* 곧 소멸될 예정. */
};

/* 스레드 식별자 타입.
   원하는 타입으로 재정의할 수 있음. */
typedef int tid_t;
#define TID_ERROR ((tid_t) -1)          /* tid_t의 에러 값. */

/* 스레드 우선순위. */
#define PRI_MIN 0                       /* 최저 우선순위. */
#define PRI_DEFAULT 31                  /* 기본 우선순위. */
#define PRI_MAX 63                      /* 최고 우선순위. */
/* 파일 디스크립터 최대 */
#define MAX_FD 64
/* 커널 스레드 또는 사용자 프로세스.
 *
 * 각 스레드 구조체는 자체 4 kB 페이지에 저장됨. 스레드 구조체
 * 자체는 페이지의 맨 아래(오프셋 0)에 위치함. 페이지의 나머지 부분은
 * 스레드의 커널 스택을 위해 예약되며, 이는 페이지의 상단(오프셋 4 kB)에서
 * 아래쪽으로 성장함. 다음은 이에 대한 그림:
 *
 *      4 kB +---------------------------------+
 *           |          커널 스택              |
 *           |                |                |
 *           |                |                |
 *           |                V                |
 *           |          아래로 성장            |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           +---------------------------------+
 *           |              magic              |
 *           |            intr_frame           |
 *           |                :                |
 *           |                :                |
 *           |               name              |
 *           |              status             |
 *      0 kB +---------------------------------+
 *
 * 이것의 결과는 두 가지:
 *
 *    1. 첫째, `struct thread'가 너무 크게 성장하는 것을 허용하면 안 됨.
 *       그렇게 되면 커널 스택을 위한 공간이 충분하지 않게 됨.
 *       기본 `struct thread'는 몇 바이트 크기에 불과함.
 *       아마도 1 kB 미만으로 유지되어야 함.
 *
 *    2. 둘째, 커널 스택이 너무 크게 성장하는 것을 허용하면 안 됨.
 *       스택이 오버플로우되면 스레드 상태가 손상됨. 따라서 커널 함수는
 *       큰 구조체나 배열을 non-static 지역 변수로 할당하면 안 됨.
 *       대신 malloc()이나 palloc_get_page()를 사용한 동적 할당을 사용할 것.
 *
 * 이러한 문제 중 하나의 첫 번째 증상은 아마도 thread_current()에서의
 * assertion 실패일 것임. 이는 실행 중인 스레드의 `struct thread'의
 * `magic' 멤버가 THREAD_MAGIC으로 설정되어 있는지 확인함.
 * 스택 오버플로우는 일반적으로 이 값을 변경하여 assertion을 발동시킴. */
/* `elem' 멤버는 이중 목적을 가짐. 실행 큐(thread.c)의 요소가 될 수도 있고,
 * 세마포어 대기 리스트(synch.c)의 요소가 될 수도 있음. 이 두 가지 방식으로
 * 사용될 수 있는 이유는 이들이 상호 배타적이기 때문임: ready 상태의
 * 스레드만 실행 큐에 있고, blocked 상태의 스레드만 세마포어 대기
 * 리스트에 있음. */
struct thread {
	/* thread.c가 소유함. */
	tid_t tid;                          /* 스레드 식별자. */
	enum thread_status status;          /* 스레드 상태. */
	char name[16];                      /* 이름 (디버깅 목적). */
	int priority;                       /* 우선순위. */

	/* thread.c와 synch.c가 공유함. */
	struct list_elem elem;              /* 리스트 요소. */

	struct list lock_list; 				/* 현재 쓰레드가 보유하고 있는 락 리스트 */
	int original_priority; 	 			/* 쓰레드의 원래 우선순위 (백업용으로 저장, 수정 X) */
	struct lock *waiting_lock;			/* 현재 쓰레드가 기다리고 있는 lock */
	struct list *waiting_list; 			/* 현재 쓰레드가 block 되어서 대기하고 있는 리스트의 위치 */

#ifdef USERPROG
	/* userprog/process.c가 소유함. */
	uint64_t *pml4;                     /* 페이지 맵 레벨 4 */

	int exit_status;                    // 종료 상태 (기본값 -1)
	int child_exit_status;              // 자식의 종료 상태를 저장 (wait에서 사용)
	struct semaphore wait_sema;         // wait 동기화
	struct semaphore fork_sema;         // fork 동기화
	struct thread *parent;              // 부모 프로세스
	struct list child_list;             // 자식 리스트
	struct list_elem child_elem;        // 자식 리스트의 요소
	bool fork_success;                  // fork 성공 여부
	bool waited;                        // 이미 wait 되었는지 여부
	struct intr_frame *parent_if;       // fork 시 부모의 interrupt frame (포인터)
	struct file **fd_table; 			// file descriptor table (one table per process)
#endif
#ifdef VM
	/* 스레드가 소유한 전체 가상 메모리를 위한 테이블. */
	struct supplemental_page_table spt;
#endif

	/* thread.c가 소유함. */
	struct intr_frame tf;               /* 전환을 위한 정보 */
	unsigned magic;                     /* 스택 오버플로우를 감지. */

	// time_sleep에서 깨어날 시간
	int64_t wakeup_tick;
};

/* false(기본값)이면 라운드 로빈 스케줄러 사용.
   true이면 다단계 피드백 큐 스케줄러 사용.
   커널 커맨드라인 옵션 "-o mlfqs"로 제어됨. */
extern bool thread_mlfqs;

bool priority_more(const struct list_elem *a, const struct list_elem *b, void *aux);

void thread_init (void);
void thread_start (void);

void thread_tick (void);
void thread_print_stats (void);

typedef void thread_func (void *aux);
tid_t thread_create (const char *name, int priority, thread_func *, void *);

void thread_block (void);
void thread_unblock (struct thread *);

struct thread *thread_current (void);
tid_t thread_tid (void);
const char *thread_name (void);

void thread_exit (void) NO_RETURN;
void thread_yield (void);

int thread_get_priority (void);
void thread_set_priority (int);

int thread_get_nice (void);
void thread_set_nice (int);
int thread_get_recent_cpu (void);
int thread_get_load_avg (void);

void do_iret (struct intr_frame *tf);

void thread_sleep (int64_t time_tick);
void thread_awake (int64_t wakeup_tick);

void thread_sleep_sort (int64_t time_tick);
void thread_awake_sort (int64_t wakeup_tick);

void thread_preemption(struct thread *thread);
void thread_donate_priority(struct thread *thread);
#endif /* threads/thread.h */
