#ifndef TATER_LIBC_H
#define TATER_LIBC_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <fry_types.h>
#include <fry_limits.h>
#include <fry_fcntl.h>
#include <fry_socket.h>
#include <fry_time.h>
#include <fry_random.h>
#include <fry_seek.h>
#include <fry_input.h>
#include "../../shared/wifi_abi.h"

size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, size_t n);
char *strcat(char *dst, const char *src);
char *strncat(char *dst, const char *src, size_t n);
void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);
char *itoa(int value, char *buf, int base);
char *utoa(unsigned int value, char *buf, int base);
int atoi(const char *s);
void *malloc(size_t size);
void free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void *aligned_alloc(size_t alignment, size_t size);
int posix_memalign(void **memptr, size_t alignment, size_t size);
void *memalign(size_t alignment, size_t size);
void *valloc(size_t size);
void *pvalloc(size_t size);
size_t malloc_usable_size(void *ptr);

/* Sorting and searching (A1 — TaterSurf libc gap fill) */
void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *));

// Syscall wrappers
long fry_write(int fd, const void *buf, size_t len);
long fry_read(int fd, void *buf, size_t len);
long fry_spawn(const char *path);
long fry_exit(int code);
long fry_gettid(void);
long fry_sleep(uint64_t ms);
long fry_open(const char *path, int flags);
long fry_close(int fd);

/* Phase 3: IPC / descriptor model */
long fry_pipe(int fds[2]);
long fry_dup(int oldfd);
long fry_dup2(int oldfd, int newfd);

struct fry_pollfd {
    int32_t  fd;
    uint16_t events;
    uint16_t revents;
};

#define FRY_POLLIN   0x0001u
#define FRY_POLLOUT  0x0002u
#define FRY_POLLERR  0x0008u
#define FRY_POLLHUP  0x0010u
#define FRY_POLLNVAL 0x0020u

long fry_poll(struct fry_pollfd *fds, uint32_t nfds, uint64_t timeout_ms);
long fry_fcntl(int fd, int cmd, long arg);
long fry_spawn_args(const char *path, const char **argv, uint32_t argc,
                    const char **envp, uint32_t envc);
long fry_get_argc(void);
long fry_get_argv(uint32_t index, char *buf, size_t len);
long fry_getenv(const char *name, char *buf, size_t len);

/* Phase 4: Socket ABI */
long fry_socket(int domain, int type, int protocol);
long fry_connect(int fd, const struct fry_sockaddr_in *addr, uint32_t addrlen);
long fry_bind(int fd, const struct fry_sockaddr_in *addr, uint32_t addrlen);
long fry_listen(int fd, int backlog);
long fry_accept(int fd, struct fry_sockaddr_in *addr, uint32_t *addrlen);
long fry_send(int fd, const void *buf, size_t len, int flags);
long fry_recv(int fd, void *buf, size_t len, int flags);
long fry_shutdown_sock(int fd, int how);
long fry_getsockopt(int fd, int level, int optname, void *optval, uint32_t *optlen);
long fry_setsockopt(int fd, int level, int optname, const void *optval, uint32_t optlen);
long fry_sendto(int fd, const void *buf, size_t len, int flags,
                const struct fry_sockaddr_in *dest_addr);
long fry_recvfrom(int fd, void *buf, size_t len, int flags,
                  struct fry_sockaddr_in *src_addr);
long fry_dns_resolve(const char *hostname, uint32_t *ip_out);

/* Phase 5: Randomness, Time, Core Runtime */
long fry_getrandom(void *buf, unsigned long len, unsigned int flags);
long fry_clock_gettime(int clock_id, struct fry_timespec *ts);
long fry_nanosleep(const struct fry_timespec *req, struct fry_timespec *rem);

/* Phase 6: Filesystem and Runtime Expansion */
long fry_lseek(int fd, int64_t offset, int whence);
long fry_ftruncate(int fd, uint64_t length);
long fry_rename(const char *old_path, const char *new_path);
/* fry_fstat declared after struct fry_stat below */

/* Phase 7: GUI/Input expansion */
long fry_kbd_event(struct fry_key_event *out);
/* fry_mouse_get_ext declared after struct fry_mouse_state below */
long fry_clipboard_get(char *buf, size_t maxlen);
long fry_clipboard_set(const char *buf, size_t len);

/* Audio (TaterSurf Phase D) */
long fry_audio_open(uint32_t sample_rate, uint8_t channels, uint8_t bits);
long fry_audio_write(const void *pcm_data, size_t len);
long fry_audio_close(void);
long fry_audio_info(void *info_buf);

long fry_getpid(void);
long fry_gettime(void);
long fry_sbrk(intptr_t increment);
void *fry_mmap(void *addr, size_t len, uint32_t prot, uint32_t flags);
void *fry_mmap_fd(void *addr, size_t len, uint32_t prot, uint32_t flags, int fd);
void *fry_mreserve(void *addr, size_t len, uint32_t flags);
void *fry_mguard(void *addr, size_t len);
long fry_mcommit(void *addr, size_t len, uint32_t prot);
long fry_munmap(void *addr, size_t len);
long fry_mprotect(void *addr, size_t len, uint32_t prot);
long fry_syscall_raw(long num, long a1);
long fry_shm_alloc(size_t size);
long fry_shm_map(int shm_id);
long fry_shm_free(int shm_id);
struct fry_stat;
long fry_stat(const char *path, struct fry_stat *st);
long fry_readdir(const char *path, void *buf, size_t len);
long fry_readdir_ex(const char *path, void *buf, size_t len);
long fry_reboot(void);
long fry_shutdown(void);
long fry_wait(uint32_t pid);
long fry_proc_count(void);

