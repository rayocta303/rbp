/* compat_glibc.c - Compatibility forwarders for glibc 2.13 runtime on modern cross-compilers */
#define _GNU_SOURCE
#include <time.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

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

/* Forwarders for 64-bit time functions in 32-bit glibc */
int __clock_gettime64(clockid_t clk_id, void *tp64) {
    struct timespec ts32;
    int ret = clock_gettime(clk_id, &ts32);
    if (ret == 0 && tp64) {
        long long *s = (long long*)tp64;
        long *ns = (long*)((char*)tp64 + sizeof(long long));
        *s = ts32.tv_sec;
        *ns = ts32.tv_nsec;
    }
    return ret;
}

int __nanosleep64(const void *req64, void *rem64) {
    struct timespec req32;
    if (req64) {
        long long *s = (long long*)req64;
        long *ns = (long*)((char*)req64 + sizeof(long long));
        req32.tv_sec = (time_t)*s;
        req32.tv_nsec = *ns;
    }
    return nanosleep(req64 ? &req32 : NULL, NULL);
}

long long __time64(long long *t64) {
    time_t t = time(NULL);
    if (t64) *t64 = t;
    return (long long)t;
}

void *__gmtime64_r(const long long *timer, void *result) {
    time_t t = timer ? (time_t)*timer : 0;
    return gmtime_r(&t, (struct tm*)result);
}

int __gettimeofday64(void *tv64, void *tz) {
    struct timeval tv32;
    int ret = gettimeofday(&tv32, (struct timezone*)tz);
    if (ret == 0 && tv64) {
        long long *s = (long long*)tv64;
        long *us = (long*)((char*)tv64 + sizeof(long long));
        *s = tv32.tv_sec;
        *us = tv32.tv_usec;
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
