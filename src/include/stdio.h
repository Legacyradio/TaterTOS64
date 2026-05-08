/*
 * TaterTOS64v3 — <stdio.h>
 *
 * POSIX/C stdio.h surface for TaterTOS userland. The actual symbols
 * (FILE, stdin/out/err, printf, fopen, etc.) are already declared in
 * src/user/libc/libc.h with their canonical POSIX names — this
 * header simply re-exposes that surface so apps that #include
 * <stdio.h> (without <libc.h>) get the same set.
 *
 * Engineered code, not a stub.
 *
 * Origin log: logs/fry832.txt
 * Triggered by: AK/Format.h:15 (Ladybird port).
 */

#ifndef _TATERTOS_STDIO_H
#define _TATERTOS_STDIO_H

#include <stddef.h>      /* size_t, NULL */
#include <stdint.h>
#include <stdarg.h>      /* va_list for v*printf */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * FILE is opaque — the layout lives in stdio.c. Apps only see the
 * forward declaration and pointers.
 */
typedef struct _FILE FILE;
typedef long fpos_t;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

#ifndef EOF
#  define EOF (-1)
#endif

#ifndef BUFSIZ
#  define BUFSIZ 4096
#endif

#ifndef SEEK_SET
#  define SEEK_SET 0
#  define SEEK_CUR 1
#  define SEEK_END 2
#endif

/*
 * setvbuf modes.
 */
#define _IOFBF  0       /* fully buffered */
#define _IOLBF  1       /* line buffered  */
#define _IONBF  2       /* unbuffered     */

/*
 * Formatted I/O — already implemented in src/user/libc/{libc,stdio}.c.
 */
int printf(const char *fmt, ...);
int snprintf(char *buf, size_t n, const char *fmt, ...);
int sprintf(char *buf, const char *fmt, ...);
int vsprintf(char *buf, const char *fmt, va_list ap);
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int vprintf(const char *fmt, va_list ap);
int fprintf(FILE *stream, const char *fmt, ...);
int vfprintf(FILE *stream, const char *fmt, va_list ap);

/*
 * Single-character I/O.
 */
int  putchar(int c);
int  getchar(void);
int  puts(const char *s);
int  fputc(int c, FILE *stream);
int  fgetc(FILE *stream);
int  getc(FILE *stream);
int  putc(int c, FILE *stream);
int  ungetc(int c, FILE *stream);
char *fgets(char *s, int n, FILE *stream);
int  fputs(const char *s, FILE *stream);

#define getc(s)        fgetc(s)
int getc_unlocked(FILE *stream);
#define putc(c, s)     fputc((c), (s))
int putc_unlocked(int c, FILE *stream);

/*
 * File operations.
 */
FILE  *fopen(const char *path, const char *mode);
FILE  *fdopen(int fd, const char *mode);
int    fclose(FILE *stream);
int    fflush(FILE *stream);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);
int    fseek(FILE *stream, long offset, int whence);
long   ftell(FILE *stream);
void   rewind(FILE *stream);
int    feof(FILE *stream);
int    ferror(FILE *stream);
void   clearerr(FILE *stream);
int    fileno(FILE *stream);
int    setvbuf(FILE *stream, char *buf, int mode, size_t size);
void   setbuf(FILE *stream, char *buf);
int fscanf(FILE *stream, const char *fmt, ...);
int vfscanf(FILE *stream, const char *fmt, va_list ap);
int sscanf(const char *str, const char *fmt, ...);
int vsscanf(const char *str, const char *fmt, va_list ap);
int scanf(const char *fmt, ...);
int vscanf(const char *fmt, va_list ap);
void perror(const char *s);

/*
 * File positioning extras.
 */
int fgetpos(FILE *stream, fpos_t *pos);
int fsetpos(FILE *stream, const fpos_t *pos);

/*
 * Temp files.
 */
FILE *tmpfile(void);
char *tmpnam(char *s);
FILE *freopen(const char *path, const char *mode, FILE *stream);

/*
 * Filename ops — backed by libc.h fry_* primitives. Declared here
 * so portable upstream code that uses the standard names compiles.
 */
int remove(const char *pathname);
int rename(const char *old_path, const char *new_path);
int renameat(int olddirfd, const char *old_path, int newdirfd, const char *new_path);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _TATERTOS_STDIO_H */
