#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "threads/init.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/palloc.h"
#include <string.h>

void syscall_entry (void);
void syscall_handler (struct intr_frame *);
static void halt(void);
static int write (int fd, const void *buffer, unsigned length);
static int create(const char *file, unsigned initial_size);
static void exit(int status);
static int open(const char *file);
static void check_valid_access(void *uaddr);

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

void
syscall_init (void) {
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f UNUSED) {
	uint64_t syscall_num = f -> R.rax;
	switch (syscall_num)
	{	
		case SYS_HALT:
			halt();
			break;
	
		case SYS_WRITE:
			/* code */
			f->R.rax = write((int)f->R.rdi, (const void *)f->R.rsi, (unsigned int)f->R.rdx);
			break;

		case SYS_EXIT:
			exit((int) f -> R.rdi);

        	break;

		case SYS_CREATE:
			f -> R.rax = create((const char *) f -> R.rdi, (unsigned) f -> R.rsi);
			break;

		case SYS_OPEN:
			f -> R.rax = open((const char *) f -> R.rdi);
			break;


		default:
			exit(-1);
			break;
	}
}

static void 
halt(void){
	power_off();
}

static void
exit(int status){
	thread_current()-> exit_status = status;
    thread_exit();
}


static int 
write (int fd, const void *buffer, unsigned length){
	if(fd == 1 || fd == 2){
		putbuf(buffer, length);
		return length;
	}
	return -1;
};

static int 
create(const char *file, unsigned initial_size){
	check_valid_access(file);

	bool result = filesys_create(file, initial_size);
	if(!result || strlen(file) > 14){
		return 0;
	}
	return 1;
}

/* 파일 식별자로 변환하고 식별자 번호를 리턴한다. */
static int
open(const char *file){
	check_valid_access(file);
	struct thread *cur = thread_current();
	int fd = cur -> fd;
	if(fd >= MAX_FD || file == NULL){
		return -1;
	}
	char *fn_copy = palloc_get_page(0);
	if(fn_copy == NULL){
		return -1;
	}
	strlcpy(fn_copy, file, PGSIZE);

	struct file *new_file = filesys_open(fn_copy);
	if(!new_file){
		palloc_free_page(fn_copy);
		return -1;
	}
	cur -> fd_table[fd] = new_file;
	cur -> fd = fd + 1;
	palloc_free_page(fn_copy);
	return fd;
}

static void 
check_valid_access(void *uaddr){
	struct thread *cur = thread_current();
	if(uaddr == NULL) exit(-1);
	if(pml4_get_page(cur -> pml4, uaddr) == NULL) exit(-1);
	if(!is_user_vaddr(uaddr)) exit(-1);
}