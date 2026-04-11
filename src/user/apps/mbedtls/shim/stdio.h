/*
 * stdio.h shim — maps to TaterTOS64v3 libc
 */
#ifndef _TATER_SHIM_STDIO_H
#define _TATER_SHIM_STDIO_H

#include <stddef.h>
#include <stdarg.h>

int printf(const char *fmt, ...);
int snprintf(char *buf, size_t n, const char *fmt, ...);
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);

/* FILE type — defer to real libc if available, else provide stub */
#ifndef _TATER_LIBC_FILE_DEFINED
typedef struct _FILE_shim { int fd; } FILE;
extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;
int fprintf(FILE *stream, const char *fmt, ...);
int vfprintf(FILE *stream, const char *fmt, va_list ap);
#endif

/* sscanf — from string_ext or stdio */
int sscanf(const char *str, const char *fmt, ...);
int sprintf(char *str, const char *fmt, ...);
int putchar(int c);
int puts(const char *s);

/* FILE operations — from TaterTOS stdio.c */
FILE *fopen(const char *path, const char *mode);
int fclose(FILE *stream);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);
int fseek(FILE *stream, long offset, int whence);
long ftell(FILE *stream);
int feof(FILE *stream);
int ferror(FILE *stream);
int fflush(FILE *stream);
int fgetc(FILE *stream);
int fputc(int c, FILE *stream);
char *fgets(char *s, int n, FILE *stream);
int fputs(const char *s, FILE *stream);
int fileno(FILE *stream);

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define EOF (-1)

/* NULL */
#ifndef NULL
#define NULL ((void *)0)
#endif

#endif
