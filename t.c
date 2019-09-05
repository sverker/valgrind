#include <stdio.h>
#include <stdlib.h>
#include <valgrind/memcheck.h>

void foo(int sz)
{
    unsigned char *x = malloc(sz);
    x[0] = sz;
    x[1] = sz+1;
    x[sz-1] = 17;
}

void bar(int sz, int i)
{
    foo(sz+i);
}

static void check(void)
{
    int leaked, dubious, reachable, suppressed;

    VALGRIND_DO_ADDED_LEAK_CHECK;
    VALGRIND_COUNT_LEAK_BLOCKS(leaked, dubious, reachable, suppressed);

    printf("%d %d %d %d\n", leaked, dubious, reachable, suppressed);
}

int main()
{
    int i;
    for (i=2; i; --i) {
        bar(123,i);
	check();
    }
    foo(234);
    check();
    bar(17,0);
    check();
    return 0;
}
