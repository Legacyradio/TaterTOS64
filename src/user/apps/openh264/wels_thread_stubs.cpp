/*
 * wels_thread_stubs.cpp -- Stub implementations for OpenH264 threading API
 *
 * TaterTOS64v3 runs the H.264 decoder single-threaded.
 * All thread/mutex/event operations are no-ops or return success.
 */

#include "WelsThreadLib.h"
#include <string.h>

WELS_THREAD_ERROR_CODE WelsMutexInit(WELS_MUTEX *mutex) {
    if (mutex) memset(mutex, 0, sizeof(*mutex));
    return WELS_THREAD_ERROR_OK;
}

WELS_THREAD_ERROR_CODE WelsMutexLock(WELS_MUTEX *mutex) {
    (void)mutex;
    return WELS_THREAD_ERROR_OK;
}

WELS_THREAD_ERROR_CODE WelsMutexUnlock(WELS_MUTEX *mutex) {
    (void)mutex;
    return WELS_THREAD_ERROR_OK;
}

WELS_THREAD_ERROR_CODE WelsMutexDestroy(WELS_MUTEX *mutex) {
    (void)mutex;
    return WELS_THREAD_ERROR_OK;
}

WELS_THREAD_ERROR_CODE WelsEventOpen(WELS_EVENT *p_event, const char *) {
    if (p_event) memset(p_event, 0, sizeof(*p_event));
    return WELS_THREAD_ERROR_OK;
}

WELS_THREAD_ERROR_CODE WelsEventClose(WELS_EVENT *p_event, const char *) {
    (void)p_event;
    return WELS_THREAD_ERROR_OK;
}

WELS_THREAD_ERROR_CODE WelsEventSignal(WELS_EVENT *p_event, WELS_MUTEX *, int *) {
    (void)p_event;
    return WELS_THREAD_ERROR_OK;
}

WELS_THREAD_ERROR_CODE WelsEventWait(WELS_EVENT *p_event, WELS_MUTEX *, int &) {
    (void)p_event;
    return WELS_THREAD_ERROR_WAIT_TIMEOUT;
}

WELS_THREAD_ERROR_CODE WelsEventWaitWithTimeOut(WELS_EVENT *p_event, unsigned long) {
    (void)p_event;
    return WELS_THREAD_ERROR_WAIT_TIMEOUT;
}

WELS_THREAD_ERROR_CODE WelsMultipleEventsWaitSingleBlocking(unsigned int, WELS_EVENT *,
                                                             WELS_EVENT *, WELS_MUTEX *,
                                                             int *, bool *) {
    return WELS_THREAD_ERROR_WAIT_TIMEOUT;
}

WELS_THREAD_ERROR_CODE WelsMultipleEventsWaitAllBlocking(unsigned int, WELS_EVENT *,
                                                          WELS_MUTEX *, WELS_EVENT *) {
    return WELS_THREAD_ERROR_WAIT_TIMEOUT;
}

WELS_THREAD_ERROR_CODE WelsThreadCreate(WELS_THREAD_HANDLE *thread,
                                         LPWELS_THREAD_ROUTINE routine,
                                         void *arg, WELS_THREAD_ATTR attr) {
    (void)thread; (void)routine; (void)arg; (void)attr;
    return WELS_THREAD_ERROR_GENERAL;  /* Thread creation not supported */
}

WELS_THREAD_ERROR_CODE WelsThreadJoin(WELS_THREAD_HANDLE thread) {
    (void)thread;
    return WELS_THREAD_ERROR_OK;
}

WELS_THREAD_ERROR_CODE WelsThreadCancel(WELS_THREAD_HANDLE thread) {
    (void)thread;
    return WELS_THREAD_ERROR_OK;
}

WELS_THREAD_ERROR_CODE WelsThreadSetName(const char *name) {
    (void)name;
    return WELS_THREAD_ERROR_OK;
}

void WelsSleep(unsigned int ms) {
    (void)ms;
}

WELS_THREAD_HANDLE WelsThreadSelf(void) {
    return (WELS_THREAD_HANDLE)0;
}

/* ---- Decoder-specific thread/event/semaphore stubs ---- */

#include "wels_decoder_thread.h"

int32_t GetCPUCount() { return 1; }

int EventCreate(SWelsDecEvent *e, int, int) {
    if (e) memset(e, 0, sizeof(*e));
    return 0;
}

void EventPost(SWelsDecEvent *) {}
int EventWait(SWelsDecEvent *, int32_t) { return 0; }
void EventReset(SWelsDecEvent *) {}
void EventDestroy(SWelsDecEvent *) {}

int SemCreate(SWelsDecSemphore *s, long, long) {
    if (s) memset(s, 0, sizeof(*s));
    return 0;
}

int SemWait(SWelsDecSemphore *, int32_t) { return 0; }
void SemRelease(SWelsDecSemphore *, long *prev) { if (prev) *prev = 0; }
void SemDestroy(SWelsDecSemphore *) {}

int ThreadCreate(SWelsDecThread *, LPWELS_THREAD_ROUTINE, void *) { return -1; }
int ThreadWait(SWelsDecThread *) { return 0; }

/* ---- localtime/strftime stubs for crt_util_safe_x.cpp ---- */

#include <time.h>

extern "C" {

struct tm *localtime(const long *) {
    static struct tm zero_tm;
    memset(&zero_tm, 0, sizeof(zero_tm));
    return &zero_tm;
}

unsigned long strftime(char *s, unsigned long max, const char *, const struct tm *) {
    if (s && max > 0) s[0] = '\0';
    return 0;
}

}
