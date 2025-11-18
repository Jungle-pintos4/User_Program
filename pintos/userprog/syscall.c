#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "filesys/filesys.h"
#include "string.h"
#include "filesys/file.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

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

void halt(void);
void exit(int status);
int write (int fd, const void *buffer, unsigned length);
bool create (char *file, unsigned initial_size);
int open (const char *file);
void close (int fd); 
int read (int fd, void *buffer, unsigned length);
int filesize (int fd);
bool check_addr(void *addr);
bool check_buffer(void *buffer, int length);

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
	switch (f->R.rax)
	{
	case SYS_HALT:
		halt();
		break;

	case SYS_WRITE:
		int fd_to_write = (int)f->R.rdi;
		const void *buffer_to_write = (void *)f->R.rsi;
		unsigned length_to_write = (unsigned) f->R.rdx;

		if (check_buffer(buffer_to_write, (int) length_to_write) == false) 
			exit(-1);

		f->R.rax = write(fd_to_write, buffer_to_write, length_to_write);
		break;

	case SYS_EXIT:
		int status = (int)f->R.rdi;

		exit(status);
        break;

	case SYS_CREATE:
		char *file_to_create = (char *)f->R.rdi;
		unsigned initial_size = (unsigned)f->R.rsi;
		
		if (check_addr(file_to_create) == false)
			exit(-1);

		f->R.rax = create(file_to_create, initial_size);
		break;

	case SYS_OPEN:
		char *file_to_open = (char *)f->R.rdi;

		if (check_addr(file_to_open) == false) 
			exit(-1);

		f->R.rax = open(file_to_open);
		break;

	case SYS_CLOSE:
		int fd_to_close = (int)f->R.rdi;
		
		close(fd_to_close);
		break;

	case SYS_READ:
		int fd_to_read = (int)f->R.rdi;
		const void *buffer_to_read = (void *)f->R.rsi;
		unsigned length_to_read = (unsigned) f->R.rdx;

		if (check_buffer(buffer_to_read, length_to_read) == false) 
			exit(-1);

		f->R.rax = read(fd_to_read, buffer_to_read, length_to_read);
		break;

	case SYS_FILESIZE:
		int fd_to_filesize = (int)f->R.rdi;
		
		f->R.rax = filesize(fd_to_filesize);
		break;

	default:
		break;
	}
}

void halt(void) {
	power_off();
}

void exit(int status) {	
	thread_current()->exit_status = status;
	thread_exit();
}

int write (int fd, const void *buffer, unsigned length) {
	if (fd <= 0 || fd > 64) {
		exit(-1);
	}

	if (fd == 1 || fd == 2){
		putbuf(buffer, length);
		return length;
	}

	struct file *target_file = thread_current()->fd_table[fd];
	if (!target_file || !buffer) {
		return 0;
	}

	int writen_size = (int) file_write(target_file, buffer, (off_t) length);
	return writen_size;
};

bool create (char *file, unsigned initial_size) {
	if (!file) // NULL 포인터이면, 바로 exit()
		exit(-1);

	if (strlen(file) == 0 || strlen(file) > 14) // 파일명이 0글자이거나, 14글자보다 크면 생성하지 않음
		return false;

	if (filesys_open(file)) { // 이미 존재하는 파일
		return false;
	}

	bool result = filesys_create(file, (off_t) initial_size);
	return result;
}

int open (const char *file) {
	if (!file)
		exit(-1);

	struct file *target_file = filesys_open(file);
	if (strlen(file) == 0 || !target_file) // 파일을 여는데 실패
		return -1;

	struct thread *curr = thread_current();
	int result = -1;

	for (int index = 3; index <= 64; index ++) {
		if (curr->fd_table[index] == NULL) {
			curr->fd_table[index] = target_file;
			result = index;
			break;
		}
	}

	return result;
}

void close(int fd) {
	if (fd <= 0 || fd >= 64) {
		exit(-1);
	}

	if (fd == 1 || fd == 2) 
		return ;

	struct file *target_file = thread_current()->fd_table[fd];
	if (!target_file) 
		return ;
	file_close(target_file);
	thread_current()->fd_table[fd] = NULL;
}

int read (int fd, void *buffer, unsigned length) {
	if (fd < 0 || fd > 64) {
		exit(-1);
	}

	if (fd == 0) {
		for (int i=0; i<length; i++) {

			((char *)buffer) [i] = input_getc();
		}
		return length;
	}

	struct file *target_file = thread_current()->fd_table[fd];
	if (!target_file || !buffer) {
		return -1;
	}

	int result = (int) file_read(target_file, buffer, (off_t) length);
	return result;
}

int filesize (int fd) {
	if (fd < 0 || fd > 64) {
		exit(-1);
	}

	struct file *target_file = thread_current()->fd_table[fd];
	if (!target_file) {
		return -1;
	}

	return (int) file_length(target_file);
}

bool check_addr(void *addr) {
	if (!addr) return false;
	if (is_user_vaddr(addr) == false) return false;
	if (pml4_get_page(thread_current()->pml4, addr) == false) return false;
	return true;
}

bool check_buffer(void *buffer, int length) {
	for (int i = 0; i < length; i++) {
		void *current_addr = ((char *)buffer) + i;

		if (check_addr(current_addr) == false) {
			return false;
		}
	}

	return true;
}
