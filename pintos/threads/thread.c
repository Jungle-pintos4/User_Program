#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* struct thread의 `magic' 멤버를 위한 임의의 값.
   스택 오버플로우를 감지하는 데 사용됨. 자세한 내용은
   thread.h 상단의 큰 주석을 참조. */
#define THREAD_MAGIC 0xcd6abf4b

/* 기본 스레드를 위한 임의의 값
   이 값을 수정하지 말 것. */
#define THREAD_BASIC 0xd42df210

/* THREAD_READY 상태에 있는 프로세스들의 리스트,
   즉, 실행 준비가 되었지만 실제로 실행 중이지는 않은 프로세스들. */
static struct list ready_list;

/* THREAD_SLEEP 상태에 있는 프로세스들의 리스트*/
static struct list sleep_list;

/* 유휴 스레드. */
static struct thread *idle_thread;

/* 초기 스레드, init.c:main()을 실행하는 스레드. */
static struct thread *initial_thread;

/* allocate_tid()에서 사용하는 락. */
static struct lock tid_lock;

/* 스레드 소멸 요청 */
static struct list destruction_req;

/* 통계. */
static long long idle_ticks;    /* 유휴 상태에서 보낸 타이머 틱 수. */
static long long kernel_ticks;  /* 커널 스레드에서의 타이머 틱 수. */
static long long user_ticks;    /* 사용자 프로그램에서의 타이머 틱 수. */

/* 스케줄링. */
#define TIME_SLICE 4            /* 각 스레드에 주어지는 타이머 틱 수. */
static unsigned thread_ticks;   /* 마지막 yield 이후의 타이머 틱 수. */

/* false(기본값)이면 라운드 로빈 스케줄러 사용.
   true이면 다단계 피드백 큐 스케줄러 사용.
   커널 커맨드라인 옵션 "-o mlfqs"로 제어됨. */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static void do_schedule(int status);
static void schedule (void);
static tid_t allocate_tid (void);

bool wakeup_tick_less(const struct list_elem *a, const struct list_elem *b, void *aux);
bool priority_more(const struct list_elem *a, const struct list_elem *b, void *aux);

/* T가 유효한 스레드를 가리키는 것으로 보이면 true를 반환. */
#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC)

/* 실행 중인 스레드를 반환.
 * CPU의 스택 포인터 `rsp'를 읽고, 이를 페이지의 시작으로
 * 내림. `struct thread'는 항상 페이지의 시작에 있고
 * 스택 포인터는 중간 어딘가에 있으므로, 이를 통해 현재 스레드를 찾음. */
#define running_thread() ((struct thread *) (pg_round_down (rrsp ())))


// thread_start를 위한 전역 디스크립터 테이블.
// gdt는 thread_init 이후에 설정되므로, 먼저
// 임시 gdt를 설정해야 함.
static uint64_t gdt[3] = { 0, 0x00af9a000000ffff, 0x00cf92000000ffff };

/* 현재 실행 중인 코드를 스레드로 변환하여 스레딩 시스템을 초기화.
   이는 일반적으로는 작동하지 않으며, loader.S가 스택의 바닥을
   페이지 경계에 조심스럽게 배치했기 때문에 이 경우에만 가능함.

   또한 실행 큐와 tid 락을 초기화함.

   이 함수를 호출한 후에는 thread_create()로 스레드를 생성하기
   전에 페이지 할당자를 반드시 초기화해야 함.

   이 함수가 완료될 때까지 thread_current()를 호출하는 것은 안전하지 않음. */
void
thread_init (void) {
	ASSERT (intr_get_level () == INTR_OFF);

	/* 커널을 위한 임시 gdt를 다시 로드
	 * 이 gdt는 사용자 컨텍스트를 포함하지 않음.
	 * 커널은 gdt_init()에서 사용자 컨텍스트를 포함한 gdt를 다시 빌드할 것임. */
	struct desc_ptr gdt_ds = {
		.size = sizeof (gdt) - 1,
		.address = (uint64_t) gdt
	};
	lgdt (&gdt_ds);

	/* 전역 스레드 컨텍스트 초기화 */
	lock_init (&tid_lock);
	list_init (&ready_list);
	list_init (&destruction_req);
	list_init (&sleep_list);

	/* 실행 중인 스레드를 위한 스레드 구조체 설정. */
	initial_thread = running_thread ();
	init_thread (initial_thread, "main", PRI_DEFAULT);
	initial_thread->status = THREAD_RUNNING;
	initial_thread->tid = allocate_tid ();
}

