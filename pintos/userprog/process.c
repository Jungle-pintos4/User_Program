#include "userprog/process.h"
#include "userprog/syscall.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#ifdef VM
#include "vm/vm.h"
#include "synch.h"
#endif

static void process_cleanup (void);
static bool load (const char *file_name, struct intr_frame *if_);
static void initd (void *f_name);
static void __do_fork (void *);

static void argument_passing(char *argv[], int argc, struct intr_frame *frame);

// fork할 때 인자값으로 전달하는 struct 구조 선언
struct args {
	struct thread *curr;
	struct intr_frame *f;
	struct semaphore create_sema;
};

/* General process initializer for initd and other process. */
/* 프로세스가 생성될 때 (initd, fork 시) 호출될 수 있으며, 
프로세스마다 독립적인 존재해야 할 자원을 설정하는 것이다. 예시 -> 파일 디스크립터 */
static void
process_init (void) {
	struct thread *current = thread_current ();
	current -> fd_table = malloc(sizeof (struct file *) * MAX_FD);
	if(current -> fd_table == NULL){
		PANIC("fd_table allocation failed");
	}
	for(int i = 0; i < MAX_FD; i++){
		current -> fd_table[i] = NULL;
	}
}

/* Starts the first userland program, called "initd", loaded from FILE_NAME.
 * The new thread may be scheduled (and may even exit)
 * before process_create_initd() returns. Returns the initd's
 * thread id, or TID_ERROR if the thread cannot be created.
 * Notice that THIS SHOULD BE CALLED ONCE. */

/* 첫 번째 유저 프로세스를 실행하기 위한 초기 커널 쓰레드 생성*/
tid_t
process_create_initd (const char *file_name) {
	char *fn_copy;
	char *fn_copy2;
	tid_t tid;

	/* Make a copy of FILE_NAME.
	 * Otherwise there's a race between the caller and load(). */
	fn_copy = palloc_get_page (0);
	if (fn_copy == NULL)
		return TID_ERROR;
	/* initd 함수에 전달할 인수 */
	strlcpy (fn_copy, file_name, PGSIZE);

	// 프로그램 이름 추출용 복사본
	fn_copy2 = palloc_get_page (0);
	if (fn_copy2 == NULL) {
		palloc_free_page(fn_copy);
		return TID_ERROR;
	}
	strlcpy (fn_copy2, file_name, PGSIZE);

	char *program, *save_ptr;
	program = strtok_r(fn_copy2, " ", &save_ptr);


	/* 새로운 User 프로그램을 실행할 쓰레드 생성 (아직은 커널 쓰레드)*/
	tid = thread_create (program, PRI_DEFAULT, initd, fn_copy);

	// 파싱용 복사본은 바로 해제
	palloc_free_page(fn_copy2);

	if (tid == TID_ERROR)
		/* 생성 실패 시 바로 자원 반납*/
		palloc_free_page (fn_copy);
	/* process_wait(메인 쓰레드)는 해당 쓰레드가 종료될 때(유저 프로세스)까지 wait함 */
	return tid;
}

/* A thread function that launches first user process. */
static void
initd (void *f_name) {
#ifdef VM
	supplemental_page_table_init (&thread_current ()->spt);
#endif

	process_init ();

	if (process_exec (f_name) < 0)
		PANIC("Fail to launch initd\n");
	NOT_REACHED ();
}

/* Clones the current process as `name`. Returns the new process's thread id, or
 * TID_ERROR if the thread cannot be created. */
tid_t
process_fork (const char *name, struct intr_frame *if_ UNUSED) {
	/* Clone current thread to new thread.*/
	struct args *args = malloc(sizeof(struct args));
	if (!args) {
		printf("errors while executing process_fork, temp \n");
		return TID_ERROR;
	}
		
	args->curr = thread_current();
	args->f = if_;
	sema_init(&(args->create_sema), 0);

	tid_t result = thread_create (name, PRI_DEFAULT, __do_fork, args);
	sema_down(&args->create_sema);

	free(args);
	args = NULL;

	return result;
}