typedef void (*fry_thread_start_t)(void *arg);

struct fry_thread {
    uint32_t tid;
    void *stack_base;
    size_t stack_len;
    void *tls_base;
};

long fry_thread_create(struct fry_thread *thr, fry_thread_start_t start, void *arg);
long fry_thread_join(struct fry_thread *thr, int *exit_code);
int fry_thread_current(struct fry_thread *thr);
__attribute__((noreturn)) void fry_thread_exit(int code);

long fry_futex_wait(volatile uint32_t *addr, uint32_t expected, uint64_t timeout_ms);
long fry_futex_wake(volatile uint32_t *addr, uint32_t count);
long fry_tls_set_base(void *base);
void *fry_tls_get_base(void);

typedef uint32_t fry_tls_key_t;

int fry_tls_key_create(fry_tls_key_t *out_key);
void *fry_tls_get(fry_tls_key_t key);
int fry_tls_set(fry_tls_key_t key, void *value);

typedef struct { volatile uint32_t state; } fry_mutex_t;
typedef struct { volatile uint32_t seq; } fry_cond_t;
typedef struct { volatile uint32_t count; } fry_sem_t;
typedef struct { volatile uint32_t state; } fry_once_t;

#define FRY_MUTEX_INIT {0u}
#define FRY_COND_INIT  {0u}
#define FRY_ONCE_INIT  {0u}

int fry_mutex_lock(fry_mutex_t *mutex);
int fry_mutex_trylock(fry_mutex_t *mutex);
int fry_mutex_unlock(fry_mutex_t *mutex);
int fry_cond_wait(fry_cond_t *cond, fry_mutex_t *mutex);
int fry_cond_signal(fry_cond_t *cond);
int fry_cond_broadcast(fry_cond_t *cond);
int fry_sem_init(fry_sem_t *sem, uint32_t value);
int fry_sem_wait(fry_sem_t *sem);
int fry_sem_post(fry_sem_t *sem);
int fry_once(fry_once_t *once, void (*init_fn)(void));

struct fry_stat {
    uint64_t size;
    uint32_t attr;
};

long fry_fstat(int fd, struct fry_stat *st);

struct fry_dirent {
    uint16_t rec_len;
    uint16_t name_len;
    uint32_t attr;
    uint64_t size;
    char name[];
};

struct fry_battery_status {
    uint32_t state;
    uint32_t present_rate;
    uint32_t remaining_capacity;
    uint32_t present_voltage;
};

// printf subset
int printf(const char *fmt, ...);
int snprintf(char *buf, size_t n, const char *fmt, ...);
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int putchar(int c);
int puts(const char *s);
int getchar(void);
char *gets_bounded(char *buf, int max);

long fry_setbrightness(uint32_t percent);
long fry_getbrightness(void);
long fry_getbattery(struct fry_battery_status *out);
struct fry_fb_info {
    uint64_t phys;
    uint64_t size;
    uint64_t user_base;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
};
long fry_fb_info(struct fry_fb_info *info);
long fry_fb_map(void);

struct fry_mouse_state {
    int32_t x;       /* absolute position 0-4095 */
    int32_t y;       /* absolute position 0-4095 */
    int32_t dx;      /* delta since last fry_mouse_get() call */
    int32_t dy;      /* delta since last fry_mouse_get() call */
    uint8_t btns;
    uint8_t _pad[3];
    int32_t wheel;   /* scroll wheel delta since last call (Phase 7) */
};
long fry_mouse_get(struct fry_mouse_state *ms);
long fry_mouse_get_ext(struct fry_mouse_state *ms);
/* Read from a process's stdout ring buffer.
   Returns: >0 bytes read, 0 alive/no data, -2 process dead+empty, -1 error */
long fry_proc_output(uint32_t pid, void *buf, size_t len);
/* Write to a process's stdin ring buffer. */
long fry_proc_input(uint32_t pid, const void *buf, size_t len);
long fry_kill(long pid);
long fry_create(const char *path, uint16_t type);
long fry_mkdir(const char *path);
long fry_unlink(const char *path);
long fry_wifi_status(struct fry_wifi_status *out);
long fry_wifi_scan(struct fry_wifi_scan_entry *out, uint32_t max_entries, uint32_t *out_count);
long fry_wifi_connect(const char *ssid, const char *passphrase);
long fry_wifi_debug(char *buf, uint32_t bufsz);
long fry_wifi_cpu_status(char *buf, uint32_t bufsz);
long fry_wifi_init_log(char *buf, uint32_t bufsz);
long fry_wifi_debug2(char *buf, uint32_t bufsz);
long fry_wifi_handoff(char *buf, uint32_t bufsz);
long fry_wifi_debug3(char *buf, uint32_t bufsz);
long fry_wifi_reinit(void);
long fry_wifi_cmd_trace(char *buf, uint32_t bufsz);
long fry_wifi_sram(char *buf, uint32_t bufsz);
long fry_wifi_deep_diag(char *buf, uint32_t bufsz);
long fry_wifi_verify(char *buf, uint32_t bufsz);
long fry_eth_diag(char *buf, uint32_t bufsz);
#define FRY_PROT_READ   0x01u
#define FRY_PROT_WRITE  0x02u
#define FRY_PROT_EXEC   0x04u