/* 인터럽트를 활성화하여 선점형 스레드 스케줄링을 시작.
   또한 유휴 스레드를 생성함. */
void
thread_start (void) {
	/* 유휴 스레드 생성. */
	struct semaphore idle_started;
	sema_init (&idle_started, 0);
	thread_create ("idle", PRI_MIN, idle, &idle_started);

	/* 선점형 스레드 스케줄링 시작. */
	intr_enable ();

	/* 유휴 스레드가 idle_thread를 초기화할 때까지 대기. */
	sema_down (&idle_started);
}

/* 각 타이머 틱마다 타이머 인터럽트 핸들러에 의해 호출됨.
   따라서 이 함수는 외부 인터럽트 컨텍스트에서 실행됨. */
void
thread_tick (void) {
	struct thread *t = thread_current ();

	/* 통계 업데이트. */
	if (t == idle_thread)
		idle_ticks++;
#ifdef USERPROG
	else if (t->pml4 != NULL)
		user_ticks++;
#endif
	else
		kernel_ticks++;

	/* 선점 강제 실행. */
	if (++thread_ticks >= TIME_SLICE)
		intr_yield_on_return ();
}

/* 스레드 통계를 출력. */
void
thread_print_stats (void) {
	printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
			idle_ticks, kernel_ticks, user_ticks);
}

/* 주어진 초기 PRIORITY로 NAME이라는 이름의 새 커널 스레드를 생성하고,
   AUX를 인자로 전달하여 FUNCTION을 실행하며, 이를 준비 큐에 추가함.
   새 스레드의 스레드 식별자를 반환하거나, 생성 실패 시 TID_ERROR를 반환함.

   thread_start()가 호출된 경우, thread_create()가 반환되기 전에
   새 스레드가 스케줄될 수 있음. thread_create()가 반환되기 전에
   종료될 수도 있음. 반대로, 원래 스레드는 새 스레드가 스케줄되기
   전에 임의의 시간 동안 실행될 수 있음. 순서를 보장해야 하는 경우
   세마포어나 다른 형태의 동기화를 사용할 것.

   제공된 코드는 새 스레드의 `priority' 멤버를 PRIORITY로 설정하지만,
   실제 우선순위 스케줄링은 구현되지 않음.
   우선순위 스케줄링은 Problem 1-3의 목표임. */
tid_t
thread_create (const char *name, int priority,
		thread_func *function, void *aux) {
	struct thread *t;
	tid_t tid;

	ASSERT (function != NULL);

	/* 스레드 할당. */
	t = palloc_get_page (PAL_ZERO);
	if (t == NULL)
		return TID_ERROR;

	/* 스레드 초기화. */
	init_thread (t, name, priority);
	tid = t->tid = allocate_tid ();

	/* 스케줄되면 kernel_thread를 호출.
	 * 참고) rdi는 첫 번째 인자, rsi는 두 번째 인자. */
	t->tf.rip = (uintptr_t) kernel_thread;
	t->tf.R.rdi = (uint64_t) function;
	t->tf.R.rsi = (uint64_t) aux;
	t->tf.ds = SEL_KDSEG;
	t->tf.es = SEL_KDSEG;
	t->tf.ss = SEL_KDSEG;
	t->tf.cs = SEL_KCSEG;
	t->tf.eflags = FLAG_IF;

	/* 실행 큐에 추가. */
	thread_unblock (t);

	/* 선점 구현 */
	thread_preemption(list_entry(list_begin(&ready_list), struct thread, elem)); 

	return tid;
}

/* 현재 스레드를 재움. thread_unblock()에 의해 깨어날 때까지
   다시 스케줄되지 않음.

   이 함수는 인터럽트가 꺼진 상태에서 호출되어야 함.
   일반적으로 synch.h의 동기화 프리미티브 중 하나를 사용하는 것이 더 나음. */
void
thread_block (void) {
	ASSERT (!intr_context ());
	ASSERT (intr_get_level () == INTR_OFF);
	thread_current ()->status = THREAD_BLOCKED;
	schedule ();
}

/* 차단된 스레드 T를 실행 준비 상태로 전환.
   T가 차단되지 않았다면 오류임. (실행 중인 스레드를 준비 상태로
   만들려면 thread_yield()를 사용할 것.)

   이 함수는 실행 중인 스레드를 선점하지 않음. 이는 중요할 수 있음:
   호출자가 직접 인터럽트를 비활성화한 경우, 스레드를 원자적으로
   차단 해제하고 다른 데이터를 업데이트할 수 있을 것으로 기대할 수 있음. */
