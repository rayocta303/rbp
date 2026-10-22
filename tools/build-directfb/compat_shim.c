/*
 * compat_shim.c -- fstat(), __fdelt_chk(), and __isoc23_* forwards
 * for glibc 2.13 target built with modern headers (glibc 2.36+ / 2.43).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdarg.h>

int
fstat (int fd, struct stat *buf)
{
     return syscall (SYS_fstat, fd, buf);
}

long int
__fdelt_chk (long int d)
{
     return d / 32;
}

extern int _real_vsscanf (const char *s, const char *format, __builtin_va_list ap) __asm__ ("vsscanf");
extern long int _real_strtol (const char *nptr, char **endptr, int base) __asm__ ("strtol");
extern unsigned long int _real_strtoul (const char *nptr, char **endptr, int base) __asm__ ("strtoul");

int
__isoc23_sscanf (const char *s, const char *format, ...)
{
     __builtin_va_list ap;
     int ret;
     __builtin_va_start (ap, format);
     ret = _real_vsscanf (s, format, ap);
     __builtin_va_end (ap);
     return ret;
}

long int
__isoc23_strtol (const char *nptr, char **endptr, int base)
{
     return _real_strtol (nptr, endptr, base);
}

unsigned long int
__isoc23_strtoul (const char *nptr, char **endptr, int base)
{
     return _real_strtoul (nptr, endptr, base);
}
