#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <malloc.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/mman.h>

char *buffer;

int main(int argc, char *argv[])
{
    char *p;
    int pagesize, i = 0;

    pagesize = sysconf(_SC_PAGE_SIZE);
    if (pagesize == -1)
        return -1;

    /* allocate a buffer aligned on a page boundary;
       initial protection is PROT_READ | PROT_WRITE */
    buffer = memalign(pagesize, 4 * pagesize);
    if (buffer == NULL)
        return -ENOMEM;

    while (argc > 1 || i++ < 100000) {
        mprotect(buffer + pagesize * 2, pagesize,
                 PROT_READ | PROT_EXEC);
    }

    return 0;
}

