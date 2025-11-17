#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "threads/init.h"
#include "filesys/filesys.h"
#include "threads/vaddr.h"
#include "include/filesys/file.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

// System call handler functions
void halt(void);
void exit(int status);
bool create(const char *file, unsigned initial_size);
int write(int fd, const void *buffer, unsigned length);
int open(const char *file);

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
	// TODO: Your implementation goes here.

	const char *file = f->R.rdi;

	switch (f->R.rax)
	{
	case SYS_WRITE:
		/* code */
		f->R.rax = write(f->R.rdi, f->R.rsi, f->R.rdx);
		break;
	
	case SYS_HALT:
		halt();
		break;

	case SYS_OPEN:
		f->R.rax = open(file);
		break;

	default:
		break;
	}
}

int open (const char *file){
	if (file == NULL || !is_user_vaddr(file)) {
        exit(-1);
    }
	struct thread *curr = thread_current();
	struct file *opened_file = filesys_open(file);

	if (opened_file != NULL){
		for(int i = 3; i < 64; i ++){
			if(curr->fdt[i] == NULL){
				curr->fdt[i] = opened_file;
				return i;
			}
		}
		file_close(opened_file);
	}else{
		return -1;
	}
	return -1;
};

void halt (void){
	power_off();
};


int write (int fd, const void *buffer, unsigned length){
	if(fd == 1 || fd == 2){
		putbuf(buffer, length);
		return length;
	}

	return 0;
};