#define FRY_MAP_SHARED  0x01u
#define FRY_MAP_PRIVATE 0x02u
#define FRY_MAP_FIXED   0x10u
#define FRY_MAP_ANON    0x20u
#define FRY_MAP_FILE    0x40u
#define FRY_MAP_RESERVE 0x80u
#define FRY_MAP_GUARD   0x100u

/* mmap returns negative errno on failure; any pointer with high bit set is an error */
#define FRY_MAP_FAILED ((void *)(intptr_t)-1)
#define FRY_IS_ERR(p) ((intptr_t)(p) < 0)
#define FRY_PTR_ERR(p) (-(int)(intptr_t)(p))

struct fry_acpi_diag {
    uint32_t ns_nodes;
    uint8_t  ec_ok;
    uint8_t  _pad0[1];
    uint16_t ec_data_port;
    uint16_t ec_cmd_port;
    uint16_t _pad1;
    uint8_t  batt_count;
    uint8_t  _pad2[3];
    uint32_t batt_sta;
    char     batt_path[64];
    uint8_t  bl_found;
    uint8_t  _pad3[3];
    uint32_t bl_sta;
    uint32_t bl_bcl_count;
    uint32_t bl_supported_count;
    uint32_t bl_raw;
    uint32_t bl_percent;
    char     bl_path[64];
    /* EC Diagnostics */
    uint8_t  ec_probe_step;      /* 0=not_probed 1=sts_ff 2=ibf_pre 3=ibf_post 4=obf 5=ok */
    uint8_t  ec_probe_status;    /* raw status byte at probe time */
    uint8_t  ec_probe_attempts;  /* number of attempts made */
    uint8_t  ec_node_found;      /* PNP0C09 found */
    uint8_t  ec_reg_called;      /* _REG(3,1) invoked */
    uint8_t  ec_ini_found;       /* _INI found and called */
    uint8_t  ec_gpe_found;       /* _GPE found */
    uint8_t  ec_gpe_num;         /* GPE number (0xFF = none) */
    uint16_t lpc_ioe_before;     /* 00:1f.0 IOE register (offset 0x82) before EC decode enable */
    uint16_t lpc_ioe_after;      /* 00:1f.0 IOE register (offset 0x82) after EC decode enable */
    uint8_t  ec_ports_source;    /* 0=default 1=ECDT 2=_CRS */
    uint8_t  lpc_bus;
    uint8_t  lpc_slot;
    uint8_t  lpc_func;
    uint16_t lpc_vendor;
    uint16_t lpc_device;
    uint8_t  lpc_class_code;
    uint8_t  lpc_subclass;
    uint8_t  lpc_prog_if;
    uint8_t  lpc_write_attempted; /* safety gates passed for IOE programming */
    uint8_t  lpc_write_performed; /* a write to config 0x80 was issued */
    uint8_t  _pad4[3];
    uint16_t lpc_cmd;
    uint16_t _pad5;
    uint32_t lpc_reg80_before;
    uint32_t lpc_reg80_after;
    uint32_t lpc_reg84;
    uint32_t lpc_reg88;
    /* PCR[DMI] sideband mirror diagnostics (fry398, fixed fry399) */
    uint32_t pcr_ioe_before;    /* PCR[DMI]+0x2774 before mirror write */
    uint32_t pcr_ioe_after;     /* PCR[DMI]+0x2774 after mirror write */
    uint8_t  pcr_mirror_done;   /* 1 if PCR mirror write was performed */
    uint8_t  p2sb_hidden;       /* P2SB was hidden, had to unhide */
    uint8_t  ec_early_sts;      /* port 0x66 status BEFORE any init */
    uint8_t  ec_post_lpc_sts;   /* port 0x66 status AFTER LPC decode config */
    uint8_t  pcr_pid;           /* sideband PID used (0xEF=DMI correct) */
    uint8_t  ec_pre_reg_sts;    /* port 0x66 status right before _REG */
    uint8_t  ec_pre_reg_probe_ok;   /* 1 if pre-_REG probe succeeded */
    uint8_t  ec_recovery_method;    /* 0=none 1=normal 2=state_reset 3=burst
                                       4=sci_drain 5=force_enable 6=burst+force */
    uint8_t  _pad6[1];
    uint16_t ec_reg_suppressed;     /* EC I/O calls suppressed during _REG */
    /* fry421: immediate raw probe diagnostics */
    uint8_t  ec_imm_step;      /* 0=not_tried 1=STS_FF 2=IBF_PRE 3=IBF_POST 4=OBF_TMO 5=OK */
    uint8_t  ec_imm_val;       /* reg[0] value if imm_step==5 */
    uint8_t  ec_imm_post_sts;  /* port 0x66 after immediate probe attempt */
    uint8_t  ec_post_setup_sts; /* port 0x66 after ec_setup_ports */
    /* fry422: floating bus detection + force SMI */
    uint8_t  ec_ibf_seen;      /* 1 if IBF was observed after cmd write (real EC, not floating) */
    uint8_t  ec_pre_data;      /* inb(0x62) before any command (pending data) */
    uint8_t  ec_smi_sent;      /* 1 if force ACPI_ENABLE was sent before re-probe */
    uint8_t  ec_imm2_step;     /* result of Phase 1 re-probe (after SMI) same encoding as imm_step */
    /* fry423: Gen3/Gen4 decode + eSPI diagnostics */
    uint32_t lpc_reg8c;        /* Generic I/O Decode Range 3 (offset 0x8C) */
    uint32_t lpc_reg90;        /* Generic I/O Decode Range 4 (offset 0x90) */
    uint32_t espi_raw[8];      /* [0]=CFG_VAL [1]=PCBC [2]=PCERR [3]=VWERR
                                   [4]=FCERR [5]=LNKERR [6]=SLV_CTL [7]=GenCfg */
    uint8_t  espi_probed;      /* 1 if eSPI PCR was probed */
    uint8_t  espi_pid;         /* PID that worked for eSPI PCR access */
    uint8_t  espi_en;          /* 1 if ESPI_EN strap set (D31:F0+0xDC bit 2) */
    uint8_t  _pad7[1];
    /* fry435: eSPI W1C error clear diagnostics */
    uint32_t espi_pre_clear[4];   /* PCERR/VWERR/FCERR/LNKERR before first clear */
    uint32_t espi_post_clear[4];  /* after most recent clear */
    uint8_t  espi_clear_run;      /* bit 0=proactive, bit 1=recovery */
    uint8_t  espi_clear_found;    /* 1 if errors were non-zero */
    uint8_t  espi_clear_ok;       /* 1 if all regs zero after clear */
    uint8_t  _pad8[1];
    /* G445: EC policy exposure */
    uint32_t ec_policy_timeout;     /* ibf_obf_timeout from policy */
    uint8_t  ec_policy_retries;     /* probe_retries from policy */
    uint8_t  ec_policy_max_fail;    /* max_consec_fail from policy */
    uint8_t  ec_policy_flags;       /* bit0=alternate bit1=swapped bit2=suppress_reg */
    uint8_t  _pad9;
    /* G451/G452: EC query/event diagnostics */
    uint32_t ec_queries_dispatched;
    uint32_t ec_queries_dropped;
    uint32_t ec_storm_count;
    /* G446: candidate source info */
    uint8_t  ec_cand_count;         /* number of EC candidates found */
    uint8_t  ec_best_cand_source;   /* 0=fallback 1=ECDT 2=CRS */
    uint8_t  ec_events_frozen;      /* 1 if events frozen */
    uint8_t  _pad10;
    /* fry444: eSPI slave channel diagnostics */
    uint32_t espi_slave_pc_cap;     /* Peripheral Channel Cap (slave reg 0x0010) */
    uint32_t espi_slave_vw_cap;     /* Virtual Wire Channel Cap (slave reg 0x0020) */
    uint8_t  espi_slave_pc_en;      /* PC Ready bit from slave */
    uint8_t  espi_slave_vw_en;      /* VW Ready bit from slave */
    uint8_t  espi_gen_chan_sup;      /* GenCfg[15:12] channel support: b0=PC b1=VW b2=OOB b3=FC */
    uint8_t  espi_slave_read_ok;    /* bitmask: b0=genCfg b1=pcCap b2=vwCap read succeeded */
    /* fry446: eSPI channel initialization diagnostics */
    uint8_t  espi_chinit_result;    /* 0=not_run 1=already_ready 2=toggled_ok 3=partial 4=failed */
    uint8_t  espi_pltrst_state;     /* 0xFF=not_found; else: b0=data b1=valid (for PLT_RST# wire) */
    uint8_t  espi_pltrst_sent;      /* 1 if PLT_RST de-assertion was attempted */
    uint8_t  espi_chinit_retries;   /* number of toggle retries actually attempted */
    uint32_t espi_chinit_pc_cap;    /* PC_CAP register value after channel init */
    uint32_t espi_chinit_vw_cap;    /* VW_CAP register value after channel init */
};
long fry_acpi_diag(struct fry_acpi_diag *out);

