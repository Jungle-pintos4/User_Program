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
#include "devices/input.h"
#include "userprog/process.h"
#include <string.h>

void syscall_entry (void);
void syscall_handler (struct intr_frame *);
static void halt(void);
static int write (int fd, const void *buffer, unsigned length);
static bool create(const char *file, unsigned initial_size);
static void exit(int status);
static int open(const char *file);
static void check_valid_access(void *uaddr);
static void close(int fd);
static int read(int fd, void *buffer, unsigned size);
static int filesize(int fd);
static int wait (tid_t pid);
static tid_t fork (const char *thread_name, struct intr_frame *if_);
static int exec (const char *file);
static void seek (int fd, unsigned position);
//static bool check_buffer(void *buffer, int length);
static int64_t get_user(const uint8_t *uadder);
static bool put_user(uint8_t *udst, uint8_t byte);

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

struct lock filesys_lock;

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
	lock_init(&filesys_lock);
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

		case SYS_CLOSE:
			close((int) f -> R.rdi);
			break;

		case SYS_READ:
			f -> R.rax = read((int) f -> R.rdi, (void *) f -> R.rsi, (unsigned) f -> R.rdx);
			break;
		
		case SYS_FILESIZE:
			f -> R.rax = filesize((int) f -> R.rdi);
			break;
		
		case SYS_WAIT:
			f-> R.rax = wait(f->R.rdi);
			break;
		
		case SYS_FORK:
			tid_t fork_result = fork((const char *) f->R.rdi, f);
      		f->R.rax = fork_result;
      		break;

		case SYS_EXEC:
			tid_t result = exec((const char *) f->R.rdi);
      		f->R.rax = result;
      		break;

		case SYS_SEEK:
			seek((int)f->R.rdi, (unsigned int)f->R.rsi);
			break;

		case SYS_REMOVE:

		default:
			printf("%d", syscall_num); 
			exit(-1);
			break;
	}
}
static void
seek (int fd, unsigned position){
	struct thread *cur = thread_current();
	if(fd <= 1 || fd >= MAX_FD){
		return;
	}

	struct file *cur_file = cur -> fd_table[fd];
	if(cur_file == NULL) return;
	
	file_seek(cur_file, position);
};

static int
exec (const char *file){
	check_valid_access(file);
	struct thread *cur = thread_current();

	char *fn_copy = palloc_get_page(0);
	if(fn_copy == NULL){
		return -1;
	}
	strlcpy(fn_copy, file, PGSIZE);

	int result = process_exec(fn_copy);

	return result;
};

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
	check_valid_access(buffer);
	struct thread *cur = thread_current();
	if(fd <= 0 || fd >= MAX_FD){
		return -1;
	}

	if(fd == 1){
		putbuf(buffer, length);
		return length;
	}
	struct file *cur_file = cur -> fd_table[fd];
	if(cur_file == NULL) return -1;

	lock_acquire(&filesys_lock);
	off_t actual_byte_written = file_write(cur_file, buffer, length);
	lock_release(&filesys_lock);

	return (int) actual_byte_written;
};

static bool 
create(const char *file, unsigned initial_size){
	check_valid_access(file);
	if(strlen(file) > 14){
		return false;
	}

	lock_acquire(&filesys_lock);
	bool result = filesys_create(file, initial_size);
	lock_release(&filesys_lock);
	if(!result){
		return false;
	}
	return true;
}

/* 파일 식별자로 변환하고 식별자 번호를 리턴한다. */
static int
open(const char *file){
	check_valid_access(file);
	struct thread *cur = thread_current();

	char *fn_copy = palloc_get_page(0);
	if(fn_copy == NULL){
		return -1;
	}
	strlcpy(fn_copy, file, PGSIZE);

	
	lock_acquire(&filesys_lock);
	struct file *new_file = filesys_open(fn_copy);
	lock_release(&filesys_lock);


	if(!new_file){
		palloc_free_page(fn_copy);
		return -1;
	}

	int fd = -1;

	for (int i = 2; i < MAX_FD; i++)
	{
		if(cur -> fd_table[i] == NULL){
			cur -> fd_table[i] = new_file;
			fd = i;
			break;
		}
	}

	if(fd < 0) file_close(new_file);

	palloc_free_page(fn_copy);
	return fd;
}

