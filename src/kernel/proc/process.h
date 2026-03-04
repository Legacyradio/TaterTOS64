#ifndef TATER_PROCESS_H
#define TATER_PROCESS_H

#include <stdint.h>

enum proc_state {
    PROC_UNUSED = 0,
    PROC_RUNNING,
    PROC_WAITING,
    PROC_ZOMBIE,
    PROC_DEAD
};

#define PROC_MAX 256
#define PROC_OUTBUF 512   /* per-process stdout ring buffer size */
#define PROC_INBUF 512    /* per-process stdin ring buffer size */

enum process_launch_error {
    PROCESS_LAUNCH_OK = 0,
    PROCESS_LAUNCH_ERR_CREATE_USER = -200
};

struct fry_process {
    uint32_t pid;
    uint64_t cr3;
    uint64_t saved_rsp;
    uint64_t saved_rip;
    uint64_t kernel_stack_top;
    uint8_t state;
    uint8_t cpu;
    struct fry_process *next;
    char name[32];
    int fd_table[64];
    void *fd_ptrs[64];
    uint32_t exit_code;
    uint64_t user_rsp;
    uint64_t user_rip;
    uint64_t wake_time_ms;
    uint32_t wait_pid;
    uint8_t is_kernel;
    uint64_t heap_start;
    uint64_t heap_end;
    uint64_t kernel_stack_phys;
    uint32_t kernel_stack_pages;
    void (*kentry)(void *arg);
    void *karg;
    /* stdout capture ring buffer — written by SYS_WRITE(fd=1),
       read by SYS_PROC_OUTPUT.  head==tail means empty. */
    uint8_t  outbuf[PROC_OUTBUF];
    uint32_t outbuf_head;   /* consumer read index */
    uint32_t outbuf_tail;   /* producer write index */
    /* stdin input ring buffer — written by SYS_PROC_INPUT,
       read by SYS_READ(fd=0).  head==tail means empty. */
    uint8_t  inbuf[PROC_INBUF];
    uint32_t inbuf_head;    /* consumer read index */
    uint32_t inbuf_tail;    /* producer write index */
};

extern struct fry_process procs[PROC_MAX];

int process_init(void);
struct fry_process *process_create_user(uint64_t cr3, uint64_t entry, uint64_t user_rsp, const char *name);
struct fry_process *process_create_kernel(void (*entry)(void *), void *arg, const char *name);
struct fry_process *proc_current(void);
void proc_set_current(struct fry_process *p);
void proc_free(uint32_t pid);
int process_launch(const char *path);
int process_last_launch_error(void);
int process_wait(uint32_t pid);
uint32_t process_count(void);

#endif
