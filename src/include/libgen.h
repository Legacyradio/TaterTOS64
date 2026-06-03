/* TaterTOS64 libgen.h — POSIX basename/dirname */
#ifndef _LIBGEN_H
#define _LIBGEN_H

#ifdef __cplusplus
extern "C" {
#endif

char *basename(char *path);
char *dirname(char *path);

#ifdef __cplusplus
}
#endif

#endif /* _LIBGEN_H */
