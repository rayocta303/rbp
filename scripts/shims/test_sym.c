#include <time.h>
#include <stdio.h>
#include <stdlib.h>

void test(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    time(NULL);
    nanosleep(&ts, NULL);
    int val = 0;
    sscanf("123", "%d", &val);
    strtol("456", NULL, 10);
    strtoul("789", NULL, 10);
}
