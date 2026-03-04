// Kernel panic handler

#include <stdint.h>

void kprint(const char *fmt, ...);

static inline uint64_t read_cr2(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr2, %0" : "=r"(v));
    return v;
}

static inline uint64_t read_cr3(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}

void kernel_panic(const char *msg) {
    uint64_t rsp = 0;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));

    kprint("KERNEL PANIC: %s\n", msg ? msg : "(null)");
    kprint("RSP=0x%llx CR2=0x%llx CR3=0x%llx\n", rsp, read_cr2(), read_cr3());

    __asm__ volatile("cli");
    for (;;) {
        __asm__ volatile("hlt");
    }
}