/* TODO : 현재 로직은 시작 주소만 검증하기 때문에, 만약 시작 주소 + 오프셋과 같은 읽기 쓰기에서 주소 침범 문제 발생 가능	
하단의 get_user, put_user로 로직 대체가 필요할 수 있으나, 현재까지 테스트 케이스에서 문제가 되지는 않음.
*/
static void 
check_valid_access(void *uaddr){
	struct thread *cur = thread_current();
	if(uaddr == NULL) exit(-1);
	if(pml4_get_page(cur -> pml4, uaddr) == NULL) exit(-1);
	if(!is_user_vaddr(uaddr)) exit(-1);
}

static void 
close(int fd){
	if(fd < 2 || fd >= MAX_FD){
		return;
	}
	struct thread *cur = thread_current();
	if(cur -> fd_table[fd] != NULL){
		lock_acquire(&filesys_lock);
		file_close(cur -> fd_table[fd]);
		lock_release(&filesys_lock);
		cur -> fd_table[fd] = NULL;
	}
}

static int
filesize(int fd){
	struct thread *cur = thread_current();
	if(fd < 2 || fd >= MAX_FD || cur -> fd_table[fd] == NULL){
		return -1;
	}
	struct file *cur_file = cur -> fd_table[fd];

	lock_acquire(&filesys_lock);
	off_t file_len = file_length(cur_file);
	lock_release(&filesys_lock);
	return (int) file_len;
}

static int 
read(int fd, void *buffer, unsigned size){
	check_valid_access(buffer);
	struct thread *cur = thread_current();
	if(fd < 0 || fd == 1 || fd >= MAX_FD ){
		return -1;
	}

	if(fd == 0){	
		unsigned rd_size = 0;
		uint8_t *buf = (uint8_t *) buffer;

		while(rd_size < size){
			uint8_t ch = input_getc();
			buf[rd_size] = ch;
			rd_size++;
		}
		return rd_size;
	} else {
		struct file *cur_file = cur -> fd_table[fd]; 
		if(cur_file == NULL){
			return -1;
		}
		lock_acquire(&filesys_lock);
		off_t bytes_read = file_read(cur_file, buffer, size);
		lock_release(&filesys_lock);
		return bytes_read;
	}
}

static
int wait (tid_t pid){
	tid_t result = process_wait(pid);
	return result;
};

static tid_t
fork (const char *thread_name, struct intr_frame *if_){
	// 1. 먼저 NULL과 유저 주소 체크
	if(thread_name == NULL || !is_user_vaddr(thread_name)){
		exit(-1);
	}

	// 2. 그 다음 페이지 매핑 확인
	if (pml4_get_page(thread_current()->pml4, thread_name) == NULL) {
		exit(-1);
	}
	tid_t result = process_fork(thread_name, if_);
    return result;

}


/* TODO : 혹시 시작 주소 다음 바이트에 문제가 생기면 사용하기 */
// static bool check_buffer(void *buffer, int length) {
//     for (int i = 0; i < length; i++) {
//         void *current_addr = ((char *)buffer) + i;
//         if (check_addr(current_addr) == false) {
//             return false;
//         }
//     }
//     return true;
// }


/* Reads a byte at user virtual address UADDR.
 * UADDR must be below KERN_BASE.
 * Returns the byte value if successful, -1 if a segfault
 * occurred. */
static int64_t
get_user (const uint8_t *uaddr) {
    int64_t result;
    __asm __volatile (
    "movabsq $done_get, %0\n"  // $done_get의 주소 값을 rax 레지스터에 넣는 명령
    "movzbq %1, %0\n"		   // [uadder] 메모리에서 1바이트 가져와서 8비트 -> 64비트 zero-extend -> rax 레지스터로  
    "done_get:\n"		
    : "=&a" (result) : "m" (*uaddr)); // 출력 0번 rax -> result, 출력 1번 m(memory) -> uadder
    return result;
}

/* Writes BYTE to user address UDST.
 * UDST must be below KERN_BASE.
 * Returns true if successful, false if a segfault occurred. */
static bool
put_user (uint8_t *udst, uint8_t byte) {
    int64_t error_code;
    __asm __volatile (
    "movabsq $done_put, %0\n" // done_put -> rax(폴트 핸들러에서 다시 점프)
    "movb %b2, %1\n"		  // q의 하위 8비트 레지스터 -> m(*udst)
    "done_put:\n"			  // 성공 -> 그냥 넘어감, 페이지 폴트 -> 핸들러 갔다가 다시 돌아옴(rax = -1, rip = done_put)
    : "=&a" (error_code), "=m" (*udst) : "q" (byte));	//출력 0번 rax -> error_code, 출력 1번 m -> udst, 입력 0 q 
    return error_code != -1;
}