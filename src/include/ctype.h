/*
 * TaterTOS64v3 — <ctype.h>
 *
 * POSIX/C ctype.h surface. Implementations live in
 * src/user/libc/string_ext.c (or libc.c). Already declared in
 * libc.h; this header re-exposes them under the canonical path.
 *
 * Origin log: logs/fry835.txt
 */

#ifndef _TATERTOS_CTYPE_H
#define _TATERTOS_CTYPE_H

#ifdef __cplusplus
extern "C" {
#endif

int isalpha(int c);
int isdigit(int c);
int isalnum(int c);
int isspace(int c);
int isupper(int c);
int islower(int c);
int isprint(int c);
int isgraph(int c);
int iscntrl(int c);
int ispunct(int c);
int isxdigit(int c);
int isblank(int c);
int isascii(int c);
int toupper(int c);
int tolower(int c);
int toascii(int c);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _TATERTOS_CTYPE_H */