#ifndef VM
/* Duplicate the parent's address space by passing this function to the
 * pml4_for_each. This is only for the project 2. 
 * 
 * 부모의 페이지 테이블 엔트리 (pte)를 복사해 자식에게 덮어씌우는 함수
 * pte : 페이지 테이블 엔트리
 * va  : 가상 주소
 * aux : 모름
 */
static bool
duplicate_pte (uint64_t *pte, void *va, void *aux) {
	struct thread *current = thread_current ();
	struct thread *parent = (struct thread *) aux;
	void *parent_page;
	void *new_page;
	bool writable;

	/* 1. TODO: If the parent_page is kernel page, then return immediately. */
	if (is_kernel_vaddr(va) == true) { // 유저 주소 (va)가 커널 주소이면, 복사하지 않고 건너뜀 (true 반환)
		return true;
	}

	/* 2. Resolve VA from the parent's page map level 4. */
	parent_page = pml4_get_page (parent->pml4, va); // 유저 주소 (va)를 토대로 커널 주소를 찾음 (parent_page)

	if (parent_page == NULL) { // 유효하지 않은 주소
		// 이러면 어카지
		return false;
	}
	/* 3. TODO: Allocate new PAL_USER page for the child and set result to
	 *    TODO: NEWPAGE. */

	void *result = palloc_get_page(PAL_USER | PAL_ZERO); // 자식 프로세스를 위해 새롭게 물리 페이지를 할당받음
	if (result == NULL) {
		// 이러면 어카냐
		return false;
	}

	/* 4. TODO: Duplicate parent's page to the new page and
	 *    TODO: check whether parent's page is writable or not (set WRITABLE
	 *    TODO: according to the result). */
	new_page = memcpy(result, parent_page, PGSIZE);
	writable = is_writable(pte);

	/* 5. Add new page to child's page table at address VA with WRITABLE
	 *    permission. */
	if (!pml4_set_page (current->pml4, va, new_page, writable)) { // 물리 페이지를 자식 프로세스의 pml4에 등록함
		/* 6. TODO: if fail to insert page, do error handling. */
		// 이것도 어카냐
		return false;
	}
	return true;
}
#endif

/* A thread function that copies parent's execution context.
 * Hint) parent->tf does not hold the userland context of the process.
 *       That is, you are required to pass second argument of process_fork to
 *       this function. */
static void
__do_fork (void *aux) {
	struct args *args_ptr = (struct args *) aux;
	struct intr_frame if_;
	struct thread *parent = args_ptr->curr;
	struct thread *current = thread_current ();
	/* TODO: somehow pass the parent_if. (i.e. process_fork()'s if_) */
	struct intr_frame *parent_if = args_ptr->f;
	bool succ = true;
	struct semaphore *create_sema = &(args_ptr->create_sema);

	/* 1. Read the cpu context to local stack. */
	memcpy (&if_, parent_if, sizeof (struct intr_frame));

	/* 2. Duplicate PT */
	current->pml4 = pml4_create();
	if (current->pml4 == NULL) {
		goto error;
	}

	process_activate (current);
#ifdef VM
	supplemental_page_table_init (&current->spt);
	if (!supplemental_page_table_copy (&current->spt, &parent->spt))
		goto error;
#else
	if (!pml4_for_each (parent->pml4, duplicate_pte, parent)) {
		goto error;
	}
		
#endif
	process_init (); // 여기서 먼저 fd_table 세팅하고

	/* TODO: Your code goes here.
	 * TODO: Hint) To duplicate the file object, use `file_duplicate`
	 * TODO:       in include/filesys/file.h. Note that parent should not return
	 * TODO:       from the fork() until this function successfully duplicates
	 * TODO:       the resources of parent.*/
	
	for(int i = 0; i < MAX_FD; i++){
		if (parent->fd_table[i] != NULL) {
			struct file *new_file = file_duplicate(parent->fd_table[i]);
			current->fd_table[i] = new_file;
		}
	}

	/* Finally, switch to the newly created process. */
	if (succ) {
		if_.R.rax = 0;
		sema_up(create_sema);
		do_iret (&if_);
	}

error:
	sema_up(create_sema);
	thread_exit ();
}

