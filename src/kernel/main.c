// TaterTOS64v3 kernel entry

#include <stdint.h>
#include "../boot/efi_handoff.h"
#include "../boot/early_serial.h"
#include "fs/vfs.h"
#include "mm/heap.h"
#include "proc/elf.h"

struct fry_handoff *g_handoff;

struct block_device;

void gdt_init(void);
void idt_init(void);
void tss_init(uint64_t rsp0_top);

void pmm_init(struct fry_handoff *handoff);
void pmm_relocate_bitmap(void);
void pmm_debug_dump_state(const char *tag, uint64_t order);
void vmm_init(struct fry_handoff *handoff);
void heap_init(void);

void kprint_init(struct fry_handoff *handoff);
void kprint(const char *fmt, ...);
void kernel_panic(const char *msg);
void acpi_tables_init(uint64_t rsdp_phys);
void madt_init(void);
void fadt_init(void);
void mcfg_init(void);
void hpet_tbl_init(void);
void irq_desc_init(void);
void irq_cr3_init(uint64_t cr3);
uint64_t vmm_get_kernel_pml4_phys(void);
void pic8259_init(void);
void lapic_init(void);
void ioapic_init(void);
void aml_parse_tables(void);
void acpi_events_init(void);
void acpi_events_start_worker(void);
void acpi_power_init(void);
void platform_init(void);
void platform_detect(void);
void pci_enum_all(void);
void acpi_bus_init(void);
void smp_init(void);
void ps2_ctrl_init(void);
void ps2_kbd_init(void);
void ps2_mouse_init(void);
void usb_init(void);
void xhci_init(void);
void usb_hub_init(void);
void usb_hid_init(void);
void nvme_init(void);
void vmd_init(void);
struct block_device *nvme_get_block_device(void);
void netcore_init(void);
void e1000_init(void);
void rtl8169_init(void);
void wifi_9260_init(void);
int part_init(void);
int fat32_init(struct block_device *bd);
int vfs_init(struct block_device *bd);
int vfs_init_ramdisk(uint64_t phys_base, uint64_t size);
void vfs_set_storage_device(struct block_device *bd);
int process_init(void);
int sched_init(void);
void sched_tick(void);
void syscall_init(void);
void aml_extended_init(void);
void hpet_init(void);
void hpet_sleep_ms(uint64_t ms);
void lapic_timer_init(void);
void gop_fb_init(void);
int process_launch(const char *path);
int process_last_launch_error(void);
void stack_switch_and_call(uint64_t new_rsp, void (*fn)(struct fry_handoff *), struct fry_handoff *arg);

/* For background aml_extended_init thread */
#include "proc/process.h"
void sched_add(uint32_t pid);

__attribute__((aligned(16), section(".stack")))
uint8_t kernel_stack[262144];

#define TATER_BUILD_TAG __DATE__ " " __TIME__
#define TATER_BUILD_ID  "2026-03-04-fry531-rollback"

static void aml_extended_init_thread(void *arg) {
    (void)arg;
    aml_extended_init();
}

static int dbg_vfs_emit_name(const char *name, void *ctx) {
    (void)ctx;
    if (!name || !*name) return 0;
    kprint("DBG_VFS entry=%s\n", name);
    return 0;
}

/* Draw a tiny top-row stage block for bare-metal handoff debugging. */
static void early_fb_stage(struct fry_handoff *handoff, uint64_t stage) {
    if (!handoff) return;
    if (!handoff->fb_base || !handoff->fb_width || !handoff->fb_height || !handoff->fb_stride) return;
    if (!handoff->boot_identity_limit || handoff->fb_base >= handoff->boot_identity_limit) return;

    uint64_t x0 = stage * 20ULL;
    if (x0 >= handoff->fb_width) return;

    uint64_t mw = 12ULL;
    uint64_t mh = 12ULL;
    uint64_t remain_w = handoff->fb_width - x0;
    if (remain_w < mw) mw = remain_w;
    if (handoff->fb_height < mh) mh = handoff->fb_height;

    volatile uint32_t *fb = (volatile uint32_t *)(uintptr_t)handoff->fb_base;
    for (uint64_t y = 0; y < mh; y++) {
        uint64_t row = y * handoff->fb_stride + x0;
        for (uint64_t x = 0; x < mw; x++) {
            fb[row + x] = 0x00F0F0F0u;
        }
    }
}

