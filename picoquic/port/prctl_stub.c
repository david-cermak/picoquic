#include <sys/prctl.h>

int prctl(int op, ...)
{
    (void)op;
    return 0;
}