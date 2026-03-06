CC = x86_64-elf-gcc
AS = nasm
LD = x86_64-elf-ld

CFLAGS = -ffreestanding -fno-stack-protector -mno-red-zone -mcmodel=kernel -O2 -std=gnu11 -Wall -Wextra
UFLAGS = -ffreestanding -fno-stack-protector -O2 -std=gnu11 -Wall -Wextra

KERNEL_OBJS = \
  src/boot/efi_main.o \
  src/boot/efi_entry.o \
  src/boot/efi_boot.o \
  src/boot/stack_switch.o \
  src/boot/gdt.o \
  src/boot/idt.o \
  src/boot/idt_stubs.o \
  src/boot/tss.o \
  src/kernel/main.o \
  src/kernel/kprint.o \
  src/kernel/panic.o \
  src/kernel/mm/pmm.o \
  src/kernel/mm/vmm.o \
  src/kernel/mm/heap.o \
  src/kernel/acpi/tables.o \
  src/kernel/acpi/madt.o \
  src/kernel/acpi/fadt.o \
  src/kernel/acpi/mcfg.o \
  src/kernel/acpi/hpet_tbl.o \
  src/kernel/acpi/namespace.o \
  src/kernel/acpi/aml_types.o \
  src/kernel/acpi/aml_parse.o \
  src/kernel/acpi/aml_exec.o \
  src/kernel/acpi/aml_ops.o \
  src/kernel/acpi/notify.o \
  src/kernel/acpi/ec.o \
  src/kernel/acpi/extended.o \
  src/kernel/acpi/resources.o \
  src/kernel/acpi/events.o \
  src/kernel/acpi/power.o \
  src/kernel/irq/irqdesc.o \
  src/kernel/irq/chip.o \
  src/kernel/irq/manage.o \
  src/kernel/drivers/bus.o \
  src/kernel/drivers/device.o \
  src/kernel/drivers/driver.o \
  src/kernel/drivers/platform.o \
  src/drivers/irqchip/pic8259.o \
  src/drivers/irqchip/lapic.o \
  src/drivers/irqchip/ioapic.o \
  src/drivers/pci/pci_ecam.o \
  src/drivers/pci/pci_core.o \
  src/drivers/pci/msi.o \
  src/drivers/acpi_bus/acpi_bus.o \
  src/drivers/smp/smp.o \
  src/drivers/smp/trampoline.o \
  src/drivers/smp/spinlock.o \
  src/drivers/timer/hpet.o \
  src/drivers/timer/lapic_timer.o \
  src/drivers/video/gop_fb.o \
  src/drivers/input/ps2_ctrl.o \
  src/drivers/input/ps2_kbd.o \
  src/drivers/input/ps2_mouse.o \
  src/drivers/usb/xhci.o \
  src/drivers/usb/usb_core.o \
  src/drivers/usb/usb_hub.o \
  src/drivers/usb/usb_hid.o \
  src/drivers/storage/nvme.o \
  src/drivers/storage/vmd.o \
  src/drivers/net/netcore.o \
  src/drivers/net/e1000.o \
  src/drivers/net/rtl8169.o \
  src/drivers/net/wifi_9260.o \
  src/drivers/net/iwlwifi_fw.o \
  src/kernel/fs/part.o \
  src/kernel/fs/fat32.o \
  src/kernel/fs/totfs.o \
  src/kernel/fs/ntfs.o \
  src/kernel/fs/vfs.o \
  src/kernel/proc/process.o \
  src/kernel/proc/sched.o \
  src/kernel/proc/elf.o \
  src/kernel/proc/syscall.o

LIBC_OBJ = src/user/libc/libc.o src/user/libc/gfx.o
INIT_OBJS = $(LIBC_OBJ) src/user/init/init.o
SHELL_OBJS = $(LIBC_OBJ) src/user/shell/shell.o
GUI_OBJS = $(LIBC_OBJ) src/user/gui/gui.o
SYSINFO_OBJS = $(LIBC_OBJ) src/user/apps/sysinfo.o
UPTIME_OBJS = $(LIBC_OBJ) src/user/apps/uptime.o
PS_OBJS = $(LIBC_OBJ) src/user/apps/ps.o
FILEMAN_OBJS = $(LIBC_OBJ) src/user/apps/fileman.o

all: kernel init shell gui sysinfo uptime ps fileman hosttools