void
thread_unblock (struct thread *t) {
	enum intr_level old_level;
	ASSERT (is_thread (t));

	old_level = intr_disable ();
	ASSERT (t->status == THREAD_BLOCKED);
	t->status = THREAD_READY;
	list_insert_ordered (&ready_list, &t->elem, priority_more, NULL);
	intr_set_level (old_level);
}


/* 실행 중인 스레드의 이름을 반환. */
const char *
thread_name (void) {
	return thread_current ()->name;
}

/* 실행 중인 스레드를 반환.
   이는 running_thread()에 몇 가지 건전성 검사를 더한 것임.
   자세한 내용은 thread.h 상단의 큰 주석을 참조. */
struct thread *
thread_current (void) {
	struct thread *t = running_thread ();

	/* T가 실제로 스레드인지 확인.
	   이러한 assertion 중 하나라도 실패하면 스레드가 스택 오버플로우를
	   일으켰을 수 있음. 각 스레드는 4 kB 미만의 스택을 가지므로,
	   몇 개의 큰 자동 배열이나 중간 정도의 재귀로도
	   스택 오버플로우가 발생할 수 있음. */
	ASSERT (is_thread (t));
	ASSERT (t->status == THREAD_RUNNING);

	return t;
}

/* 실행 중인 스레드의 tid를 반환. */
tid_t
thread_tid (void) {
	return thread_current ()->tid;
}

/* 현재 스레드의 스케줄을 해제하고 소멸시킴.
   호출자에게 절대 반환되지 않음. */
void
thread_exit (void) {
	ASSERT (!intr_context ());

#ifdef USERPROG
	process_exit ();
#endif

	/* 상태를 dying으로 설정하고 다른 프로세스를 스케줄.
	   schedule_tail() 호출 중에 소멸될 것임. */
	intr_disable ();
	do_schedule (THREAD_DYING);
	NOT_REACHED ();
}

/* CPU를 양보. 현재 스레드는 재우지 않으며
   스케줄러의 결정에 따라 즉시 다시 스케줄될 수 있음. 
   다른 스레드로 계속 양보하는거*/
void
thread_yield (void) {
	struct thread *curr = thread_current ();
	enum intr_level old_level;

	ASSERT (!intr_context ());

	old_level = intr_disable ();
	if (curr != idle_thread)
		list_insert_ordered (&ready_list, &curr->elem, priority_more, NULL);
	do_schedule (THREAD_READY);
	intr_set_level (old_level);
}

// 20251106
/*timer_yield는 대기 상태로 진입이지만 해당 함수는 sleep 상태로 진입*/
void thread_sleep (int64_t wakeup_tick) {
	struct thread *curr = thread_current ();
	enum intr_level old_level;

	ASSERT (!intr_context ());
// 이거 중요
	old_level = intr_disable ();

	curr->wakeup_tick = wakeup_tick;

	list_push_back (&sleep_list, &curr->elem);
	
	thread_block();
// 이거 중요
	intr_set_level (old_level);
}

// 20251106
/*timer_yield는 대기 상태로 진입이지만 해당 함수는 sleep 상태로 진입, insert할 때 sort*/
void thread_sleep_sort (int64_t wakeup_tick) {
	struct thread *curr = thread_current ();
	enum intr_level old_level;

	ASSERT (!intr_context ());
	old_level = intr_disable ();

	curr->wakeup_tick = wakeup_tick;

	list_insert_ordered(&sleep_list, &curr->elem, wakeup_tick_less, NULL);
	thread_block();
	intr_set_level (old_level);
}

// 비교 함수
bool wakeup_tick_less(const struct list_elem *a, const struct list_elem *b, void *aux)
{
	struct thread *ta = list_entry(a, struct thread, elem);
	struct thread *tb = list_entry(b, struct thread, elem);
	return ta->wakeup_tick < tb->wakeup_tick;
}

bool priority_more(const struct list_elem *a, const struct list_elem *b, void *aux)
{
	struct thread *ta = list_entry(a, struct thread, elem);
	struct thread *tb = list_entry(b, struct thread, elem);
	return ta->priority > tb->priority;
}

// 20251107
/*block 상태에 있는 thread wakeup*/
void thread_awake_sort(int64_t wakeup_tick)
{
	if (list_empty(&sleep_list))
		return;

	while (!list_empty(&sleep_list))
	{
		struct thread *t = list_entry(list_pop_front(&sleep_list), struct thread, elem);
		if (t->wakeup_tick <= wakeup_tick){
			thread_unblock(t);
		}else{
			list_push_front(&sleep_list, &t->elem);
			break;
		}
	}
}