struct fry_storage_info {
    uint8_t  nvme_detected;       /* 1 if NVMe controller found */
    uint8_t  root_fs_type;        /* 0=none, 1=FAT32, 2=ToTFS, 3=NTFS, 4=ramdisk */
    uint8_t  secondary_fs_type;   /* 0=none, 1=FAT32, 2=ToTFS, 3=NTFS, 4=ramdisk */
    uint8_t  flags;               /* bit0=root source is ramdisk/live media */
    uint32_t sector_size;         /* NVMe sector size (512/4096) */
    uint64_t total_sectors;       /* NVMe total sectors */
    char     root_mount[16];      /* "/" */
    char     secondary_mount[16]; /* "/nvme" or empty */
};
#define FRY_STORAGE_FLAG_ROOT_RAMDISK_SOURCE 0x01u
long fry_storage_info(struct fry_storage_info *out);

struct fry_path_fs_info {
    uint8_t  fs_type;             /* 0=none, 1=FAT32, 2=ToTFS, 3=NTFS, 4=ramdisk */
    uint8_t  pad[3];
    char     mount[16];
};
long fry_path_fs_info(const char *path, struct fry_path_fs_info *out);

#define FRY_MAX_MOUNT_INFO 16u

struct fry_mount_info {
    uint8_t  fs_type;             /* 0=none, 1=FAT32, 2=ToTFS, 3=NTFS, 4=ramdisk */
    uint8_t  pad[3];
    char     mount[64];
};