/* Switch the current execution context to the f_name.
 * Returns -1 on fail. */
int
process_exec (void *f_name) {
	char *file_name = palloc_get_page(0);
	strlcpy(file_name, f_name, PGSIZE);
	bool success;

	/* We cannot use the intr_frame in the thread structure.
	 * This is because when current thread rescheduled,
	 * it stores the execution information to the member. */
	struct intr_frame _if;
	_if.ds = _if.es = _if.ss = SEL_UDSEG;
	_if.cs = SEL_UCSEG;
	_if.eflags = FLAG_IF | FLAG_MBS;

	/* We first kill the current context */
	process_cleanup ();

	/* And then load the binary */
	success = load (file_name, &_if);

	char *save_ptr;
	strtok_r(file_name, " ", &save_ptr);

	// 파싱된 file_name으로 filesys_open을 시도
	struct file* curr_file = filesys_open(file_name);
	if (curr_file != NULL) {
		// file_name이 유효한 값일 때만, executable_file 갱신 및 deny_write 설정
		thread_current()->executable_file = curr_file;
		file_deny_write(curr_file);
	}

	/* If load failed, quit. */
	palloc_free_page (file_name);
	if (!success)
		return -1;

	/* Start switched process. */
	do_iret (&_if);
	NOT_REACHED ();
}


/* Waits for thread TID to die and returns its exit status.  If
 * it was terminated by the kernel (i.e. killed due to an
 * exception), returns -1.  If TID is invalid or if it was not a
 * child of the calling process, or if process_wait() has already
 * been successfully called for the given TID, returns -1
 * immediately, without waiting.
 *
 * This function will be implemented in problem 2-2.  For now, it
 * does nothing. */
int
process_wait (tid_t child_tid) {
	struct thread *curr = thread_current();
    struct thread *child = NULL;
    struct list temp_list;
    
	list_init(&temp_list);

	struct list_elem *target = list_begin(&curr -> child_list);
	while(target != list_end(&curr -> child_list)){
		struct thread *t = list_entry(target, struct thread, child_elem);
		if(t -> tid == child_tid){
			child = t;
			list_remove(target);
			break;
		}
		target = list_next(target);
	}

    if (child == NULL) {
        return -1;
    }

	sema_down(&child->wait_sema);
    return child->exit_status;
}

/* Exit the process. This function is called by thread_exit (). */
void
process_exit (void) {
	struct thread *curr = thread_current ();

	// Process termination message
	printf("%s: exit(%d)\n", curr->name, curr->exit_status);
	sema_up(&curr->wait_sema);

	if(curr -> fd_table != NULL){
		for(int i = 0; i < MAX_FD; i++){
			if(curr -> fd_table[i] != NULL){
				lock_acquire(&filesys_lock);
				file_close(curr -> fd_table[i]);
				lock_release(&filesys_lock);
			}
		}
		free(curr -> fd_table);
		curr -> fd_table = NULL;
	}

	struct file *curr_file = curr->executable_file;
	if (curr_file != NULL) {
		file_close(curr_file);
	}

	process_cleanup ();
}

/* Free the current process's resources. */
static void
process_cleanup (void) {
	struct thread *curr = thread_current ();

#ifdef VM
	supplemental_page_table_kill (&curr->spt);
#endif
	uint64_t *pml4;
	/* Destroy the current process's page directory and switch back
	 * to the kernel-only page directory. */
	pml4 = curr->pml4;
	if (pml4 != NULL) {
		/* Correct ordering here is crucial.  We must set
		 * cur->pagedir to NULL before switching page directories,
		 * so that a timer interrupt can't switch back to the
		 * process page directory.  We must activate the base page
		 * directory before destroying the process's page
		 * directory, or our active page directory will be one
		 * that's been freed (and cleared). */
		curr->pml4 = NULL;
		pml4_activate (NULL);
		pml4_destroy (pml4);
	}
}

