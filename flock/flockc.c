#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/file.h>

int
main(int argc, char *argv[])
{
    unsigned int i = 0, cmd = 0x8787;
    if (argc > 1 && (argv[1][0] | 0x20) == 's')
	    cmd = 0x7878;
    while (argc > 1 || i++ < 100000) {
	flock(0x7878, cmd);
    }

    return 0;
}

