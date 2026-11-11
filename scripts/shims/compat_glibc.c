/* compat_glibc.c - Compatibility forwarders for glibc 2.13 runtime on modern cross-compilers */
#define _GNU_SOURCE
#include <unistd.h>
#include <sys/syscall.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#ifndef __NR_nanosleep
#define __NR_nanosleep 162
#endif
#ifndef __NR_clock_gettime
#define __NR_clock_gettime 263
#endif
#ifndef __NR_gettimeofday
#define __NR_gettimeofday 78
#endif
#ifndef __NR_clock_gettime64
#define __NR_clock_gettime64 403
#endif

/* Undefine macros that might map to __isoc23_* */
#undef strtol
#undef strtoul
#undef strtoll
#undef strtoull
#undef strtod
#undef strtof
#undef strtold
#undef sscanf
#undef vsscanf
#undef fscanf
#undef vfscanf
#undef scanf
#undef vscanf

extern int __isoc99_vsscanf(const char *s, const char *format, va_list ap);
extern int __isoc99_sscanf(const char *s, const char *format, ...);
extern int __isoc99_vfscanf(FILE *stream, const char *format, va_list ap);
extern int __isoc99_fscanf(FILE *stream, const char *format, ...);
extern int __isoc99_vscanf(const char *format, va_list ap);
extern int __isoc99_scanf(const char *format, ...);

extern long int __strtol_internal(const char *nptr, char **endptr, int base, int group);
extern unsigned long int __strtoul_internal(const char *nptr, char **endptr, int base, int group);
extern long long int __strtoll_internal(const char *nptr, char **endptr, int base, int group);
extern unsigned long long int __strtoull_internal(const char *nptr, char **endptr, int base, int group);
extern double __strtod_internal(const char *nptr, char **endptr, int group);
extern float __strtof_internal(const char *nptr, char **endptr, int group);
extern long double __strtold_internal(const char *nptr, char **endptr, int group);

/* Real-Time Scheduling stubs: prevents EPERM inside container/chroot when creating RT tasks */
int pthread_attr_setschedpolicy(void *attr, int policy) {
    (void)attr; (void)policy;
    return 0;
}

int pthread_attr_setschedparam(void *attr, const void *param) {
    (void)attr; (void)param;
    return 0;
}

int pthread_setschedparam(unsigned long thread, int policy, const void *param) {
    (void)thread; (void)policy; (void)param;
    return 0;
}

int pthread_setschedprio(unsigned long thread, int prio) {
    (void)thread; (void)prio;
    return 0;
}

int sched_setscheduler(int pid, int policy, const void *param) {
    (void)pid; (void)policy; (void)param;
    return 0;
}

/* Forwarders for 64-bit time functions using direct kernel syscalls to avoid GLIBC_2.17 dependency */
int __clock_gettime64(int clk_id, void *tp64) {
    if (!tp64) return -1;
    long ret = syscall(__NR_clock_gettime64, clk_id, tp64);
    if (ret == 0) return 0;
    struct { long sec; long nsec; } ts32;
    ret = syscall(__NR_clock_gettime, clk_id, &ts32);
    if (ret == 0) {
        long long *s = (long long*)tp64;
        long *ns = (long*)((char*)tp64 + sizeof(long long));
        *s = ts32.sec;
        *ns = ts32.nsec;
    }
    return ret;
}

int __nanosleep64(const void *req64, void *rem64) {
    if (!req64) return 0;
    struct { long sec; long nsec; } req32;
    long long *s = (long long*)req64;
    long *ns = (long*)((char*)req64 + sizeof(long long));
    req32.sec = (long)*s;
    req32.nsec = *ns;
    return syscall(__NR_nanosleep, &req32, NULL);
}

long long __time64(long long *t64) {
    struct { long sec; long usec; } tv32;
    syscall(__NR_gettimeofday, &tv32, NULL);
    if (t64) *t64 = tv32.sec;
    return (long long)tv32.sec;
}

void *__gmtime64_r(const long long *timer, void *result) {
    time_t t = timer ? (time_t)*timer : 0;
    return gmtime_r(&t, (struct tm*)result);
}

int __gettimeofday64(void *tv64, void *tz) {
    if (!tv64) return -1;
    struct { long sec; long usec; } tv32;
    int ret = syscall(__NR_gettimeofday, &tv32, tz);
    if (ret == 0) {
        long long *s = (long long*)tv64;
        long *us = (long*)((char*)tv64 + sizeof(long long));
        *s = tv32.sec;
        *us = tv32.usec;
    }
    return ret;
}

int __isoc23_vsscanf(const char *s, const char *format, va_list ap) {
    return __isoc99_vsscanf(s, format, ap);
}

int __isoc23_sscanf(const char *s, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int ret = __isoc99_vsscanf(s, format, ap);
    va_end(ap);
    return ret;
}

int __isoc23_vfscanf(FILE *stream, const char *format, va_list ap) {
    return __isoc99_vfscanf(stream, format, ap);
}

int __isoc23_fscanf(FILE *stream, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int ret = __isoc99_vfscanf(stream, format, ap);
    va_end(ap);
    return ret;
}

int __isoc23_vscanf(const char *format, va_list ap) {
    return __isoc99_vscanf(format, ap);
}

int __isoc23_scanf(const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int ret = __isoc99_vscanf(format, ap);
    va_end(ap);
    return ret;
}

long int __isoc23_strtol(const char *nptr, char **endptr, int base) {
    return __strtol_internal(nptr, endptr, base, 0);
}

unsigned long int __isoc23_strtoul(const char *nptr, char **endptr, int base) {
    return __strtoul_internal(nptr, endptr, base, 0);
}

long long int __isoc23_strtoll(const char *nptr, char **endptr, int base) {
    return __strtoll_internal(nptr, endptr, base, 0);
}

unsigned long long int __isoc23_strtoull(const char *nptr, char **endptr, int base) {
    return __strtoull_internal(nptr, endptr, base, 0);
}

double __isoc23_strtod(const char *nptr, char **endptr) {
    return __strtod_internal(nptr, endptr, 0);
}

float __isoc23_strtof(const char *nptr, char **endptr) {
    return __strtof_internal(nptr, endptr, 0);
}

long double __isoc23_strtold(const char *nptr, char **endptr) {
    return __strtold_internal(nptr, endptr, 0);
}
