/*
 * selftest.c — Kernel boot-time self-test (Phase 0 ABI discipline)
 *
 * Validates core ABI invariants at boot before any userspace runs.
 * Called once from after_vmm() when TATER_SELFTEST is enabled.
 *
 * On failure: dumps the failing check name, expected/got values to serial,
 * then panics. On success: prints pass count to serial and continues boot.
 *
 * All output goes through early_serial_puts (bypasses kprint filter).
 */

#include <stdint.h>
#include <errno.h>
#include "proc/process.h"
#include "../boot/early_serial.h"

void kernel_panic(const char *msg);

static uint32_t st_pass;
static uint32_t st_fail;

static void st_put_dec(uint64_t v) {
    char buf[21];
    int pos = 20;
    buf[pos] = '\0';
    if (v == 0) { early_serial_puts("0"); return; }
    while (v > 0) {
        buf[--pos] = '0' + (char)(v % 10);
        v /= 10;
    }
    early_serial_puts(&buf[pos]);
}

static void st_check(const char *name, uint64_t got, uint64_t want) {
    if (got == want) {
        st_pass++;
        return;
    }
    st_fail++;
    early_serial_puts("SELFTEST FAIL: ");
    early_serial_puts(name);
    early_serial_puts("  got=0x");
    early_serial_puthex64(got);
    early_serial_puts("  want=0x");
    early_serial_puthex64(want);
    early_serial_puts("\n");
    kernel_panic("kernel self-test failed");
}

void kernel_selftest(void) {
    st_pass = 0;
    st_fail = 0;
    early_serial_puts("selftest: running\n");

    /* errno.h values match Linux ABI numbering */
    st_check("EPERM",  EPERM,   1);
    st_check("ENOENT", ENOENT,  2);
    st_check("EBADF",  EBADF,   9);
    st_check("ENOMEM", ENOMEM, 12);
    st_check("EFAULT", EFAULT, 14);
    st_check("EINVAL", EINVAL, 22);
    st_check("ENOSYS", ENOSYS, 38);

    /* Struct/constant sizes */
    st_check("PROC_MAX",       PROC_MAX,       256);
    st_check("PROC_OUTBUF",    PROC_OUTBUF,    512);
    st_check("PROC_INBUF",     PROC_INBUF,     512);
    st_check("PROC_VMREG_MAX", PROC_VMREG_MAX, 256);

    /* Negative errno encoding: -(errno) must produce correct uint64_t */
    st_check("-EINVAL", (uint64_t)-EINVAL, 0xFFFFFFFFFFFFFFEAULL);
    st_check("-EBADF",  (uint64_t)-EBADF,  0xFFFFFFFFFFFFFFF7ULL);

    early_serial_puts("selftest: ");
    st_put_dec(st_pass);
    early_serial_puts(" passed, 0 failed\n");
}