struct fry_mounts_info {
    uint32_t count;
    struct fry_mount_info entries[FRY_MAX_MOUNT_INFO];
};
long fry_mounts_info(struct fry_mounts_info *out);

struct fry_mount_dbg {
    char     mount[64];
    uint8_t  fs_type;      /* 0=none, 1=FAT32, 2=ToTFS, 3=NTFS, 4=ramdisk */
    uint8_t  pad[3];
    uint32_t sector_size;  /* logical sector size */
    uint32_t block_size;   /* cluster/block size */
    uint64_t part_lba;     /* partition start LBA */
};

struct fry_mounts_dbg {
    uint32_t count;
    struct fry_mount_dbg entries[FRY_MAX_MOUNT_INFO];
};
long fry_mounts_dbg(struct fry_mounts_dbg *out);

// Non-blocking getchar: returns -1 if no key is ready.
int getchar_nb(void);

/* =====================================================================
 * Phase 8: Userspace Porting Layer
 * ===================================================================== */

/* -----------------------------------------------------------------------
 * stdio.c — FILE streams
 * ----------------------------------------------------------------------- */

#define _TATER_LIBC_FILE_DEFINED
typedef struct _FILE FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

FILE *fopen(const char *path, const char *mode);
FILE *fdopen(int fd, const char *mode);
int fclose(FILE *stream);
int fflush(FILE *stream);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);
int fgetc(FILE *stream);
int fputc(int c, FILE *stream);
int ungetc(int c, FILE *stream);
char *fgets(char *s, int n, FILE *stream);
int fputs(const char *s, FILE *stream);
int fprintf(FILE *stream, const char *fmt, ...);
int vfprintf(FILE *stream, const char *fmt, va_list ap);
int sprintf(char *buf, const char *fmt, ...);
int sscanf(const char *str, const char *fmt, ...);
int vsscanf(const char *str, const char *fmt, va_list ap);
int fscanf(FILE *stream, const char *fmt, ...);
int fseek(FILE *stream, long offset, int whence);
long ftell(FILE *stream);
void rewind(FILE *stream);
int feof(FILE *stream);
int ferror(FILE *stream);
void clearerr(FILE *stream);
int fileno(FILE *stream);
int setvbuf(FILE *stream, char *buf, int mode, size_t size);
void setbuf(FILE *stream, char *buf);
int remove_file(const char *path);
int rename_file(const char *oldpath, const char *newpath);

/* stdio buffering modes */
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

/* SEEK constants (aliases for fry_seek.h values) */
#define SEEK_SET FRY_SEEK_SET
#define SEEK_CUR FRY_SEEK_CUR
#define SEEK_END FRY_SEEK_END

/* EOF */
#ifndef EOF
#define EOF (-1)
#endif

/* -----------------------------------------------------------------------
 * string_ext.c — Extended string functions
 * ----------------------------------------------------------------------- */

char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);
char *strpbrk(const char *s, const char *accept);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char *strtok(char *str, const char *delim);
char *strtok_r(char *str, const char *delim, char **saveptr);
char *strdup(const char *s);
char *strndup(const char *s, size_t n);
void *memchr(const void *s, int c, size_t n);
long strtol(const char *nptr, char **endptr, int base);
unsigned long strtoul(const char *nptr, char **endptr, int base);
long long strtoll(const char *nptr, char **endptr, int base);
unsigned long long strtoull(const char *nptr, char **endptr, int base);
double strtod(const char *nptr, char **endptr);
float strtof(const char *nptr, char **endptr);
char *strerror(int errnum);
void perror(const char *s);

/* ctype functions */
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
int toupper(int c);
int tolower(int c);

/* abs/labs/llabs */
int abs(int x);
long labs(long x);
long long llabs(long long x);

/* -----------------------------------------------------------------------
 * fenv.c — Floating-point environment
 * ----------------------------------------------------------------------- */

typedef unsigned int fexcept_t;

typedef struct {
    int __round;
    unsigned int __excepts;
} fenv_t;

#define FE_INVALID    0x01
#define FE_DIVBYZERO  0x04
#define FE_OVERFLOW   0x08
#define FE_UNDERFLOW  0x10
#define FE_INEXACT    0x20
#define FE_ALL_EXCEPT (FE_INVALID | FE_DIVBYZERO | FE_OVERFLOW | FE_UNDERFLOW | FE_INEXACT)

#define FE_TONEAREST  0
#define FE_DOWNWARD   1
#define FE_UPWARD     2
#define FE_TOWARDZERO 3

extern const fenv_t __fenv_dfl_env;