static void early_fb_color(struct fry_handoff *handoff, uint64_t col, uint64_t row_idx, uint32_t color) {
    if (!handoff) return;
    if (!handoff->fb_base || !handoff->fb_width || !handoff->fb_height || !handoff->fb_stride) return;
    if (!handoff->boot_identity_limit || handoff->fb_base >= handoff->boot_identity_limit) return;

    uint64_t x0 = col * 20ULL;
    uint64_t y0 = row_idx * 20ULL;
    if (x0 >= handoff->fb_width || y0 >= handoff->fb_height) return;

    uint64_t mw = 12ULL;
    uint64_t mh = 12ULL;
    uint64_t remain_w = handoff->fb_width - x0;
    uint64_t remain_h = handoff->fb_height - y0;
    if (remain_w < mw) mw = remain_w;
    if (remain_h < mh) mh = remain_h;

    volatile uint32_t *fb = (volatile uint32_t *)(uintptr_t)handoff->fb_base;
    for (uint64_t y = 0; y < mh; y++) {
        uint64_t row = (y0 + y) * handoff->fb_stride + x0;
        for (uint64_t x = 0; x < mw; x++) {
            fb[row + x] = color;
        }
    }
}

static void after_vmm(struct fry_handoff *handoff) {
    early_fb_stage(handoff, 13);
    early_serial_puts("K_AFTER_VMM_ENTRY\n");
    early_fb_stage(handoff, 14);
    early_debug_putc('a');
    // Emit a stage marker to both debugcon and serial so live QEMU output matches logs.
    #define STAGE(c) do { early_debug_putc(c); early_serial_putc(c); } while (0)
    STAGE('S');
    // Restore normal console path after VMM is live.
    kprint_init(handoff);
    early_fb_stage(handoff, 15);
    STAGE('P');
    kprint("TaterTOS64v3 starting\n");
    kprint("build: %s\n", TATER_BUILD_TAG);
    early_fb_stage(handoff, 16);
    heap_init();
    // Copy handoff into kernel heap so it's accessible even under user CR3.
    struct fry_handoff *handoff_copy = (struct fry_handoff *)kmalloc(sizeof(*handoff_copy));
    if (handoff_copy) {
        *handoff_copy = *handoff;
        g_handoff = handoff_copy;
        handoff = handoff_copy;
    }
    early_fb_stage(handoff, 17);

    // Phase 2: ACPI table layer
    STAGE('T');
    early_debug_puts("K_ACPI_TABLES\n");
    acpi_tables_init(handoff ? handoff->rsdp_phys : 0);
    STAGE('t');
    STAGE('M');
    early_debug_puts("K_ACPI_MADT\n");
    madt_init();
    STAGE('m');
    STAGE('F');
    early_debug_puts("K_ACPI_FADT\n");
    fadt_init();
    STAGE('f');
    STAGE('G');
    early_debug_puts("K_ACPI_MCFG\n");
    mcfg_init();
    STAGE('g');
    hpet_tbl_init();
    early_fb_stage(handoff, 18);
    #undef STAGE

    // Phase 3: IRQ infrastructure
    irq_desc_init();
    irq_cr3_init(vmm_get_kernel_pml4_phys());
    pic8259_init();
    lapic_init();
    ioapic_init();
    early_fb_stage(handoff, 19);

    // Phase 4: AML interpreter
    aml_parse_tables();
    acpi_events_init();
    acpi_power_init();
    early_fb_stage(handoff, 20);

    // Phase 5: Driver model
    platform_init();

    // Phase 6: PCI bus driver
    pci_enum_all();

    // Phase 6b: platform fingerprint/profile detection
    platform_detect();

    // Phase 7: ACPI bus driver
    acpi_bus_init();
    early_fb_stage(handoff, 21);

    // Phase 9: Timers must be initialized before SMP so APs can use HPET
    // for LAPIC timer calibration in ap_entry().
    hpet_init();

    // Phase 8: SMP
    smp_init();

    // BSP LAPIC timer (APs set up their own in ap_entry)
    lapic_timer_init();
    early_fb_stage(handoff, 22);

    // Phase 9: Input drivers
    kprint("init: ps2\n");
    ps2_ctrl_init();
    ps2_kbd_init();
    ps2_mouse_init();

    // Phase 9: Video
    kprint("init: gop\n");
    gop_fb_init();
    early_fb_stage(handoff, 23);

    // Phase 10: USB subsystem
    kprint("init: usb\n");
    usb_init();
    xhci_init();
    usb_hub_init();
    usb_hid_init();
    early_fb_stage(handoff, 24);

    // Phase 11: Storage
    kprint("init: storage\n");
    early_fb_stage(handoff, 25);
    kprint("storage: vmd init\n");
    vmd_init();
    early_fb_stage(handoff, 26);
    kprint("storage: nvme init\n");
    nvme_init();
    kprint("storage: done\n");
    early_fb_stage(handoff, 27);

    // Phase 12: Network
    kprint("init: net\n");
    netcore_init();
    e1000_init();
    rtl8169_init();
    wifi_9260_init();
    early_fb_stage(handoff, 28);

    // Phase 13: VFS + filesystem
    kprint("init: vfs\n");
    kprint("init: ramdisk base=0x%llx size=%llu\n",
           (unsigned long long)(handoff ? handoff->ramdisk_base : 0ULL),
           (unsigned long long)(handoff ? handoff->ramdisk_size : 0ULL));
    enum {
        ROOT_SRC_NONE = 0,
        ROOT_SRC_RAMDISK = 1,
        ROOT_SRC_BLOCK = 2
    };
    int root_source = ROOT_SRC_NONE;
    int have_ramdisk = (handoff && handoff->ramdisk_base && handoff->ramdisk_size);
    struct block_device *nvme_bd = nvme_get_block_device();
    vfs_set_storage_device(nvme_bd);
    if (nvme_bd && !have_ramdisk) {
        part_init();
        fat32_init(nvme_bd);
    }
    {
        // Choose root source once at boot and keep it fixed for this session.
        if (have_ramdisk) {
            kprint("init: vfs ramdisk primary\n");
            if (vfs_init_ramdisk(handoff->ramdisk_base, handoff->ramdisk_size) == 0) {
                root_source = ROOT_SRC_RAMDISK;
                if (nvme_bd) {
                    if (vfs_mount_secondary(nvme_bd, "/nvme") == 0) {
                        kprint("init: live mode, NVMe secondary auto-mount ready\n");
                    } else {
                        kprint("init: live mode, NVMe secondary auto-mount unavailable\n");
                    }
                }
            } else {
                kprint("init: vfs ramdisk failed\n");
            }
        } else if (nvme_bd) {
            if (vfs_init(nvme_bd) == 0) {
                root_source = ROOT_SRC_BLOCK;
            } else {
                kprint("init: vfs nvme failed, no ramdisk\n");
            }
        } else {
            kprint("init: vfs no device\n");
        }

        kprint("DBG_VFS source=%s have_rd=%d rd_base=0x%llx rd_size=%llu nvme=%d\n",
               (root_source == ROOT_SRC_RAMDISK) ? "ramdisk" :
               ((root_source == ROOT_SRC_BLOCK) ? "block" : "none"),
               have_ramdisk ? 1 : 0,
               (unsigned long long)(handoff ? handoff->ramdisk_base : 0ULL),
               (unsigned long long)(handoff ? handoff->ramdisk_size : 0ULL),
               nvme_bd ? 1 : 0);
        {
            struct vfs_stat st;
            int rc_system_init = vfs_stat("/system/INIT.FRY", &st);
            int rc_system_gui = vfs_stat("/system/GUI.FRY", &st);
            int rc_apps_shell = vfs_stat("/apps/SHELL.TOT", &st);
            int rc_apps_sysinfo = vfs_stat("/apps/SYSINFO.FRY", &st);
            int rc_root_gui = vfs_stat("/GUI.FRY", &st);
            int rc_root_sh = vfs_stat("/SHELL.TOT", &st);
            kprint("DBG_VFS stat init=%d gui_system=%d shell_apps=%d sysinfo_apps=%d gui_root=%d sh_root=%d\n",
                   rc_system_init, rc_system_gui, rc_apps_shell,
                   rc_apps_sysinfo, rc_root_gui, rc_root_sh);
            /*
             * Row 3 is the live-root payload-presence row:
             * col0=/system/INIT.FRY col1=/system/GUI.FRY col2=/apps/SHELL.TOT
             * col3=/apps/SYSINFO.FRY col4=/GUI.FRY col5=/SHELL.TOT
             * green=present red=missing
             */
            early_fb_color(handoff, 0, 3, (rc_system_init == 0) ? 0x0000FF00u : 0x00FF0000u);
            early_fb_color(handoff, 1, 3, (rc_system_gui  == 0) ? 0x0000FF00u : 0x00FF0000u);
            early_fb_color(handoff, 2, 3, (rc_apps_shell  == 0) ? 0x0000FF00u : 0x00FF0000u);
            early_fb_color(handoff, 3, 3, (rc_apps_sysinfo == 0) ? 0x0000FF00u : 0x00FF0000u);
            early_fb_color(handoff, 4, 3, (rc_root_gui    == 0) ? 0x0000FF00u : 0x00FF0000u);
            early_fb_color(handoff, 5, 3, (rc_root_sh     == 0) ? 0x0000FF00u : 0x00FF0000u);
        }
        vfs_readdir("/", dbg_vfs_emit_name, 0);
        vfs_readdir("/system", dbg_vfs_emit_name, 0);
        vfs_readdir("/apps", dbg_vfs_emit_name, 0);
        vfs_readdir("/fry", dbg_vfs_emit_name, 0);
        vfs_readdir("/FRY", dbg_vfs_emit_name, 0);
        early_fb_stage(handoff, 29);

        // Phase 14: Process + userspace
        kprint("init: proc\n");
        process_init();
        sched_init();
        syscall_init();
        // Phase 15: Launch init — session policy lives in userspace now
        kprint("init: launch\n");
        int launch_rc = process_launch("/system/INIT.FRY");
        if (launch_rc < 0)
            launch_rc = process_launch("/INIT.FRY");
        if (launch_rc < 0) {
            kprint("FATAL: no INIT.FRY found (code=%d)\n", process_last_launch_error());
            kernel_panic("cannot launch /system/INIT.FRY or /INIT.FRY");
        }
        kprint("init: user pid=%d\n", launch_rc);
        early_fb_stage(handoff, 32);
        early_serial_puts("K_BOOT_USER_ENQUEUED\n");
        early_fb_stage(handoff, 30);
    }

    /*
     * Do one direct scheduler handoff before enabling the rest of background
     * kernel work. On bare metal this removes dependence on the first LAPIC
     * timer tick for the initial jump into userspace.
     */
    sched_tick();

    // Enable interrupts now — scheduler needs LAPIC timer to keep userspace moving.
    __asm__ volatile("sti");

    /*
     * Start deferred kernel workers only after the process/scheduler layer is
     * live and the first boot userspace handoff has had a chance to run.
     */
    acpi_events_start_worker();

    // Phase 16: Extended AML coverage (background — EC probing is slow)
    kprint("init: aml_ext (background)\n");
    {
        struct fry_process *aml_t = process_create_kernel(
            aml_extended_init_thread, 0, "aml_ext");
        if (aml_t) {
            sched_add(aml_t->pid);
        } else {
            kprint("init: aml_ext thread failed, running inline\n");
            aml_extended_init();
        }
    }
    early_fb_stage(handoff, 31);

    for (;;) {
        __asm__ volatile("hlt");
    }
}