/* Sets up the CPU for running user code in the nest thread.
 * This function is called on every context switch. */
void
process_activate (struct thread *next) {
	/* Activate thread's page tables. */
	pml4_activate (next->pml4);

	/* Set thread's kernel stack for use in processing interrupts. */
	tss_update (next);
}

/* We load ELF binaries.  The following definitions are taken
 * from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
#define EI_NIDENT 16

#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
 * This appears at the very beginning of an ELF binary. */
struct ELF64_hdr {
	unsigned char e_ident[EI_NIDENT];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
};

struct ELF64_PHDR {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

/* Abbreviations */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack (struct intr_frame *if_);
static bool validate_segment (const struct Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes,
		bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
 * Stores the executable's entry point into *RIP
 * and its initial stack pointer into *RSP.
 * Returns true if successful, false otherwise. */
static bool
load (const char *file_name, struct intr_frame *if_) {
	struct thread *t = thread_current ();
	struct ELF ehdr;
	struct file *file = NULL;
	off_t file_ofs;
	bool success = false;
	int i;

	// 일단 file_name이 들어오면 copy해서 parsing해보자
	// palloc으로 file name이 들어가는 single page 하나 생성
	char *fn_copy = palloc_get_page(0);

	if (fn_copy == NULL)
		return false;

	// pintos는 보안 이슈로 strcpy 사용 불가
	strlcpy(fn_copy, file_name, PGSIZE);

	char *argv[128];
	int argc = 0;
	char *token, *save_ptr;

	// 파싱: argv 배열에 토큰들 저장
	for (token = strtok_r(fn_copy, " ", &save_ptr); token != NULL;
	     token = strtok_r(NULL, " ", &save_ptr)) {
		argv[argc++] = token;
	}

	/* 페이지 테이블의 최상위 목차를 만들기(커널 주소 공간 매핑, 유저 공간은 비어 있음) */
	t->pml4 = pml4_create ();
	if (t->pml4 == NULL)
		goto done;
	process_activate (thread_current ());

	/* Open executable file. */
	file = filesys_open (argv[0]);
	if (file == NULL) {
		printf ("load: %s: open failed\n", argv[0]);
		goto done;
	}

	/* ELF 헤더 검증 -> ELF 헤더를 ehdr 구조체에 저장   */
	if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
			|| memcmp (ehdr.e_ident, "\177ELF\2\1\1", 7) /* ELF 파일이 맞는지 매직 넘버 확인*/
			|| ehdr.e_type != 2						
			|| ehdr.e_machine != 0x3E // amd64
			|| ehdr.e_version != 1
			|| ehdr.e_phentsize != sizeof (struct Phdr)
			|| ehdr.e_phnum > 1024) {
		printf ("load: %s: error loading executable\n", file_name);
		goto done;
	}

	/* Read program headers. -> 프로그램 헤더 테이블 읽어서 각 세그먼트마다 메모리 적재  */
	file_ofs = ehdr.e_phoff;
	/* 프로그램 헤더의 개수 */
	for (i = 0; i < ehdr.e_phnum; i++) {
		struct Phdr phdr;

		if (file_ofs < 0 || file_ofs > file_length (file))
			goto done;
		file_seek (file, file_ofs);

		if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
			goto done;
		file_ofs += sizeof phdr;
		switch (phdr.p_type) {
			case PT_NULL:
			case PT_NOTE:
			case PT_PHDR:
			case PT_STACK:
			default:
				/* Ignore this segment. */
				break;
			case PT_DYNAMIC:
			case PT_INTERP:
			case PT_SHLIB:
				goto done;
			case PT_LOAD:
				if (validate_segment (&phdr, file)) {
					bool writable = (phdr.p_flags & PF_W) != 0;
					uint64_t file_page = phdr.p_offset & ~PGMASK;
					uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
					uint64_t page_offset = phdr.p_vaddr & PGMASK;
					uint32_t read_bytes, zero_bytes;
					if (phdr.p_filesz > 0) {
						/* Normal segment.
						 * Read initial part from disk and zero the rest. */
						read_bytes = page_offset + phdr.p_filesz;
						zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
								- read_bytes);
					} else {
						/* Entirely zero.
						 * Don't read anything from disk. */
						read_bytes = 0;
						zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
					}
					/* 세그먼트 단위로 가상 메모리에 적재 */
					/* file: 실행 파일을 다룰 핸들, ofs: 목적 파일의 어디서부터 읽을지(offset), upage: 가상 메모리에 어디에 올릴지*/
					/* read_bytes: 파일에서 몇 바이트를 읽을 지, zero_bytes: 나머지 부분을 0으로 채울 바이트 수, writeable: 이 메모리 영역에 쓰기 가능한지 여부  */
					if (!load_segment (file, file_page, (void *) mem_page,
								read_bytes, zero_bytes, writable))
						goto done;
				}
				else
					goto done;
				break;
		}
	}