test-elf-bounds: out/elf_loader_bounds_test
	./out/elf_loader_bounds_test

test-storage-fuzz: out/storage_meta_fuzz
	./out/storage_meta_fuzz

out/elf_loader_bounds_test: tools/elf_loader_bounds_test.c src/kernel/proc/elf.c src/kernel/proc/elf.h
	mkdir -p out
	gcc -O2 -std=gnu11 -Wall -Wextra -I. tools/elf_loader_bounds_test.c src/kernel/proc/elf.c -o out/elf_loader_bounds_test

out/storage_meta_fuzz: tools/storage_meta_fuzz.c src/kernel/fs/part.c src/kernel/fs/part.h src/kernel/fs/ntfs.c src/kernel/fs/ntfs.h src/kernel/fs/vfs.h
	mkdir -p out
	gcc -O2 -std=gnu11 -Wall -Wextra -ffunction-sections -fdata-sections -I. \
	  tools/storage_meta_fuzz.c src/kernel/fs/part.c src/kernel/fs/ntfs.c \
	  -Wl,--gc-sections -o out/storage_meta_fuzz

hosttools:
	gcc -O2 -o tools/mktotfs tools/mktotfs.c
	gcc -O2 -o tools/totcopy tools/totcopy.c

src/boot/idt_stubs.o: src/boot/idt_stubs.asm
	$(AS) -f elf64 $< -o $@

src/boot/multiboot2.o: src/boot/multiboot2.asm
	$(AS) -f elf64 $< -o $@

src/boot/mb_entry.o: src/boot/mb_entry.asm
	$(AS) -f elf64 $< -o $@

src/boot/efi_entry.o: src/boot/efi_entry.asm
	$(AS) -f elf64 $< -o $@

src/boot/efi_boot.o: src/boot/efi_boot.asm
	$(AS) -f elf64 $< -o $@

src/boot/stack_switch.o: src/boot/stack_switch.asm
	$(AS) -f elf64 $< -o $@

src/drivers/smp/trampoline.o: src/drivers/smp/trampoline.asm
	$(AS) -f elf64 $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

src/user/%.o: src/user/%.c
	$(CC) $(UFLAGS) -c $< -o $@

kernel: $(KERNEL_OBJS)
	$(LD) -T kernel_linker.ld -o kernel.elf $(KERNEL_OBJS)

init: $(INIT_OBJS)
	$(LD) -T user_linker.ld -o init.elf $(INIT_OBJS)
	./tools/frypack.py init.elf init.fry

shell: $(SHELL_OBJS)
	$(LD) -T user_linker.ld -o shell.elf $(SHELL_OBJS)
	./tools/totpack.py shell.elf shell.tot 0x0002

gui: $(GUI_OBJS)
	$(LD) -T user_linker.ld -o gui.elf $(GUI_OBJS)
	./tools/frypack.py gui.elf gui.fry

sysinfo: $(SYSINFO_OBJS)
	$(LD) -T user_linker.ld -o sysinfo.elf $(SYSINFO_OBJS)
	./tools/frypack.py sysinfo.elf sysinfo.fry

uptime: $(UPTIME_OBJS)
	$(LD) -T user_linker.ld -o uptime.elf $(UPTIME_OBJS)
	./tools/frypack.py uptime.elf uptime.fry

ps: $(PS_OBJS)
	$(LD) -T user_linker.ld -o ps.elf $(PS_OBJS)
	./tools/frypack.py ps.elf ps.fry

fileman: $(FILEMAN_OBJS)
	$(LD) -T user_linker.ld -o fileman.elf $(FILEMAN_OBJS)
	./tools/frypack.py fileman.elf fileman.fry

clean:
	rm -f $(KERNEL_OBJS) $(LIBC_OBJ) src/user/libc/gfx.o \
	  src/user/init/init.o src/user/shell/shell.o src/user/gui/gui.o src/user/apps/sysinfo.o \
	  src/user/apps/uptime.o src/user/apps/ps.o src/user/apps/fileman.o \
	  kernel.elf init.elf init.fry shell.elf shell.tot shell.fry gui.elf gui.fry \
	  sysinfo.elf sysinfo.fry uptime.elf uptime.fry ps.elf ps.fry \
	  fileman.elf fileman.fry \
	  tools/mktotfs tools/totcopy