void _fry_start(struct fry_handoff *handoff) {
    early_fb_stage(handoff, 4);
    early_serial_init();
    early_serial_puts("K_START\n");
    early_serial_puts("K_BUILD " TATER_BUILD_ID "\n");
    early_debug_puts("K_BUILD " TATER_BUILD_ID "\n");
    early_debug_putc('k');
    g_handoff = handoff;
    // Low-level CPU tables
    gdt_init();
    idt_init();
    early_fb_stage(handoff, 5);
    early_serial_puts("K_GDT_IDT\n");
    early_debug_putc('g');

    // TSS setup with kernel stack
    uint64_t rsp0_top = (uint64_t)(kernel_stack + sizeof(kernel_stack));
    tss_init(rsp0_top);
    early_fb_stage(handoff, 6);
    early_serial_puts("K_TSS\n");
    early_debug_putc('t');

    // Memory managers
    pmm_init(handoff);
    early_fb_stage(handoff, 7);
    early_serial_puts("K_PMM\n");
    early_debug_putc('p');
    vmm_init(handoff);
    early_fb_stage(handoff, 8);
    early_serial_puts("K_PMM_RELOCATE_BEGIN\n");
    early_debug_putc('r');
    pmm_relocate_bitmap();
    early_serial_puts("K_PMM_RELOCATE_END\n");
    early_debug_putc('R');
    pmm_debug_dump_state("PMM_AFTER_RELOC", 8);
    early_fb_stage(handoff, 9);
    early_fb_stage(handoff, 10);
    early_serial_puts("K_VMM\n");
    early_fb_stage(handoff, 11);
    early_debug_putc('v');
    early_fb_stage(handoff, 12);
    early_serial_puts("K_STACK_SWITCH_CALL\n");
    early_debug_putc('w');
    stack_switch_and_call((uint64_t)(kernel_stack + sizeof(kernel_stack)), after_vmm, handoff);
    early_debug_putc('r');
    for (;;) {
        __asm__ volatile("hlt");
    }
}