	/* 유저 스택 생성 및 설정 */
	if (!setup_stack (if_))
		goto done;

	/* Start address. */
	if_->rip = ehdr.e_entry;

	/* TODO: Your code goes here.
	 * TODO: Implement argument passing (see project2/argument_passing.html). */
	argument_passing(argv, argc, if_);

	success = true;

done:
	/* We arrive here whether the load is successful or not. */
	if(file != NULL){
		file_close(file);
		// file_name을 실행할 것이므로, file_name에 쓸 수 없게 만든다 (ROX)
		// file_deny_write(file);
		// printf("process.c: file_deny_write, %s \n", argv[0]);
	}
	if(fn_copy != NULL){
		palloc_free_page(fn_copy);
	}
	return success;
}

void argument_passing(char *argv[], int argc, struct intr_frame *frame){
	// 인자 주소들을 저장할 배열
	char *arg_addresses[128];

	// 인자 문자열들을 스택에 push (역순)
	for (int i = argc - 1; i >= 0; i--) {
		int len = strlen(argv[i]) + 1;
		// 공간 확보 : 여기 len 바이트만큼 공간 쓸게
		frame->rsp -= len;
		// 실제 데이터 삽입
		memcpy(frame->rsp, argv[i], len);
		/* 인자 문자열 삽입 후 argv[argc - 1] ~ argv[0] 삽입을 위해 백업*/
        arg_addresses[i] = frame->rsp;
	}

	while(frame->rsp % 8 != 0){
		frame->rsp--;
	}

	// 64bit 설정이기에 8byte단위의 레지스터 설정
	// argv[argc] = NULL
	frame->rsp -= 8;
	*(uint64_t *)frame->rsp = 0;

    // argv[i] 포인터들 push (역순)
    for (int i = argc - 1; i >= 0; i--) {
          frame->rsp -= 8;
          *(uint64_t *)frame->rsp = (uint64_t)arg_addresses[i];
    }

    // return address (fake) 
    frame->rsp -= 8;
    *(uint64_t *)frame->rsp = 0;

    // rdi(argc), rsi(argv) 설정
    frame->R.rdi = argc;
	// argv 배열의 시작 주소
    frame->R.rsi = frame->rsp + 8;
}


/* Checks whether PHDR describes a valid, loadable segment in
 * FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Phdr *phdr, struct file *file) {
	/* p_offset and p_vaddr must have the same page offset. */
	if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
		return false;

	/* p_offset must point within FILE. */
	if (phdr->p_offset > (uint64_t) file_length (file))
		return false;

	/* p_memsz must be at least as big as p_filesz. */
	if (phdr->p_memsz < phdr->p_filesz)
		return false;

	/* The segment must not be empty. */
	if (phdr->p_memsz == 0)
		return false;

	/* The virtual memory region must both start and end within the
	   user address space range. */
	if (!is_user_vaddr ((void *) phdr->p_vaddr))
		return false;
	if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
		return false;

	/* The region cannot "wrap around" across the kernel virtual
	   address space. */
	if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
		return false;

	/* Disallow mapping page 0.
	   Not only is it a bad idea to map page 0, but if we allowed
	   it then user code that passed a null pointer to system calls
	   could quite likely panic the kernel by way of null pointer
	   assertions in memcpy(), etc. */
	if (phdr->p_vaddr < PGSIZE)
		return false;

	/* It's okay. */
	return true;
}