// 20251107
/*block 상태에 있는 thread wakeup*/ 
void thread_awake(int64_t wakeup_tick) {
	if(list_empty (&sleep_list))
		return;

	// 현재 리스트 크기
    int size = list_size(&sleep_list);  

    for (int i = 0; i < size; i++){
        struct thread *t = list_entry(list_pop_front(&sleep_list), struct thread, elem);
        if(t->wakeup_tick <= wakeup_tick){
            thread_unblock(t);
        } else {
            list_push_back(&sleep_list, &t->elem);
        }
    }
}

/* 
주어진 쓰레드가 선점할 수 있는지 확인하고, 선점할 수 있으면 선점한다.
인터럽트 컨텍스트, 쓰레드 컨텍스트 둘 모두에서 가능하다.
*/
void thread_preemption(struct thread *thread) {
	if (thread_get_priority() >= thread->priority)
		return ;

	if (intr_context()) {
		intr_yield_on_return();
	} else {
		thread_yield();
	}
}

/* 
현재 쓰레드가 주어진 쓰레드에게 우선순위를 기부하는 함수 
외부 인터럽트 컨텍스트에서 실행될 수 없다.
*/
void thread_donate_priority(struct thread *thread) {
	ASSERT (!intr_context ());
	enum intr_level old_level = intr_disable ();

	int depth = 0;

	while (depth < 8) {
		if (thread->priority < thread_get_priority()) {
			thread->priority = thread_get_priority(); // 우선순위 기부

			if (thread->status == THREAD_READY) {
				list_remove(&(thread->elem));
				list_insert_ordered(&ready_list, &(thread->elem), priority_more, NULL);
			} else if (thread->status == THREAD_BLOCKED) {
				list_remove(&(thread->elem));
				list_insert_ordered(thread->waiting_list, &(thread->elem), priority_more, NULL);
			}
		}

		struct lock *next_lock = thread->waiting_lock;
		if (next_lock == NULL || next_lock->holder == NULL) {
			break;
		}

		thread = thread->waiting_lock->holder;
		depth++;
	}

	intr_set_level(old_level);
}

/* 현재 스레드의 우선순위를 NEW_PRIORITY로 설정. */
void
thread_set_priority (int new_priority) {
	thread_current ()->original_priority = new_priority;
	refresh_priority();
	thread_preemption(list_entry(list_begin (&ready_list), struct thread, elem));
}

/* 현재 스레드의 우선순위를 반환. */
int
thread_get_priority (void) {
	return thread_current ()->priority;
}

/* 현재 스레드의 nice 값을 NICE로 설정. */
void
thread_set_nice (int nice UNUSED) {
	/* TODO: 여기에 구현 작성 */
}

/* 현재 스레드의 nice 값을 반환. */
int
thread_get_nice (void) {
	/* TODO: 여기에 구현 작성 */
	return 0;
}

/* 시스템 로드 평균의 100배 값을 반환. */
int
thread_get_load_avg (void) {
	/* TODO: 여기에 구현 작성 */
	return 0;
}

/* 현재 스레드의 recent_cpu 값의 100배를 반환. */
int
thread_get_recent_cpu (void) {
	/* TODO: 여기에 구현 작성 */
	return 0;
}

/* 유휴 스레드. 다른 스레드가 실행 준비되지 않았을 때 실행됨.

   유휴 스레드는 thread_start()에 의해 처음에 준비 리스트에 배치됨.
   처음에 한 번 스케줄되며, 그 시점에서 idle_thread를 초기화하고
   thread_start()가 계속되도록 전달받은 세마포어를 "up"하고
   즉시 차단됨. 그 후 유휴 스레드는 준비 리스트에 나타나지 않음.
   준비 리스트가 비어 있을 때 next_thread_to_run()에 의해
   특수한 경우로 반환됨. */
