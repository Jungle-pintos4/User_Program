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
int read (int fd, void *buffer, unsigned length);
int filesize(int fd);
void close (int fd);

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
		f->R.rax = write(f->R.rdi, f->R.rsi, f->R.rdx);
		break;

	case SYS_EXIT:
		int status = f->R.rdi;
		exit(status);
        break;
	
	case SYS_HALT:
		halt();
		break;
	
	case SYS_CREATE:
		f->R.rax = create(file, (unsigned)f->R.rsi);
		break;

	case SYS_OPEN:
		f->R.rax = open(file);
		break;

	case SYS_READ:
		f->R.rax = read(f->R.rdi, f->R.rsi, f->R.rdx);
		break;

	case SYS_FILESIZE:
		f-> R.rax = filesize(f -> R.rdi);
		break;
	
	case SYS_CLOSE:
		close(f -> R.rdi);
		break;

	default:
		break;
	}
}

int open (const char *file){
	if (file == NULL || !is_user_vaddr(file)) {
        exit(-1);
    }
	
	if (pml4_get_page(thread_current()->pml4, file) == NULL) {
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

void exit (int status){
	thread_current()->exit_status = status;
    thread_exit();
};

bool create(const char *file, unsigned initial_size) {
    // NULL이거나 커널 영역이면 종료
	// bad ptr -> pml4 valideate 판독해야됌 귀찮음
	if (file == NULL || !is_user_vaddr(file)) {
        exit(-1);
    }
	if (pml4_get_page(thread_current()->pml4, file) == NULL){
		exit(-1);
	}

    // 정상 케이스
	return filesys_create(file, initial_size);
}

int read (int fd, void *buffer, unsigned length){
	struct thread *curr = thread_current();
	if (!is_user_vaddr(buffer)) {
        exit(-1);
    }

	if (pml4_get_page(thread_current()->pml4, buffer) == NULL) {
		exit(-1);
	}

	if (fd == 0){
		for(int i=0; i <length; i++){
			((char *)buffer)[i] = input_getc();
		}
		return length;
	}else{
		if(fd < 3 || fd > 64){
			exit(-1);
		}
		struct file *file = curr->fdt[fd];
			if (file == NULL) {
        		exit(-1);
    		}
		return file_read(file, buffer, length);
	}

	return -1;
};

int write (int fd, const void *buffer, unsigned length){
	struct thread *curr = thread_current();
	if(!is_user_vaddr(buffer)) {
        	exit(-1);
    }

	if (pml4_get_page(thread_current()->pml4, buffer) == NULL) {
		exit(-1);
	}

	if(fd == 1 || fd == 2){
		putbuf(buffer, length);
		return length;
	}else{
		if(fd < 3 || fd > 64){
			exit(-1);
		}
		struct file *file = curr->fdt[fd];
		if (file == NULL) {
        	exit(-1);
    	}

		return file_write(file, buffer, length);
	}

	return 0;
};

int filesize(int fd){
	if(fd < 2 || fd > 64){
		exit(-1);
	}
	struct thread *curr = thread_current();
	struct file *file = curr->fdt[fd];

	return file_length(file);
}

void close (int fd){
	if(fd < 3 || fd > 64){
		exit(-1);
	}
	struct thread *curr = thread_current();
	struct file *file = curr->fdt[fd];

	if (file == NULL) {  // 이미 닫힌 파일
    	exit(-1);
    }

	file_close(file);

	curr->fdt[fd] = NULL;
};