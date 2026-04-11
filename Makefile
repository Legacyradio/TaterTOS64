TOOLBIN = $(CURDIR)/tools/host/bin

CC = $(TOOLBIN)/x86_64-elf-gcc
AS = nasm
LD = $(TOOLBIN)/x86_64-elf-ld
HOSTCC ?= gcc

CXX = $(TOOLBIN)/x86_64-elf-g++
CFLAGS = -ffreestanding -fno-stack-protector -mno-red-zone -mcmodel=kernel -O2 -std=gnu11 -Wall -Wextra -Isrc/include
UFLAGS = -ffreestanding -fno-stack-protector -O2 -std=gnu11 -Wall -Wextra -Isrc/include
UCXXFLAGS = -ffreestanding -fno-stack-protector -nostdinc++ -O2 -std=c++11 -fno-exceptions -fno-rtti -Wall -Wextra -Isrc/include

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
  src/drivers/timer/rtc.o \
  src/kernel/entropy/entropy.o \
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
  src/drivers/net/i219.o \
  src/drivers/net/e1000.o \
  src/drivers/net/rtl8169.o \
  src/drivers/net/wifi_9260.o \
  src/drivers/net/iwlwifi_fw.o \
  src/drivers/net/iwlwifi_fw_blob.o \
  src/drivers/net/iwl_mac80211.o \
  src/drivers/net/iwl_crypto.o \
  src/drivers/net/iwl_wpa2.o \
  src/kernel/fs/part.o \
  src/kernel/fs/fat32.o \
  src/kernel/fs/totfs.o \
  src/kernel/fs/ntfs.o \
  src/kernel/fs/vfs.o \
  src/kernel/proc/process.o \
  src/kernel/proc/sched.o \
  src/kernel/proc/elf.o \
  src/kernel/proc/syscall.o \
  src/kernel/selftest.o \
  src/drivers/audio/hda.o

LIBC_OBJ = src/user/libc/libc.o src/user/libc/gfx.o \
  src/user/libc/string_ext.o src/user/libc/stdio.o src/user/libc/math.o \
  src/user/libc/posix.o src/user/libc/pthread.o src/user/libc/netdb.o \
  src/user/libc/time_ext.o src/user/libc/dlfcn.o src/user/libc/setjmp.o
INIT_OBJS = $(LIBC_OBJ) src/user/init/init.o
SHELL_OBJS = $(LIBC_OBJ) src/user/shell/shell.o
GUI_OBJS = $(LIBC_OBJ) src/user/gui/gui.o
SYSINFO_OBJS = $(LIBC_OBJ) src/user/apps/sysinfo.o
UPTIME_OBJS = $(LIBC_OBJ) src/user/apps/uptime.o
PS_OBJS = $(LIBC_OBJ) src/user/apps/ps.o
FILEMAN_OBJS = $(LIBC_OBJ) src/user/apps/fileman.o
NETMGR_OBJS = $(LIBC_OBJ) src/user/apps/netmgr.o
VMTEST_OBJS = $(LIBC_OBJ) src/user/apps/vmtest.o
VMFAULT_OBJS = $(LIBC_OBJ) src/user/apps/vmfault.o
ABITEST_OBJS = $(LIBC_OBJ) src/user/apps/abitest.o
THTEST_OBJS = $(LIBC_OBJ) src/user/apps/thtest.o
EVLOOP_OBJS = $(LIBC_OBJ) src/user/apps/evloop.o

