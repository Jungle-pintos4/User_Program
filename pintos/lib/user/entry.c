#include <syscall.h>

int main (int, char *[]);
void _start (int argc, char *argv[]);

void
_start (int argc, char *argv[]) {
	int res = main (argc, argv);

	write(1, "Main returned, calling exit...\n", 30);
	exit (res);
}