int feclearexcept(int excepts);
int fegetexceptflag(fexcept_t *flagp, int excepts);
int feraiseexcept(int excepts);
int fesetexceptflag(const fexcept_t *flagp, int excepts);
int fetestexcept(int excepts);
int fegetround(void);
int fesetround(int round);
int fegetenv(fenv_t *envp);
int feholdexcept(fenv_t *envp);
int fesetenv(const fenv_t *envp);
int feupdateenv(const fenv_t *envp);

/* -----------------------------------------------------------------------
 * math.c — Math library
 * ----------------------------------------------------------------------- */

double fabs(double x);
float  fabsf(float x);
double fmin(double x, double y);
double fmax(double x, double y);
float  fminf(float x, float y);
float  fmaxf(float x, float y);
double copysign(double mag, double sgn);
double floor(double x);
float  floorf(float x);
double ceil(double x);
float  ceilf(float x);
double round(double x);
float  roundf(float x);
long lround(double x);
long long llround(double x);
double trunc(double x);
float  truncf(float x);
double fmod(double x, double y);
float  fmodf(float x, float y);
double remainder(double x, double y);
double sqrt(double x);
float  sqrtf(float x);
double cbrt(double x);
double hypot(double x, double y);
double exp(double x);
float  expf(float x);
double exp2(double x);
double log(double x);
float  logf(float x);
double log2(double x);
double log10(double x);
float  log10f(float x);
float  log2f(float x);
double log1p(double x);
double expm1(double x);
double pow(double base, double exponent);
float  powf(float base, float exp_);
double sin(double x);
float  sinf(float x);
double cos(double x);
float  cosf(float x);
double tan(double x);
float  tanf(float x);
double atan(double x);
float  atanf(float x);
double atan2(double y, double x);
float  atan2f(float y, float x);
double asin(double x);
float  asinf(float x);
double acos(double x);
float  acosf(float x);
double sinh(double x);
double cosh(double x);
double tanh(double x);
double ldexp(double x, int exp_);
double frexp(double x, int *exp_);
double modf(double x, double *iptr);
double scalbn(double x, int n);
int isinf_d(double x);
int isnan_d(double x);
int isfinite_d(double x);

#define HUGE_VAL  1e308
#define INFINITY  __builtin_inf()
#define NAN       __builtin_nan("")

/* -----------------------------------------------------------------------
 * posix.c — POSIX compatibility shims
 * ----------------------------------------------------------------------- */

/* errno */
int *__errno_location(void);
#define errno (*__errno_location())

/* Signals (stubs) */
typedef void (*sighandler_t)(int);
sighandler_t signal(int sig, sighandler_t handler);
int raise_compat(int sig);

/* Directory streams */
typedef struct _DIR DIR;

struct dirent_compat {
    uint32_t d_ino;
    uint8_t  d_type;
    char     d_name[256];
};

#define DT_UNKNOWN 0
#define DT_REG     8
#define DT_DIR     4

DIR *opendir(const char *path);
struct dirent_compat *readdir_compat(DIR *dirp);
int closedir(DIR *dirp);

/* File system helpers */
char *getcwd(char *buf, size_t size);
int chdir(const char *path);
int access(const char *path, int mode);
int unlink(const char *path);
int rmdir(const char *path);
int mkdir_compat(const char *path, uint32_t mode);
int stat_compat(const char *path, struct fry_stat *st);
int fstat_compat(int fd, struct fry_stat *st);
int lstat_compat(const char *path, struct fry_stat *st);

/* access() mode flags */
#define F_OK 0
#define R_OK 4
#define W_OK 2
#define X_OK 1

/* Process control */
int getpid_compat(void);
int getppid_compat(void);
int getuid_compat(void);
int geteuid_compat(void);
int getgid_compat(void);
int getegid_compat(void);
pid_t fork(void);
int execve(const char *path, char *const argv[], char *const envp[]);
int execv(const char *path, char *const argv[]);
int execvp(const char *file, char *const argv[]);
pid_t waitpid(pid_t pid, int *status, int options);
pid_t wait(int *status);
__attribute__((noreturn)) void _exit_compat(int status);
__attribute__((noreturn)) void abort_compat(void);
int atexit_compat(void (*func)(void));
void exit_compat(int status);

/* Environment */
char *getenv_compat(const char *name);
int setenv_compat(const char *name, const char *value, int overwrite);
int unsetenv_compat(const char *name);

/* Misc POSIX */
unsigned int sleep_compat(unsigned int seconds);
int usleep_compat(unsigned int usec);
long sysconf_compat(int name);
int getpagesize_compat(void);
int pipe_compat(int pipefd[2]);
int dup_compat(int oldfd);
int dup2_compat(int oldfd, int newfd);
int close_compat(int fd);
long read_compat(int fd, void *buf, size_t count);
long write_compat(int fd, const void *buf, size_t count);
long lseek_compat(int fd, long offset, int whence);
int open_compat(const char *path, int flags);
int fcntl_compat(int fd, int cmd, long arg);
struct rlimit;
struct rusage;
void *mmap_compat(void *addr, size_t length, int prot, int flags, int fd, long offset);
int munmap_compat(void *addr, size_t length);
int mprotect_compat(void *addr, size_t length, int prot);
int msync(void *addr, size_t length, int flags);
int madvise(void *addr, size_t length, int advice);
int posix_madvise(void *addr, size_t length, int advice);
int getrlimit(int resource, struct rlimit *rlim);
int setrlimit(int resource, const struct rlimit *rlim);
int getrusage(int who, struct rusage *usage);
int poll_compat(struct fry_pollfd *fds, uint32_t nfds, int timeout);
int gethostname_compat(char *name, size_t len);