# TaterSurf: BearSSL objects + browser app
BEARSSL_SRC = src/user/apps/bearssl/src
BEARSSL_OBJS = $(patsubst %.c,%.o,$(wildcard $(BEARSSL_SRC)/aead/*.c)) \
  $(patsubst %.c,%.o,$(wildcard $(BEARSSL_SRC)/codec/*.c)) \
  $(patsubst %.c,%.o,$(wildcard $(BEARSSL_SRC)/ec/*.c)) \
  $(patsubst %.c,%.o,$(wildcard $(BEARSSL_SRC)/hash/*.c)) \
  $(patsubst %.c,%.o,$(wildcard $(BEARSSL_SRC)/int/*.c)) \
  $(patsubst %.c,%.o,$(wildcard $(BEARSSL_SRC)/kdf/*.c)) \
  $(patsubst %.c,%.o,$(wildcard $(BEARSSL_SRC)/mac/*.c)) \
  $(patsubst %.c,%.o,$(wildcard $(BEARSSL_SRC)/rand/*.c)) \
  $(patsubst %.c,%.o,$(wildcard $(BEARSSL_SRC)/rsa/*.c)) \
  $(patsubst %.c,%.o,$(wildcard $(BEARSSL_SRC)/ssl/*.c)) \
  $(patsubst %.c,%.o,$(wildcard $(BEARSSL_SRC)/symcipher/*.c)) \
  $(patsubst %.c,%.o,$(wildcard $(BEARSSL_SRC)/x509/*.c)) \
  $(BEARSSL_SRC)/settings.o \
  src/user/apps/bearssl/tatertos_rt.o

# QuickJS objects
QUICKJS_DIR = src/user/apps/quickjs
QUICKJS_OBJS = $(QUICKJS_DIR)/quickjs.o $(QUICKJS_DIR)/cutils.o \
  $(QUICKJS_DIR)/libregexp.o $(QUICKJS_DIR)/libunicode.o $(QUICKJS_DIR)/libbf.o

# OpenH264 decoder objects (C++)
OH264_DIR = src/user/apps/openh264
OH264_COMMON = $(patsubst %.cpp,%.o,$(wildcard $(OH264_DIR)/common/src/*.cpp))
OH264_DECODER = $(patsubst %.cpp,%.o,$(wildcard $(OH264_DIR)/decoder/core/src/*.cpp))
OH264_PLUS = $(patsubst %.cpp,%.o,$(wildcard $(OH264_DIR)/decoder/plus/src/*.cpp))
OH264_OBJS = $(OH264_COMMON) $(OH264_DECODER) $(OH264_PLUS)
OH264_CXXRT = $(OH264_DIR)/cxxrt.o
OH264_WRAP = src/user/apps/ts_h264_wrap.o
OH264_THREAD_STUBS = $(OH264_DIR)/wels_thread_stubs.o

# libopus objects (full library — encoder dead code gets excluded by linker)
OPUS_DIR = src/user/apps/opus
OPUS_CELT_OBJS = $(patsubst %.c,%.o,$(wildcard $(OPUS_DIR)/celt/*.c))
OPUS_SILK_ALL = $(patsubst %.c,%.o,$(wildcard $(OPUS_DIR)/silk/*.c))
OPUS_SILK_ENC = $(OPUS_DIR)/silk/enc_API.o $(OPUS_DIR)/silk/encode_indices.o \
  $(OPUS_DIR)/silk/encode_pulses.o $(OPUS_DIR)/silk/control_SNR.o \
  $(OPUS_DIR)/silk/control_audio_bandwidth.o $(OPUS_DIR)/silk/control_codec.o \
  $(OPUS_DIR)/silk/check_control_input.o $(OPUS_DIR)/silk/HP_variable_cutoff.o \
  $(OPUS_DIR)/silk/stereo_encode_pred.o $(OPUS_DIR)/silk/stereo_find_predictor.o \
  $(OPUS_DIR)/silk/stereo_LR_to_MS.o $(OPUS_DIR)/silk/stereo_quant_pred.o \
  $(OPUS_DIR)/silk/NSQ.o $(OPUS_DIR)/silk/NSQ_del_dec.o $(OPUS_DIR)/silk/VAD.o \
  $(OPUS_DIR)/silk/NLSF_VQ.o $(OPUS_DIR)/silk/NLSF_VQ_weights_laroia.o \
  $(OPUS_DIR)/silk/NLSF_VQ_weights_laroia.o \
  $(OPUS_DIR)/silk/process_NLSFs.o
OPUS_SILK_OBJS = $(filter-out $(OPUS_SILK_ENC),$(OPUS_SILK_ALL))
OPUS_SRC_OBJS = $(patsubst %.c,%.o,$(wildcard $(OPUS_DIR)/src/*.c))
OPUS_OBJS = $(OPUS_CELT_OBJS) $(OPUS_SILK_OBJS) $(OPUS_SRC_OBJS)

TATERSURF_OBJS = $(LIBC_OBJ) $(BEARSSL_OBJS) \
  $(QUICKJS_OBJS) $(OH264_OBJS) $(OH264_CXXRT) $(OH264_WRAP) $(OH264_THREAD_STUBS) $(OPUS_OBJS) src/user/apps/tatersurf.o

USER_OBJS = \
  src/user/init/init.o \
  src/user/shell/shell.o \
  src/user/gui/gui.o \
  src/user/apps/sysinfo.o \
  src/user/apps/uptime.o \
  src/user/apps/ps.o \
  src/user/apps/fileman.o \
  src/user/apps/netmgr.o \
  src/user/apps/vmtest.o \
  src/user/apps/vmfault.o \
  src/user/apps/abitest.o \
  src/user/apps/thtest.o \
  src/user/apps/evloop.o

.PHONY: all clean kernel init shell gui sysinfo uptime ps fileman netmgr vmtest vmfault abitest thtest evloop tatersurf hosttools test-elf-bounds test-storage-fuzz
.SECONDARY:

all: kernel init shell gui sysinfo uptime ps fileman netmgr vmtest vmfault abitest thtest evloop tatersurf hosttools

test-elf-bounds: out/elf_loader_bounds_test
	./out/elf_loader_bounds_test

test-storage-fuzz: out/storage_meta_fuzz
	./out/storage_meta_fuzz

out/elf_loader_bounds_test: tools/elf_loader_bounds_test.c src/kernel/proc/elf.c src/kernel/proc/elf.h
	mkdir -p out
	$(HOSTCC) -O2 -std=gnu11 -Wall -Wextra -I. tools/elf_loader_bounds_test.c src/kernel/proc/elf.c -o out/elf_loader_bounds_test

out/storage_meta_fuzz: tools/storage_meta_fuzz.c src/kernel/fs/part.c src/kernel/fs/part.h src/kernel/fs/ntfs.c src/kernel/fs/ntfs.h src/kernel/fs/vfs.h
	mkdir -p out
	$(HOSTCC) -O2 -std=gnu11 -Wall -Wextra -ffunction-sections -fdata-sections -I. \
	  tools/storage_meta_fuzz.c src/kernel/fs/part.c src/kernel/fs/ntfs.c \
	  -Wl,--gc-sections -o out/storage_meta_fuzz

hosttools:
	$(HOSTCC) -O2 -o tools/mktotfs tools/mktotfs.c
	$(HOSTCC) -O2 -o tools/totcopy tools/totcopy.c

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

src/user/libc/setjmp.o: src/user/libc/setjmp.S
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

netmgr: $(NETMGR_OBJS)
	$(LD) -T user_linker.ld -o netmgr.elf $(NETMGR_OBJS)
	./tools/frypack.py netmgr.elf netmgr.fry

vmtest: $(VMTEST_OBJS)
	$(LD) -T user_linker.ld -o vmtest.elf $(VMTEST_OBJS)
	./tools/frypack.py vmtest.elf vmtest.fry

vmfault: $(VMFAULT_OBJS)
	$(LD) -T user_linker.ld -o vmfault.elf $(VMFAULT_OBJS)
	./tools/frypack.py vmfault.elf vmfault.fry

abitest: $(ABITEST_OBJS)
	$(LD) -T user_linker.ld -o abitest.elf $(ABITEST_OBJS)
	./tools/frypack.py abitest.elf abitest.fry

thtest: $(THTEST_OBJS)
	$(LD) -T user_linker.ld -o thtest.elf $(THTEST_OBJS)
	./tools/frypack.py thtest.elf thtest.fry

evloop: $(EVLOOP_OBJS)
	$(LD) -T user_linker.ld -o evloop.elf $(EVLOOP_OBJS)
	./tools/frypack.py evloop.elf evloop.fry

# BearSSL library objects: compile with shim headers
BEARSSL_CFLAGS = $(UFLAGS) -Isrc/user/apps/bearssl/inc -Isrc/user/apps/bearssl/src -Isrc/user/apps/mbedtls/shim -Wno-unused-parameter

$(BEARSSL_SRC)/aead/%.o: $(BEARSSL_SRC)/aead/%.c
	$(CC) $(BEARSSL_CFLAGS) -c $< -o $@
$(BEARSSL_SRC)/codec/%.o: $(BEARSSL_SRC)/codec/%.c
	$(CC) $(BEARSSL_CFLAGS) -c $< -o $@
$(BEARSSL_SRC)/ec/%.o: $(BEARSSL_SRC)/ec/%.c
	$(CC) $(BEARSSL_CFLAGS) -c $< -o $@
$(BEARSSL_SRC)/hash/%.o: $(BEARSSL_SRC)/hash/%.c
	$(CC) $(BEARSSL_CFLAGS) -c $< -o $@
$(BEARSSL_SRC)/int/%.o: $(BEARSSL_SRC)/int/%.c
	$(CC) $(BEARSSL_CFLAGS) -c $< -o $@
$(BEARSSL_SRC)/kdf/%.o: $(BEARSSL_SRC)/kdf/%.c
	$(CC) $(BEARSSL_CFLAGS) -c $< -o $@
$(BEARSSL_SRC)/mac/%.o: $(BEARSSL_SRC)/mac/%.c
	$(CC) $(BEARSSL_CFLAGS) -c $< -o $@
$(BEARSSL_SRC)/rand/%.o: $(BEARSSL_SRC)/rand/%.c
	$(CC) $(BEARSSL_CFLAGS) -c $< -o $@
$(BEARSSL_SRC)/rsa/%.o: $(BEARSSL_SRC)/rsa/%.c
	$(CC) $(BEARSSL_CFLAGS) -c $< -o $@
$(BEARSSL_SRC)/ssl/%.o: $(BEARSSL_SRC)/ssl/%.c
	$(CC) $(BEARSSL_CFLAGS) -c $< -o $@
$(BEARSSL_SRC)/symcipher/%.o: $(BEARSSL_SRC)/symcipher/%.c
	$(CC) $(BEARSSL_CFLAGS) -c $< -o $@
$(BEARSSL_SRC)/x509/%.o: $(BEARSSL_SRC)/x509/%.c
	$(CC) $(BEARSSL_CFLAGS) -c $< -o $@
$(BEARSSL_SRC)/settings.o: $(BEARSSL_SRC)/settings.c
	$(CC) $(BEARSSL_CFLAGS) -c $< -o $@
src/user/apps/bearssl/tatertos_rt.o: src/user/apps/bearssl/tatertos_rt.c
	$(CC) $(UFLAGS) -c $< -o $@

# QuickJS: compile with shim headers and relaxed warnings
$(QUICKJS_DIR)/%.o: $(QUICKJS_DIR)/%.c
	$(CC) $(UFLAGS) -DCONFIG_VERSION=\"2024-01-13\" \
	  -Isrc/user/apps/mbedtls/shim -Isrc/user/apps/quickjs \
	  -Wno-sign-compare -Wno-unused-parameter -Wno-implicit-fallthrough \
	  -Wno-implicit-function-declaration -c $< -o $@

# OpenH264: compile C++ decoder with shim headers
OH264_INC = -I$(OH264_DIR)/shim -I$(OH264_DIR)/common/inc -I$(OH264_DIR)/decoder/core/inc \
  -I$(OH264_DIR)/decoder/plus/inc -I$(OH264_DIR)/api/wels

$(OH264_DIR)/common/src/%.o: $(OH264_DIR)/common/src/%.cpp
	$(CXX) $(UCXXFLAGS) $(OH264_INC) -Wno-unused-parameter -c $< -o $@

$(OH264_DIR)/decoder/core/src/%.o: $(OH264_DIR)/decoder/core/src/%.cpp
	$(CXX) $(UCXXFLAGS) $(OH264_INC) -Wno-unused-parameter -c $< -o $@

$(OH264_DIR)/decoder/plus/src/%.o: $(OH264_DIR)/decoder/plus/src/%.cpp
	$(CXX) $(UCXXFLAGS) $(OH264_INC) -Wno-unused-parameter -c $< -o $@

$(OH264_DIR)/cxxrt.o: $(OH264_DIR)/cxxrt.cpp
	$(CXX) $(UCXXFLAGS) -Isrc/include -c $< -o $@

$(OH264_DIR)/wels_thread_stubs.o: $(OH264_DIR)/wels_thread_stubs.cpp
	$(CXX) $(UCXXFLAGS) $(OH264_INC) -Wno-unused-parameter -c $< -o $@

src/user/apps/ts_h264_wrap.o: src/user/apps/ts_h264_wrap.cpp
	$(CXX) $(UCXXFLAGS) $(OH264_INC) -Isrc/include -Isrc/user/apps -c $< -o $@

# libopus: compile with config header
OPUS_INC = -DHAVE_CONFIG_H -I$(OPUS_DIR) -I$(OPUS_DIR)/include -I$(OPUS_DIR)/celt -I$(OPUS_DIR)/silk -Isrc/user/apps/mbedtls/shim
OPUS_WARN = -Wno-sign-compare -Wno-unused-parameter

$(OPUS_DIR)/celt/%.o: $(OPUS_DIR)/celt/%.c
	$(CC) $(UFLAGS) $(OPUS_INC) $(OPUS_WARN) -c $< -o $@

$(OPUS_DIR)/silk/%.o: $(OPUS_DIR)/silk/%.c
	$(CC) $(UFLAGS) $(OPUS_INC) -I$(OPUS_DIR)/silk/float $(OPUS_WARN) -c $< -o $@

$(OPUS_DIR)/silk/float/%.o: $(OPUS_DIR)/silk/float/%.c
	$(CC) $(UFLAGS) $(OPUS_INC) -I$(OPUS_DIR)/silk/float $(OPUS_WARN) -c $< -o $@

$(OPUS_DIR)/src/%.o: $(OPUS_DIR)/src/%.c
	$(CC) $(UFLAGS) $(OPUS_INC) $(OPUS_WARN) -c $< -o $@

src/user/apps/tatersurf.o: src/user/apps/tatersurf.c
	$(CC) $(UFLAGS) -Isrc/user/apps/mbedtls/shim -Isrc/user/apps/bearssl/inc -Isrc/user/apps/bearssl/src -Isrc/user/apps -Isrc/user -I$(OH264_DIR)/api/wels -I$(OPUS_DIR)/include -Isrc/user/apps/quickjs -Wno-implicit-function-declaration -c $< -o $@

tatersurf: $(TATERSURF_OBJS)
	$(LD) -T user_linker.ld -o tatersurf.elf $(TATERSURF_OBJS)
	./tools/frypack.py tatersurf.elf tatersurf.fry

clean:
	rm -f $(KERNEL_OBJS) $(LIBC_OBJ) \
	  src/user/init/init.o src/user/shell/shell.o src/user/gui/gui.o src/user/apps/sysinfo.o \
	  src/user/apps/uptime.o src/user/apps/ps.o src/user/apps/fileman.o src/user/apps/netmgr.o \
	  src/user/apps/vmtest.o src/user/apps/vmfault.o src/user/apps/abitest.o src/user/apps/thtest.o \
	  src/user/apps/evloop.o src/user/apps/tatersurf.o \
	  $(BEARSSL_OBJS) \
	  $(QUICKJS_OBJS) $(OH264_OBJS) $(OH264_CXXRT) $(OPUS_OBJS) \
	  kernel.elf init.elf init.fry shell.elf shell.tot shell.fry gui.elf gui.fry \
	  sysinfo.elf sysinfo.fry uptime.elf uptime.fry ps.elf ps.fry \
	  fileman.elf fileman.fry netmgr.elf netmgr.fry vmtest.elf vmtest.fry \
	  vmfault.elf vmfault.fry abitest.elf abitest.fry thtest.elf thtest.fry \
	  evloop.elf evloop.fry \
	  tatersurf.elf tatersurf.fry \
	  tools/mktotfs tools/totcopy