#ifndef VM
/* Codes of this block will be ONLY USED DURING project 2.
 * If you want to implement the function for whole project 2, implement it
 * outside of #ifndef macro. */

/* load() helpers. */
static bool install_page (void *upage, void *kpage, bool writable);

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	file_seek (file, ofs);
	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		/* 이 페이지에 채워야 할 바이트 수를 계산한다.*/
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* Get a page of memory. */
		/* 데이터를 담을 물리 메모리 페이지를 할당받는다.*/
		uint8_t *kpage = palloc_get_page (PAL_USER);
		if (kpage == NULL)
			return false;

		/* 할당받은 페이지에 파일 내용을 읽어 채운다. */
		if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes) {
			palloc_free_page (kpage);
			return false;
		}
		memset (kpage + page_read_bytes, 0, page_zero_bytes);

		/* 물리페이지를 가상 주소와 매핑한다.(페이지 테이블에 기록한다.,)*/
		if (!install_page (upage, kpage, writable)) {
			printf("fail\n");
			palloc_free_page (kpage);
			return false;
		}

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a minimal stack by mapping a zeroed page at the USER_STACK */
static bool
setup_stack (struct intr_frame *if_) {
	uint8_t *kpage;
	bool success = false;
	/* 유저용 물리 페이지 생성 -> 0으로 초기화*/
	kpage = palloc_get_page (PAL_USER | PAL_ZERO);
	if (kpage != NULL) {
		/* 가상 주소와 물리 페이지를 매핑 (페이지 테이블에 반영)*/
		success = install_page (((uint8_t *) USER_STACK) - PGSIZE, kpage, true);
		if (success)
			if_->rsp = USER_STACK;
		else
			palloc_free_page (kpage);
	}
	return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
 * virtual address KPAGE to the page table.
 * If WRITABLE is true, the user process may modify the page;
 * otherwise, it is read-only.
 * UPAGE must not already be mapped.
 * KPAGE should probably be a page obtained from the user pool
 * with palloc_get_page().
 * Returns true on success, false if UPAGE is already mapped or
 * if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable) {
	struct thread *t = thread_current ();

	/* Verify that there's not already a page at that virtual
	 * address, then map our page there. */
	return (pml4_get_page (t->pml4, upage) == NULL
			&& pml4_set_page (t->pml4, upage, kpage, writable));
}
#else
/* From here, codes will be used after project 3.
 * If you want to implement the function for only project 2, implement it on the
 * upper block. */

static bool
lazy_load_segment (struct page *page, void *aux) {
	/* TODO: Load the segment from the file */
	/* TODO: This called when the first page fault occurs on address VA. */
	/* TODO: VA is available when calling this function. */
}

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* TODO: Set up aux to pass information to the lazy_load_segment. */
		void *aux = NULL;
		if (!vm_alloc_page_with_initializer (VM_ANON, upage,
					writable, lazy_load_segment, aux))
			return false;

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a PAGE of stack at the USER_STACK. Return true on success. */
static bool
setup_stack (struct intr_frame *if_) {
	bool success = false;
	void *stack_bottom = (void *) (((uint8_t *) USER_STACK) - PGSIZE);

	/* TODO: Map the stack on stack_bottom and claim the page immediately.
	 * TODO: If success, set the rsp accordingly.
	 * TODO: You should mark the page is stack. */
	/* TODO: Your code goes here */

	return success;
}
#endif /* VM */