/* -----------------------------------------------------------------------
 * pthread.c — POSIX threads
 * ----------------------------------------------------------------------- */

typedef struct {
    struct fry_thread _thr;
    void *_ctx;
} pthread_t;

/*
 * Keep the public pthread wrappers pointer-sized so upstream libraries that
 * embed them in pointer arrays can treat the storage as opaque.
 */
typedef union {
    fry_mutex_t _m;
    void       *_align;
} pthread_mutex_t;

typedef union {
    fry_cond_t _c;
    void      *_align;
} pthread_cond_t;

typedef struct {
    fry_mutex_t _m;
    fry_cond_t  _cond;
    int         _readers;
    int         _writer;
} pthread_rwlock_t;

typedef union {
    fry_once_t _o;
    void      *_align;
} pthread_once_t;

typedef uint32_t pthread_key_t;

typedef struct {
    int _detachstate;
    size_t _stacksize;
    void *_stackaddr;
    int _scope;
    int _inheritsched;
    int _schedpolicy;
    struct sched_param { int sched_priority; } _schedparam;
} pthread_attr_t;

typedef struct {
    int _type;
} pthread_mutexattr_t;

typedef struct {
    int _clock;
} pthread_condattr_t;

typedef struct {
    int _pshared;
} pthread_rwlockattr_t;

#define PTHREAD_MUTEX_INITIALIZER  { {0u} }
#define PTHREAD_COND_INITIALIZER   { {0u} }
#define PTHREAD_RWLOCK_INITIALIZER { {0u}, {0u}, 0, 0 }
#define PTHREAD_ONCE_INIT          { {0u} }

#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED 1

#define PTHREAD_SCOPE_SYSTEM  0
#define PTHREAD_SCOPE_PROCESS 1

#define PTHREAD_INHERIT_SCHED  0
#define PTHREAD_EXPLICIT_SCHED 1

#define PTHREAD_MUTEX_NORMAL      0
#define PTHREAD_MUTEX_RECURSIVE   1
#define PTHREAD_MUTEX_ERRORCHECK  2
#define PTHREAD_MUTEX_DEFAULT     PTHREAD_MUTEX_NORMAL
#define PTHREAD_MUTEX_ADAPTIVE_NP 3
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP PTHREAD_MUTEX_INITIALIZER
#define PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP PTHREAD_MUTEX_INITIALIZER
#define PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP PTHREAD_MUTEX_INITIALIZER

#define SCHED_OTHER 0
#define SCHED_FIFO  1
#define SCHED_RR    2

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg);
int pthread_join(pthread_t thread, void **retval);
int pthread_detach(pthread_t thread);
pthread_t pthread_self(void);
int pthread_equal(pthread_t t1, pthread_t t2);
int pthread_kill(pthread_t thread, int sig);
int pthread_setname_np(pthread_t thread, const char *name);
int pthread_getname_np(pthread_t thread, char *name, size_t len);
int pthread_getattr_np(pthread_t thread, pthread_attr_t *attr);
void pthread_yield(void);

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr);
int pthread_mutex_destroy(pthread_mutex_t *mutex);
int pthread_mutex_lock(pthread_mutex_t *mutex);
int pthread_mutex_trylock(pthread_mutex_t *mutex);
int pthread_mutex_unlock(pthread_mutex_t *mutex);

int pthread_mutexattr_init(pthread_mutexattr_t *attr);
int pthread_mutexattr_destroy(pthread_mutexattr_t *attr);
int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type);

int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr);
int pthread_cond_destroy(pthread_cond_t *cond);
int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex);
int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
                           const struct fry_timespec *abstime);
int pthread_cond_signal(pthread_cond_t *cond);
int pthread_cond_broadcast(pthread_cond_t *cond);

int pthread_condattr_init(pthread_condattr_t *attr);
int pthread_condattr_destroy(pthread_condattr_t *attr);

int pthread_rwlock_init(pthread_rwlock_t *rwl, const pthread_rwlockattr_t *attr);
int pthread_rwlock_destroy(pthread_rwlock_t *rwl);
int pthread_rwlock_rdlock(pthread_rwlock_t *rwl);
int pthread_rwlock_wrlock(pthread_rwlock_t *rwl);
int pthread_rwlock_unlock(pthread_rwlock_t *rwl);

int pthread_once(pthread_once_t *once_control, void (*init_routine)(void));

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *));
int pthread_key_delete(pthread_key_t key);
int pthread_setspecific(pthread_key_t key, const void *value);
void *pthread_getspecific(pthread_key_t key);

int pthread_attr_init(pthread_attr_t *attr);
int pthread_attr_destroy(pthread_attr_t *attr);
int pthread_attr_setdetachstate(pthread_attr_t *attr, int state);
int pthread_attr_setstacksize(pthread_attr_t *attr, size_t size);
int pthread_attr_getstacksize(const pthread_attr_t *attr, size_t *size);
int pthread_attr_getstack(const pthread_attr_t *attr, void **stackaddr,
                          size_t *stacksize);