static void
idle (void *idle_started_ UNUSED) {
	struct semaphore *idle_started = idle_started_;

	idle_thread = thread_current ();
	sema_up (idle_started);

	for (;;) {
		/* 다른 스레드를 실행하도록 함. */
		intr_disable ();
		thread_block ();

		/* 인터럽트를 다시 활성화하고 다음 인터럽트를 기다림.

		   `sti' 명령어는 다음 명령어가 완료될 때까지 인터럽트를 비활성화하므로,
		   이 두 명령어는 원자적으로 실행됨. 이 원자성은 중요함;
		   그렇지 않으면 인터럽트를 다시 활성화하는 것과 다음 인터럽트가
		   발생하기를 기다리는 것 사이에 인터럽트가 처리될 수 있어,
		   최대 한 클록 틱만큼의 시간이 낭비될 수 있음.

		   [IA32-v2a] "HLT", [IA32-v2b] "STI", 그리고 [IA32-v3a]
		   7.11.1 "HLT Instruction"을 참조. */
		asm volatile ("sti; hlt" : : : "memory");
	}
}

/* 커널 스레드의 기초로 사용되는 함수. */
static void
kernel_thread (thread_func *function, void *aux) {
	ASSERT (function != NULL);

	intr_enable ();       /* 스케줄러는 인터럽트가 꺼진 상태로 실행됨. */
	function (aux);       /* 스레드 함수 실행. */
	thread_exit ();       /* function()이 반환되면 스레드를 종료. */
}


/* T를 NAME이라는 이름의 차단된 스레드로 기본 초기화. */
static void
init_thread (struct thread *t, const char *name, int priority) {
	ASSERT (t != NULL);
	ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
	ASSERT (name != NULL);

	memset (t, 0, sizeof *t);
	t->status = THREAD_BLOCKED;
	strlcpy (t->name, name, sizeof t->name);
	t->tf.rsp = (uint64_t) t + PGSIZE - sizeof (void *);
	t->priority = priority;
	t->magic = THREAD_MAGIC;
	t->original_priority = priority;
	t->waiting_lock = NULL;
	t->waiting_list = NULL;
	list_init(&t->lock_list);
#ifdef USERPROG
	t->pf = NULL;
	t->child_stat = NULL;
	t->execute_file = NULL;
	list_init(&t->child_list);
#endif
}

/* 스케줄될 다음 스레드를 선택하여 반환. 실행 큐가 비어있지 않다면
   실행 큐에서 스레드를 반환해야 함. (실행 중인 스레드가 계속 실행될 수
   있다면 실행 큐에 있을 것임.) 실행 큐가 비어있다면
   idle_thread를 반환. */
