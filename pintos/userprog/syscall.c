#include "userprog/syscall.h"
#include "userprog/process.h"
#include "threads/synch.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "threads/init.h"
#include "threads/vaddr.h"
#include "filesys/file.h"
#include "filesys/filesys.h"

/* TODO : 임시 방편 전역 변수 -> 나중에 반드시 수정해야 함*/
extern struct semaphore tmp_sema;

void syscall_entry (void);
void syscall_handler (struct intr_frame *);
static void sys_halt (void); 
static void sys_exit (int status);
static int sys_write (int fd, const void *buffer, unsigned length);
static int sys_create(const char *file, unsigned initial_size);


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
syscall_handler (struct intr_frame *f ) {
	// TODO: Your implementation goes here.
	uint64_t syscall_num= f -> R.rax;
	switch(syscall_num){
		case SYS_HALT:
			sys_halt();
			break;
		case SYS_EXIT:
			sys_exit((int) f -> R.rdi);
			break;
		case SYS_WRITE:
			f -> R.rax = sys_write((int) f -> R.rdi, (const void *) f -> R.rsi, (unsigned int) f -> R.rdx);
			break;
		case SYS_CREATE:
			f -> R.rax = sys_create((const char *) f -> R.rdi, (unsigned) f -> R.rsi);
			break;
		default:
			sys_exit(-1);
			break;
	}
}

static void sys_halt (void){
	power_off();
}

static void sys_exit (int status){
	struct thread *cur = thread_current();
	printf("%s: exit(%d)\n", cur -> name, status);
	/* TODO : 임시 방편 나중에 반드시 수정 */
	sema_up(&tmp_sema);
	thread_exit();
}

static int sys_write (int fd, const void *buffer, unsigned length){
	if(fd == 1){
		putbuf(buffer, length);
		return (int) length;
	}
	return -1;
}

static int sys_create(const char *file, unsigned initial_size){
	if(file == NULL){
		sys_exit(-1);
		// return 0;
	}

	bool result = filesys_create(file, initial_size);
	if(!result) {
		sys_exit(-1);
		// return 0;
	} 
	return 1;
}