int pthread_attr_setscope(pthread_attr_t *attr, int scope);
int pthread_attr_setinheritsched(pthread_attr_t *attr, int inheritsched);
int pthread_attr_setschedpolicy(pthread_attr_t *attr, int policy);
int pthread_attr_setschedparam(pthread_attr_t *attr,
                               const struct sched_param *param);
int pthread_attr_getschedparam(const pthread_attr_t *attr,
                               struct sched_param *param);
int pthread_attr_getschedpolicy(const pthread_attr_t *attr, int *policy);
int pthread_setschedparam(pthread_t thread, int policy,
                          const struct sched_param *param);
int pthread_getschedparam(pthread_t thread, int *policy,
                          struct sched_param *param);
int sched_get_priority_min(int policy);
int sched_get_priority_max(int policy);
int sched_yield(void);

#ifdef __cplusplus
static inline bool operator==(const pthread_t &lhs, const pthread_t &rhs) {
    return lhs._thr.tid == rhs._thr.tid;
}

static inline bool operator!=(const pthread_t &lhs, const pthread_t &rhs) {
    return !(lhs == rhs);
}
#endif

/* -----------------------------------------------------------------------
 * netdb.c — Network resolver
 * ----------------------------------------------------------------------- */

struct addrinfo {
    int              ai_flags;
    int              ai_family;
    int              ai_socktype;
    int              ai_protocol;
    uint32_t         ai_addrlen;
    struct fry_sockaddr *ai_addr;
    char            *ai_canonname;
    struct addrinfo *ai_next;
};

int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **res);
void freeaddrinfo(struct addrinfo *res);
const char *gai_strerror(int errcode);
int getnameinfo(const struct fry_sockaddr *sa, uint32_t salen,
                char *host, uint32_t hostlen,
                char *serv, uint32_t servlen, int flags);
int inet_pton(int af, const char *src, void *dst);
const char *inet_ntop(int af, const void *src, char *dst, uint32_t size);
uint32_t inet_addr(const char *cp);

/* getaddrinfo flags */
#define AI_PASSIVE     0x01
#define AI_CANONNAME   0x02
#define AI_NUMERICHOST 0x04
#define AI_NUMERICSERV 0x08

/* getaddrinfo error codes */
#define EAI_NONAME   (-2)
#define EAI_AGAIN    (-3)
#define EAI_FAIL     (-4)
#define EAI_FAMILY   (-6)
#define EAI_SOCKTYPE (-7)
#define EAI_SERVICE  (-8)
#define EAI_MEMORY   (-10)
#define EAI_SYSTEM   (-11)

/* -----------------------------------------------------------------------
 * time_ext.c — Extended time functions
 * ----------------------------------------------------------------------- */

typedef int64_t time_t;

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
    long tm_gmtoff;
    const char *tm_zone;
};

struct timeval_compat {
    int64_t tv_sec;
    int64_t tv_usec;
};

time_t time_func(time_t *tloc);
struct tm *gmtime_r(const time_t *timep, struct tm *result);
struct tm *gmtime_func(const time_t *timep);
struct tm *localtime_r(const time_t *timep, struct tm *result);
struct tm *localtime_func(const time_t *timep);
extern long timezone;
extern int daylight;
extern char *tzname[2];
void tzset(void);
time_t mktime_func(struct tm *tm);
double difftime_func(time_t time1, time_t time0);
int gettimeofday_func(struct timeval_compat *tv, void *tz);
int clock_gettime_compat(int clock_id, struct fry_timespec *ts);
int nanosleep_compat(const struct fry_timespec *req, struct fry_timespec *rem);
size_t strftime_func(char *buf, size_t maxsize, const char *fmt, const struct tm *tm);
char *asctime_func(const struct tm *tm);
char *ctime_func(const time_t *timep);
long clock_func(void);

#define CLOCKS_PER_SEC 1000000

/* -----------------------------------------------------------------------
 * dlfcn.c — Dynamic loading stubs
 * ----------------------------------------------------------------------- */

void *dlopen(const char *filename, int flags);
void *dlsym(void *handle, const char *symbol);
int dlclose(void *handle);
char *dlerror(void);

#define RTLD_LAZY   0x00001
#define RTLD_NOW    0x00002
#define RTLD_GLOBAL 0x00100
#define RTLD_LOCAL  0x00000

/* -----------------------------------------------------------------------
 * Convenience: NULL, SIZE_MAX, BUFSIZ, PATH_MAX
 * ----------------------------------------------------------------------- */

#ifndef NULL
#define NULL ((void *)0)
#endif

#ifndef SIZE_MAX
#define SIZE_MAX ((size_t)-1)
#endif

#ifndef BUFSIZ
#define BUFSIZ 4096
#endif

#ifndef PATH_MAX
#define PATH_MAX 256
#endif

#ifndef SSIZE_MAX
#define SSIZE_MAX ((long)0x7FFFFFFFFFFFFFFF)
#endif

typedef long ssize_t;
typedef int64_t off_t;
typedef uint32_t mode_t;
typedef uint32_t uid_t;
typedef uint32_t gid_t;
typedef int32_t pid_t;

#endif