static struct thread *
next_thread_to_run (void) {
	if (list_empty (&ready_list))
		return idle_thread;
	else
		return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/* iretq를 사용하여 스레드를 시작 */
void
do_iret (struct intr_frame *tf) {
	__asm __volatile(
			"movq %0, %%rsp\n"
			"movq 0(%%rsp),%%r15\n"
			"movq 8(%%rsp),%%r14\n"
			"movq 16(%%rsp),%%r13\n"
			"movq 24(%%rsp),%%r12\n"
			"movq 32(%%rsp),%%r11\n"
			"movq 40(%%rsp),%%r10\n"
			"movq 48(%%rsp),%%r9\n"
			"movq 56(%%rsp),%%r8\n"
			"movq 64(%%rsp),%%rsi\n"
			"movq 72(%%rsp),%%rdi\n"
			"movq 80(%%rsp),%%rbp\n"
			"movq 88(%%rsp),%%rdx\n"
			"movq 96(%%rsp),%%rcx\n"
			"movq 104(%%rsp),%%rbx\n"
			"movq 112(%%rsp),%%rax\n"
			"addq $120,%%rsp\n"
			"movw 8(%%rsp),%%ds\n"
			"movw (%%rsp),%%es\n"
			"addq $32, %%rsp\n"
			"iretq"
			: : "g" ((uint64_t) tf) : "memory");
}

/* 새 스레드의 페이지 테이블을 활성화하여 스레드를 전환하고,
   이전 스레드가 dying 상태라면 소멸시킴.

   이 함수가 호출될 때, PREV 스레드로부터 방금 전환했고,
   새 스레드는 이미 실행 중이며, 인터럽트는 여전히 비활성화됨.

   스레드 전환이 완료될 때까지 printf()를 호출하는 것은 안전하지 않음.
   실제로는 printf()를 함수 끝에 추가해야 함을 의미함. */
static void
thread_launch (struct thread *th) {
	uint64_t tf_cur = (uint64_t) &running_thread ()->tf;
	uint64_t tf = (uint64_t) &th->tf;
	ASSERT (intr_get_level () == INTR_OFF);

	/* 메인 전환 로직.
	 * 먼저 전체 실행 컨텍스트를 intr_frame으로 복원한 다음
	 * do_iret을 호출하여 다음 스레드로 전환함.
	 * 주의: 전환이 완료될 때까지 여기서부터 스택을 사용하면 안 됨. */
	__asm __volatile (
			/* 사용될 레지스터 저장. */
			"push %%rax\n"
			"push %%rbx\n"
			"push %%rcx\n"
			/* 입력을 한 번 가져옴 */
			"movq %0, %%rax\n"
			"movq %1, %%rcx\n"
			"movq %%r15, 0(%%rax)\n"
			"movq %%r14, 8(%%rax)\n"
			"movq %%r13, 16(%%rax)\n"
			"movq %%r12, 24(%%rax)\n"
			"movq %%r11, 32(%%rax)\n"
			"movq %%r10, 40(%%rax)\n"
			"movq %%r9, 48(%%rax)\n"
			"movq %%r8, 56(%%rax)\n"
			"movq %%rsi, 64(%%rax)\n"
			"movq %%rdi, 72(%%rax)\n"
			"movq %%rbp, 80(%%rax)\n"
			"movq %%rdx, 88(%%rax)\n"
			"pop %%rbx\n"              // 저장된 rcx
			"movq %%rbx, 96(%%rax)\n"
			"pop %%rbx\n"              // 저장된 rbx
			"movq %%rbx, 104(%%rax)\n"
			"pop %%rbx\n"              // 저장된 rax
			"movq %%rbx, 112(%%rax)\n"
			"addq $120, %%rax\n"
			"movw %%es, (%%rax)\n"
			"movw %%ds, 8(%%rax)\n"
			"addq $32, %%rax\n"
			"call __next\n"         // 현재 rip를 읽음.
			"__next:\n"
			"pop %%rbx\n"
			"addq $(out_iret -  __next), %%rbx\n"
			"movq %%rbx, 0(%%rax)\n" // rip
			"movw %%cs, 8(%%rax)\n"  // cs
			"pushfq\n"
			"popq %%rbx\n"
			"mov %%rbx, 16(%%rax)\n" // eflags
			"mov %%rsp, 24(%%rax)\n" // rsp
			"movw %%ss, 32(%%rax)\n"
			"mov %%rcx, %%rdi\n"
			"call do_iret\n"
			"out_iret:\n"
			: : "g"(tf_cur), "g" (tf) : "memory"
			);
}

/* 새 프로세스를 스케줄. 진입 시 인터럽트는 꺼져 있어야 함.
 * 이 함수는 현재 스레드의 상태를 status로 수정한 다음
 * 실행할 다른 스레드를 찾아 전환함.
 * schedule() 내에서 printf()를 호출하는 것은 안전하지 않음. */
static void
do_schedule(int status) {
	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (thread_current()->status == THREAD_RUNNING);
	while (!list_empty (&destruction_req)) {
		struct thread *victim =
			list_entry (list_pop_front (&destruction_req), struct thread, elem);
		palloc_free_page(victim);
	}
	thread_current ()->status = status;
	schedule ();
}

static void
schedule (void) {
	struct thread *curr = running_thread ();
	struct thread *next = next_thread_to_run ();

	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (curr->status != THREAD_RUNNING);
	ASSERT (is_thread (next));
	/* 실행 중으로 표시. */
	next->status = THREAD_RUNNING;

	/* 새 타임 슬라이스 시작. */
	thread_ticks = 0;

#ifdef USERPROG
	/* 새 주소 공간 활성화. */
	process_activate (next);
#endif

	if (curr != next) {
		/* 전환된 스레드가 dying 상태라면 struct thread를 소멸.
		   이는 thread_exit()가 자신의 발밑을 빼지 않도록 늦게 발생해야 함.
		   페이지가 현재 스택에 의해 사용되고 있으므로 여기서는
		   페이지 해제 요청만 큐에 넣음.
		   실제 소멸 로직은 schedule()의 시작 부분에서 호출될 것임. */
		if (curr && curr->status == THREAD_DYING && curr != initial_thread) {
			ASSERT (curr != next);
			list_push_back (&destruction_req, &curr->elem);
		}

		/* 스레드를 전환하기 전에, 먼저 현재 실행 중인 정보를 저장. */
		thread_launch (next);
	}
}

/* 새 스레드에 사용할 tid를 반환. */
static tid_t
allocate_tid (void) {
	static tid_t next_tid = 1;
	tid_t tid;

	lock_acquire (&tid_lock);
	tid = next_tid++;
	lock_release (&tid_lock);

	return tid;
}
