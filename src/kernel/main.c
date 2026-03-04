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
void acpi_power_init(void);
void platform_init(void);
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
int fat32_init(void);
int vfs_init(struct block_device *bd);
int vfs_init_ramdisk(uint64_t phys_base, uint64_t size);
int process_init(void);
int sched_init(void);
void syscall_init(void);
void aml_extended_init(void);
void hpet_init(void);
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
#define TATER_BUILD_ID  "2026-02-26-vfsprobe2"

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

static void after_vmm(struct fry_handoff *handoff) {
    (void)handoff;
    // Emit a stage marker to both debugcon and serial so live QEMU output matches logs.
    #define STAGE(c) do { early_debug_putc(c); early_serial_putc(c); } while (0)
    STAGE('S');
    // Restore normal console path after VMM is live.
    kprint_init(handoff);
    STAGE('P');
    kprint("TaterTOS64v3 starting\n");
    kprint("build: %s\n", TATER_BUILD_TAG);
    heap_init();
    // Copy handoff into kernel heap so it's accessible even under user CR3.
    struct fry_handoff *handoff_copy = (struct fry_handoff *)kmalloc(sizeof(*handoff_copy));
    if (handoff_copy) {
        *handoff_copy = *handoff;
        g_handoff = handoff_copy;
        handoff = handoff_copy;
    }

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
    #undef STAGE

    // Phase 3: IRQ infrastructure
    irq_desc_init();
    irq_cr3_init(vmm_get_kernel_pml4_phys());
    pic8259_init();
    lapic_init();
    ioapic_init();

    // Phase 4: AML interpreter
    aml_parse_tables();
    acpi_events_init();
    acpi_power_init();

    // Phase 5: Driver model
    platform_init();

    // Phase 6: PCI bus driver
    pci_enum_all();

    // Phase 7: ACPI bus driver
    acpi_bus_init();

    // Phase 9: Timers must be initialized before SMP so APs can use HPET
    // for LAPIC timer calibration in ap_entry().
    hpet_init();

    // Phase 8: SMP
    smp_init();

    // BSP LAPIC timer (APs set up their own in ap_entry)
    lapic_timer_init();

    // Phase 9: Input drivers
    kprint("init: ps2\n");
    ps2_ctrl_init();
    ps2_kbd_init();
    ps2_mouse_init();

    // Phase 9: Video
    kprint("init: gop\n");
    gop_fb_init();

    // Phase 10: USB subsystem
    kprint("init: usb\n");
    usb_init();
    xhci_init();
    usb_hub_init();
    usb_hid_init();

    // Phase 11: Storage
    kprint("init: storage\n");
    kprint("storage: vmd init\n");
    vmd_init();
    kprint("storage: nvme init\n");
    nvme_init();
    kprint("storage: done\n");

    // Phase 12: Network
    kprint("init: net\n");
    netcore_init();
    e1000_init();
    rtl8169_init();
    wifi_9260_init();

    // Phase 13: VFS + filesystem
    kprint("init: vfs\n");
    kprint("init: ramdisk base=0x%llx size=%llu\n",
           (unsigned long long)(handoff ? handoff->ramdisk_base : 0ULL),
           (unsigned long long)(handoff ? handoff->ramdisk_size : 0ULL));
    int vfs_uses_ramdisk = 0;
    part_init();
    fat32_init();
    {
        int have_ramdisk = (handoff && handoff->ramdisk_base && handoff->ramdisk_size);
        struct block_device *nvme_bd = nvme_get_block_device();

        // Live-media behavior: if a boot ramdisk exists, prefer it over host NVMe.
        if (have_ramdisk) {
            kprint("init: vfs ramdisk primary\n");
            if (vfs_init_ramdisk(handoff->ramdisk_base, handoff->ramdisk_size) == 0) {
                vfs_uses_ramdisk = 1;
                /* Mount NVMe as secondary alongside ramdisk */
                if (nvme_bd) {
                    kprint("init: mounting NVMe secondary at /nvme\n");
                    if (vfs_mount_secondary(nvme_bd, "/nvme") != 0) {
                        kprint("init: NVMe secondary mount failed (no supported FS)\n");
                    }
                }
            } else if (nvme_bd) {
                kprint("init: vfs ramdisk failed, trying nvme\n");
                if (vfs_init(nvme_bd) != 0) {
                    kprint("init: vfs nvme failed after ramdisk\n");
                }
            } else {
                kprint("init: vfs ramdisk failed, no device\n");
            }
        } else if (nvme_bd) {
            if (vfs_init(nvme_bd) != 0) {
                kprint("init: vfs nvme failed, no ramdisk\n");
            }
        } else {
            kprint("init: vfs no device\n");
        }

        kprint("DBG_VFS source=%s have_rd=%d rd_base=0x%llx rd_size=%llu nvme=%d\n",
               vfs_uses_ramdisk ? "ramdisk" : "block",
               have_ramdisk ? 1 : 0,
               (unsigned long long)(handoff ? handoff->ramdisk_base : 0ULL),
               (unsigned long long)(handoff ? handoff->ramdisk_size : 0ULL),
               nvme_bd ? 1 : 0);
        {
            struct vfs_stat st;
            int rc_root_gui = vfs_stat("/GUI.FRY", &st);
            int rc_dir_gui = vfs_stat("/fry/GUI.FRY", &st);
            int rc_dir_gui_uc = vfs_stat("/FRY/GUI.FRY", &st);
            int rc_efi_gui = vfs_stat("/EFI/FRY/GUI.FRY", &st);
            int rc_root_sh = vfs_stat("/SHELL.TOT", &st);
            int rc_dir_sh = vfs_stat("/fry/SHELL.TOT", &st);
            int rc_dir_sh_uc = vfs_stat("/FRY/SHELL.TOT", &st);
            int rc_efi_sh = vfs_stat("/EFI/FRY/SHELL.TOT", &st);
            kprint("DBG_VFS stat gui_root=%d gui_dir=%d gui_dir_uc=%d gui_efi=%d sh_root=%d sh_dir=%d sh_dir_uc=%d sh_efi=%d\n",
                   rc_root_gui, rc_dir_gui, rc_dir_gui_uc, rc_efi_gui,
                   rc_root_sh, rc_dir_sh, rc_dir_sh_uc, rc_efi_sh);
        }
        vfs_readdir("/", dbg_vfs_emit_name, 0);
        vfs_readdir("/fry", dbg_vfs_emit_name, 0);
        vfs_readdir("/FRY", dbg_vfs_emit_name, 0);
        vfs_readdir("/EFI", dbg_vfs_emit_name, 0);
        vfs_readdir("/EFI/FRY", dbg_vfs_emit_name, 0);
        vfs_readdir("/EFI/BOOT", dbg_vfs_emit_name, 0);
        vfs_readdir("/EFI/BOOT/FRY", dbg_vfs_emit_name, 0);

        // Phase 14: Process + userspace
        kprint("init: proc\n");
        process_init();
        sched_init();
        syscall_init();

        // Phase 15: User GUI shell
        kprint("init: launch\n");
        const char *gui_paths[] = {
            "/GUI.FRY",
            "/fry/GUI.FRY",
            "/FRY/GUI.FRY",
            "/EFI/fry/GUI.FRY",
            "/EFI/FRY/GUI.FRY",
            "/EFI/BOOT/GUI.FRY",
            "/EFI/BOOT/fry/GUI.FRY",
            "/EFI/BOOT/FRY/GUI.FRY"
        };
        const char *shell_paths[] = {
            "/SHELL.TOT",
            "/fry/SHELL.TOT",
            "/FRY/SHELL.TOT",
            "/EFI/fry/SHELL.TOT",
            "/EFI/FRY/SHELL.TOT",
            "/EFI/BOOT/SHELL.TOT",
            "/EFI/BOOT/fry/SHELL.TOT",
            "/EFI/BOOT/FRY/SHELL.TOT"
        };
        int launch_rc = -1;
        const char *launch_path = 0;
        for (uint32_t i = 0; i < (uint32_t)(sizeof(gui_paths) / sizeof(gui_paths[0])) && launch_rc < 0; i++) {
            launch_path = gui_paths[i];
            launch_rc = process_launch(launch_path);
            if (launch_rc < 0 &&
                !vfs_uses_ramdisk &&
                process_last_launch_error() == ELF_LOAD_ERR_OPEN &&
                handoff && handoff->ramdisk_base && handoff->ramdisk_size &&
                vfs_init_ramdisk(handoff->ramdisk_base, handoff->ramdisk_size) == 0) {
                vfs_uses_ramdisk = 1;
                kprint("init: GUI open failed on primary VFS, ramdisk retry\n");
                launch_rc = process_launch(launch_path);
            }
        }
        if (launch_rc < 0) {
            kprint("ERROR: GUI launch failed path=%s code=%d\n",
                   launch_path ? launch_path : "/GUI.FRY",
                   process_last_launch_error());
            for (uint32_t i = 0; i < (uint32_t)(sizeof(shell_paths) / sizeof(shell_paths[0])) && launch_rc < 0; i++) {
                launch_path = shell_paths[i];
                launch_rc = process_launch(launch_path);
                if (launch_rc < 0 &&
                    !vfs_uses_ramdisk &&
                    process_last_launch_error() == ELF_LOAD_ERR_OPEN &&
                    handoff && handoff->ramdisk_base && handoff->ramdisk_size &&
                    vfs_init_ramdisk(handoff->ramdisk_base, handoff->ramdisk_size) == 0) {
                    vfs_uses_ramdisk = 1;
                    kprint("init: SHELL open failed on primary VFS, ramdisk retry\n");
                    launch_rc = process_launch(launch_path);
                }
            }
        }
        if (launch_rc < 0) {
            kprint("ERROR: SHELL launch failed path=%s code=%d\n",
                   launch_path ? launch_path : "/SHELL.TOT",
                   process_last_launch_error());
        } else {
            kprint("init: user pid=%d\n", launch_rc);
        }
    }

    // Enable interrupts now — scheduler needs LAPIC timer to run GUI.
    // Phase 16 (aml_extended_init) runs in a background kernel thread
    // so it doesn't block the GUI from appearing.
    __asm__ volatile("sti");

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

    for (;;) {
        __asm__ volatile("hlt");
    }
}

void _fry_start(struct fry_handoff *handoff) {
    early_serial_init();
    early_serial_puts("K_START\n");
    early_serial_puts("K_BUILD " TATER_BUILD_ID "\n");
    early_debug_puts("K_BUILD " TATER_BUILD_ID "\n");
    early_debug_putc('k');
    g_handoff = handoff;
    // Low-level CPU tables
    gdt_init();
    idt_init();
    early_serial_puts("K_GDT_IDT\n");
    early_debug_putc('g');

    // TSS setup with kernel stack
    uint64_t rsp0_top = (uint64_t)(kernel_stack + sizeof(kernel_stack));
    tss_init(rsp0_top);
    early_serial_puts("K_TSS\n");
    early_debug_putc('t');

    // Memory managers
    pmm_init(handoff);
    early_serial_puts("K_PMM\n");
    early_debug_putc('p');
    vmm_init(handoff);
    pmm_relocate_bitmap();
    early_serial_puts("K_VMM\n");
    early_debug_putc('v');
    stack_switch_and_call((uint64_t)(kernel_stack + sizeof(kernel_stack)), after_vmm, handoff);
    early_debug_putc('r');
    for (;;) {
        __asm__ volatile("hlt");
    }
}
