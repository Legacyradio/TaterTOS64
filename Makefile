TOOLBIN = $(CURDIR)/tools/host/bin

CC = $(TOOLBIN)/x86_64-elf-gcc
AS = nasm
LD = $(TOOLBIN)/x86_64-elf-ld
HOSTCC ?= gcc
PKG_CONFIG ?= pkg-config
HOST_GXX ?= g++
HOST_GXX_VERSION ?= $(shell $(HOST_GXX) -dumpversion)
HOST_GXX_MACHINE ?= $(shell $(HOST_GXX) -dumpmachine)

CXX = $(TOOLBIN)/x86_64-elf-g++
CFLAGS = -ffreestanding -fno-stack-protector -mno-red-zone -mcmodel=kernel -O2 -std=gnu11 -Wall -Wextra -Isrc/include
UFLAGS = -ffreestanding -fno-stack-protector -O2 -std=gnu11 -Wall -Wextra -Isrc/include -Isrc/user/libc
UCXXFLAGS = -ffreestanding -fno-stack-protector -mno-red-zone -O2 -std=gnu++23 -fno-exceptions -frtti -Wall -Wextra -isystem src/include/c++ -Isrc/include -Isrc/user/libc
HARFBUZZ_CFLAGS := $(filter -I% -D%,$(shell $(PKG_CONFIG) --cflags harfbuzz 2>/dev/null))
SKIA_CFLAGS ?= -I/usr/include $(filter -I% -D%,$(shell $(PKG_CONFIG) --cflags skia 2>/dev/null))
SKIA_LIBS = /usr/lib/libskia.a \
            $(CURDIR)/external/lib/libpng.a \
            $(CURDIR)/external/lib/libwebpdemux.a \
            $(CURDIR)/external/lib/libwebpdecoder.a \
            $(CURDIR)/external/lib/libfreetype.a \
            $(CURDIR)/external/lib/libjpeg.a \
            $(CURDIR)/external/lib/libwebp.a \
            $(CURDIR)/external/lib/libz.a
HOST_STDCXX_CFLAGS ?= \
	-isystem /usr/include/c++/$(HOST_GXX_VERSION) \
	-isystem /usr/include/c++/$(HOST_GXX_VERSION)/$(HOST_GXX_MACHINE) \
	-isystem /usr/include/c++/$(HOST_GXX_VERSION)/backward

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

LIBC_OBJ = src/user/libc/libc.o src/user/libc/errno.o src/user/libc/gfx.o \
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
SMOKETEST_OBJS = $(LIBC_OBJ) src/user/apps/smoketest.o
EVLOOP_OBJS = $(LIBC_OBJ) src/user/apps/evloop.o
CHROME_PROBE_OBJS = $(LIBC_OBJ) src/user/apps/chrome_probe.o

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
QUICKJS_REGEX_OBJS = $(QUICKJS_DIR)/cutils.o $(QUICKJS_DIR)/libregexp.o $(QUICKJS_DIR)/libunicode.o

# LibTomMath objects for Ladybird LibCrypto BigInt.
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
  src/user/apps/smoketest.o \
  src/user/apps/evloop.o \
  src/user/apps/chrome_probe.o

.PHONY: all clean kernel init shell gui sysinfo uptime ps fileman netmgr vmtest vmfault abitest thtest smoketest evloop chrome_probe hosttools test-elf-bounds test-storage-fuzz
.SECONDARY:

all: kernel init shell gui sysinfo uptime ps fileman netmgr vmtest vmfault abitest thtest smoketest evloop chrome_probe hosttools


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

src/user/%.o: src/user/%.cpp
	$(CXX) $(UCXXFLAGS) -c $< -o $@

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

smoketest: $(SMOKETEST_OBJS)
	$(LD) -T user_linker.ld -o smoketest.elf $(SMOKETEST_OBJS)
	./tools/frypack.py smoketest.elf smoketest.fry

evloop: $(EVLOOP_OBJS)
	$(LD) -T user_linker.ld -o evloop.elf $(EVLOOP_OBJS)
	./tools/frypack.py evloop.elf evloop.fry

chrome_probe: $(CHROME_PROBE_OBJS)
	$(LD) -T user_linker.ld -o chrome_probe.elf $(CHROME_PROBE_OBJS)
	./tools/frypack.py chrome_probe.elf chrome_probe.fry

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
	  -Dalloca=__builtin_alloca \
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

clean:
	rm -f $(KERNEL_OBJS) $(LIBC_OBJ) \
	  src/user/init/init.o src/user/shell/shell.o src/user/gui/gui.o src/user/apps/sysinfo.o \
	  src/user/apps/uptime.o src/user/apps/ps.o src/user/apps/fileman.o src/user/apps/netmgr.o \
  src/user/apps/vmtest.o src/user/apps/vmfault.o src/user/apps/abitest.o src/user/apps/thtest.o src/user/apps/smoketest.o \
  src/user/apps/evloop.o \
  src/user/apps/chrome_probe.o \
	  $(BEARSSL_OBJS) \
	  $(QUICKJS_OBJS) $(OH264_OBJS) $(OH264_CXXRT) $(OPUS_OBJS) \
	  $(AK_OBJS) $(LADYBIRD_DIR)/AK.a \
	  kernel.elf init.elf init.fry shell.elf shell.tot shell.fry gui.elf gui.fry \
	  sysinfo.elf sysinfo.fry uptime.elf uptime.fry ps.elf ps.fry \
	  fileman.elf fileman.fry netmgr.elf netmgr.fry vmtest.elf vmtest.fry \
  vmfault.elf vmfault.fry abitest.elf abitest.fry thtest.elf thtest.fry smoketest.elf smoketest.fry \
  evloop.elf evloop.fry \
  chrome_probe.elf chrome_probe.fry \
	  tools/mktotfs tools/totcopy

# =====================================================================
# Ladybird port — Chunk 2: AK static library
# Origin logs: logs/fry816 (initial plan)
#              logs/fry819 (rediff)
#              logs/fry821 (file manifest)
#              logs/fry822 (CMake authoritative compile list)
#              logs/fry823 (sister-lib carve-out, U1+S1b confirmed)
#              logs/fry827 (AK source + Backtrace.h + Debug.h + S1b patch)
#              logs/fry828 (this rule)
# =====================================================================
LADYBIRD_DIR = src/user/apps/ladybird
LADYBIRD_3RD = src/user/apps/ladybird-3rd
AK_DIR       = $(LADYBIRD_DIR)/AK
AR           = /usr/bin/x86_64-elf-ar

AK_CPP_NAMES = \
    Assertions Base64 ByteString ByteStringImpl CircularBuffer \
    ConstrainedStream CountingStream Error FlyString Format \
    GenericLexer Hex JsonArray JsonObject JsonParser JsonValue \
    LexicalPath MemoryStream NumberFormat OptionParser Random \
    StackInfo Stream String StringBase StringBuilder \
    StringConversions StringUtils StringView Time Utf16FlyString \
    Utf16String Utf16StringData Utf16View Utf32View Utf8View \
    kmalloc

AK_OBJS = $(addsuffix .o,$(addprefix $(AK_DIR)/,$(AK_CPP_NAMES)))

$(AK_DIR)/%.o: $(AK_DIR)/%.cpp
	$(CXX) $(AK_CXXFLAGS) -c $< -o $@

.PHONY: ak
ak:
	@echo "AK library build removed (Ladybird port deleted)"
	@touch ak_libcore_smoke.elf ak_libcore_smoke.fry

ak_libcore_smoke: ak
	@true

event_smoke net_smoke gfx_smoke link_smoke web_smoke:
	@echo "$@ build removed (Ladybird port deleted)"
	@touch $@.elf $@.fry

$(LADYBIRD_DIR)/AK.a $(LADYBIRD_DIR)/LibCore.a $(LADYBIRD_DIR)/LibIDL.a $(LADYBIRD_DIR)/LibURL.a $(LADYBIRD_DIR)/LibRegex.a $(LADYBIRD_DIR)/LibGfx.a $(LADYBIRD_DIR)/LibFileSystem.a $(LADYBIRD_DIR)/LibCrypto.a $(LADYBIRD_DIR)/LibJS.a $(LADYBIRD_DIR)/LibGC.a $(LADYBIRD_DIR)/LibHTTP.a $(LADYBIRD_DIR)/LibIPC.a $(LADYBIRD_DIR)/LibWeb.a $(LADYBIRD_DIR)/LibTomMath.a:
	@echo "Ladybird library $@: skipped"
	@touch $@

ifeq ($(LADYBIRD_PORT_ENABLED),1)

# AK requires the cross-compiler's own libstdc++ headers
# (initializer_list, iterator, memory, new, utility). Therefore NO
# -nostdinc++ here, unlike the UCXXFLAGS used by OpenH264 / ts_h264_wrap.
# AK requires C++23 (concepts, deducing-this, <=>). gcc 15.2.0 supports it.
# AK_OS_TATERTOS triggers the S1b kmalloc patch (libc malloc instead of mimalloc).
LADY_POSIX_FLAGS =

AK_CXXFLAGS = -ffreestanding -fno-stack-protector -mno-red-zone \
              -O2 -std=gnu++23 \
              -ffunction-sections -fdata-sections \
              -fno-exceptions -frtti \
              -Wall -Wextra \
              -DAK_OS_TATERTOS=1 \
              -isystem src/include/c++ \
              -Isrc/include -Isrc/user/libc \
              -I$(LADYBIRD_DIR) \
              -I$(LADYBIRD_3RD)/fmt/include \
              -I$(LADYBIRD_3RD)/fast_float/include \
              -I$(LADYBIRD_3RD)/simdutf/include \
              -I$(LADYBIRD_3RD)/simdutf/src \
              -U__STDCPP_FLOAT64_T__  -U__STDCPP_FLOAT32_T__ \
              -U__STDCPP_FLOAT16_T__  -U__STDCPP_BFLOAT16_T__ \
              -DFMT_USE_LOCALE=0  -DFMT_USE_FCNTL=0

# =====================================================================
# Ladybird port — Chunk 2 part 2: LibCore static library
# Origin logs: fry822 (CMake authoritative), fry823 (U1: MimeData
# carved out due to LibURL transitive dep), fry838 (this rule).
# =====================================================================
LIBCORE_DIR  = $(LADYBIRD_DIR)/Libraries/LibCore
LIBCORE_CXXFLAGS = $(AK_CXXFLAGS) -I$(LADYBIRD_DIR)/Libraries $(LADY_POSIX_FLAGS)

LIBCORE_CPP_NAMES = \
    AddressInfoVector  AnonymousBuffer  ArgsParser  ConfigFile  Directory \
    DirectoryEntry  DirIterator  ElapsedTimer  Environment  EventLoop \
    EventLoopImplementation  EventLoopImplementationTaterTOS  EventReceiver \
    TaterTOSInputNotifier \
    File  FileWatcherUnimplemented  LocalServer  MappedFile  Notifier \
    Process  ReportTime  Resource  ResourceImplementation \
    ResourceImplementationFile  SharedVersion  SocketAddress  Socket \
    StandardPaths  System  SystemServerTakeover  TCPServer \
    ThreadEventQueue  Timer  TimeZoneWatcherUnimplemented  UDPServer \
    Version MimeData

LIBCORE_OBJS = $(addsuffix .o,$(addprefix $(LIBCORE_DIR)/,$(LIBCORE_CPP_NAMES))) \
               $(LIBCORE_DIR)/Platform/ProcessStatisticsUnimplemented.o

# Pattern rule for LibCore source files (top level + Platform/).
$(LIBCORE_DIR)/%.o: $(LIBCORE_DIR)/%.cpp
	$(CXX) $(LIBCORE_CXXFLAGS) -c $< -o $@

.PHONY: libcore
libcore: $(LADYBIRD_DIR)/LibCore.a

$(LADYBIRD_DIR)/LibCore.a: $(LIBCORE_OBJS)
	$(AR) rcs $@ $(LIBCORE_OBJS)

# =====================================================================
# Ladybird port — LibIDL static library
# =====================================================================
LIBIDL_DIR = $(LADYBIRD_DIR)/Libraries/LibIDL
LIBIDL_CXXFLAGS = $(AK_CXXFLAGS) -frtti -I$(LADYBIRD_DIR)/Libraries $(LADY_POSIX_FLAGS)

LIBIDL_CPP_NAMES = ExposedTo IDLParser Types

LIBIDL_OBJS = $(addsuffix .o,$(addprefix $(LIBIDL_DIR)/,$(LIBIDL_CPP_NAMES)))

$(LIBIDL_DIR)/%.o: $(LIBIDL_DIR)/%.cpp
	$(CXX) $(LIBIDL_CXXFLAGS) -c $< -o $@

.PHONY: libidl
libidl: $(LADYBIRD_DIR)/LibIDL.a

$(LADYBIRD_DIR)/LibIDL.a: $(LIBIDL_OBJS)
	$(AR) rcs $@ $(LIBIDL_OBJS)

# =====================================================================
# Ladybird port — LibURL static library
# =====================================================================
LIBURL_DIR = $(LADYBIRD_DIR)/Libraries/LibURL
LIBURL_CXXFLAGS = $(AK_CXXFLAGS) -I$(LADYBIRD_DIR)/Libraries $(LADY_POSIX_FLAGS)

LIBURL_CPP_NAMES = Host Origin Parser Site URL

LIBURL_OBJS = $(addsuffix .o,$(addprefix $(LIBURL_DIR)/,$(LIBURL_CPP_NAMES)))

$(LIBURL_DIR)/%.o: $(LIBURL_DIR)/%.cpp
	$(CXX) $(LIBURL_CXXFLAGS) -c $< -o $@

.PHONY: liburl
liburl: $(LADYBIRD_DIR)/LibURL.a

$(LADYBIRD_DIR)/LibURL.a: $(LIBURL_OBJS)
	$(AR) rcs $@ $(LIBURL_OBJS)

# =====================================================================
# Ladybird port — LibRegex static library
# =====================================================================
LIBREGEX_DIR = $(LADYBIRD_DIR)/Libraries/LibRegex
LIBREGEX_CXXFLAGS = $(AK_CXXFLAGS) -frtti -I$(LADYBIRD_DIR)/Libraries -I$(QUICKJS_DIR) $(LADY_POSIX_FLAGS)

LIBREGEX_CPP_NAMES = ECMAScriptRegex

LIBREGEX_OBJS = $(addsuffix .o,$(addprefix $(LIBREGEX_DIR)/,$(LIBREGEX_CPP_NAMES)))

$(LIBREGEX_DIR)/%.o: $(LIBREGEX_DIR)/%.cpp
	$(CXX) $(LIBREGEX_CXXFLAGS) -c $< -o $@

.PHONY: libregex
libregex: $(LADYBIRD_DIR)/LibRegex.a

$(LADYBIRD_DIR)/LibRegex.a: $(LIBREGEX_OBJS)
	$(AR) rcs $@ $(LIBREGEX_OBJS)

# =====================================================================
# Ladybird port — LibTextCodec static library
# =====================================================================
LIBTEXTCODEC_DIR = $(LADYBIRD_DIR)/Libraries/LibTextCodec
LIBTEXTCODEC_CXXFLAGS = $(AK_CXXFLAGS) -I$(LADYBIRD_DIR)/Libraries $(LADY_POSIX_FLAGS)
LIBTEXTCODEC_CPP_NAMES = Decoder Encoder
LIBTEXTCODEC_OBJS = $(addsuffix .o,$(addprefix $(LIBTEXTCODEC_DIR)/,$(LIBTEXTCODEC_CPP_NAMES)))

$(LIBTEXTCODEC_DIR)/%.o: $(LIBTEXTCODEC_DIR)/%.cpp
	$(CXX) $(LIBTEXTCODEC_CXXFLAGS) -c $< -o $@

.PHONY: libtextcodec
libtextcodec: $(LADYBIRD_DIR)/LibTextCodec.a

$(LADYBIRD_DIR)/LibTextCodec.a: $(LIBTEXTCODEC_OBJS)
	$(AR) rcs $@ $(LIBTEXTCODEC_OBJS)

# =====================================================================
# Ladybird port — LibHTTP static library
# =====================================================================
LIBHTTP_DIR = $(LADYBIRD_DIR)/Libraries/LibHTTP
LIBHTTP_CXXFLAGS = $(AK_CXXFLAGS) -I$(LADYBIRD_DIR)/Libraries $(LADY_POSIX_FLAGS)
LIBHTTP_CPP_NAMES = HTTP Header HeaderList Method
LIBHTTP_COOKIE_NAMES = Cookie ParsedCookie
LIBHTTP_OBJS = $(addsuffix .o,$(addprefix $(LIBHTTP_DIR)/,$(LIBHTTP_CPP_NAMES))) \
               $(addsuffix .o,$(addprefix $(LIBHTTP_DIR)/Cookie/,$(LIBHTTP_COOKIE_NAMES)))

$(LIBHTTP_DIR)/%.o: $(LIBHTTP_DIR)/%.cpp
	$(CXX) $(LIBHTTP_CXXFLAGS) -c $< -o $@

.PHONY: libhttp
libhttp: $(LADYBIRD_DIR)/LibHTTP.a

$(LADYBIRD_DIR)/LibHTTP.a: $(LIBHTTP_OBJS)
	$(AR) rcs $@ $(LIBHTTP_OBJS)

# =====================================================================
# Ladybird port — LibIPC static library
# =====================================================================
LIBIPC_DIR = $(LADYBIRD_DIR)/Libraries/LibIPC
LIBIPC_CXXFLAGS = $(AK_CXXFLAGS) -I$(LADYBIRD_DIR)/Libraries $(LADY_POSIX_FLAGS)
LIBIPC_CPP_NAMES = Attachment File Message TransportHandle
LIBIPC_OBJS = $(addsuffix .o,$(addprefix $(LIBIPC_DIR)/,$(LIBIPC_CPP_NAMES)))

$(LIBIPC_DIR)/%.o: $(LIBIPC_DIR)/%.cpp
	$(CXX) $(LIBIPC_CXXFLAGS) -c $< -o $@

.PHONY: libipc
libipc: $(LADYBIRD_DIR)/LibIPC.a

$(LADYBIRD_DIR)/LibIPC.a: $(LIBIPC_OBJS)
	$(AR) rcs $@ $(LIBIPC_OBJS)

# =====================================================================
# Ladybird port — LibTomMath static library
# =====================================================================
LIBTOMMATH_CFLAGS = $(UFLAGS) -I$(LIBTOMMATH_DIR)

$(LIBTOMMATH_DIR)/%.o: $(LIBTOMMATH_DIR)/%.c
	$(CC) $(LIBTOMMATH_CFLAGS) -c $< -o $@

.PHONY: libtommath
libtommath: $(LADYBIRD_DIR)/LibTomMath.a

$(LADYBIRD_DIR)/LibTomMath.a: $(LIBTOMMATH_OBJS)
	$(AR) rcs $@ $(LIBTOMMATH_OBJS)

# =====================================================================
# Ladybird port — LibCrypto static library
# =====================================================================
LIBCRYPTO_DIR = $(LADYBIRD_DIR)/Libraries/LibCrypto
LIBCRYPTO_CXXFLAGS = $(AK_CXXFLAGS) -frtti -I$(LADYBIRD_DIR)/Libraries -I$(LIBTOMMATH_DIR) $(LADY_POSIX_FLAGS)

LIBCRYPTO_BIGINT_NAMES = SignedBigInteger UnsignedBigInteger
LIBCRYPTO_OBJS = $(addsuffix .o,$(addprefix $(LIBCRYPTO_DIR)/BigInt/,$(LIBCRYPTO_BIGINT_NAMES)))

$(LIBCRYPTO_DIR)/BigInt/%.o: $(LIBCRYPTO_DIR)/BigInt/%.cpp
	$(CXX) $(LIBCRYPTO_CXXFLAGS) -c $< -o $@

.PHONY: libcrypto
libcrypto: $(LADYBIRD_DIR)/LibCrypto.a

$(LADYBIRD_DIR)/LibCrypto.a: $(LIBCRYPTO_OBJS)
	$(AR) rcs $@ $(LIBCRYPTO_OBJS)

# =====================================================================
# Ladybird port — LibJS static library
# =====================================================================
LIBJS_DIR = $(LADYBIRD_DIR)/Libraries/LibJS
SIMDJSON_DIR = $(LADYBIRD_DIR)-3rd/simdjson
LIBJS_CXXFLAGS = $(AK_CXXFLAGS) -frtti -I$(LADYBIRD_DIR)/Libraries -I$(SIMDJSON_DIR) -I$(QUICKJS_DIR) $(LADY_POSIX_FLAGS)

LIBJS_CPP_BASE = Console CyclicModule Module ParserError Print RustIntegration Script SourceCode SourceTextModule SyntheticModule
LIBJS_CPP_RUNTIME = \
    AbstractOperations Accessor Agent Array \
    AggregateError AggregateErrorConstructor AggregateErrorPrototype \
    ArrayBuffer ArrayBufferConstructor ArrayBufferPrototype \
    ArrayConstructor ArrayIterator ArrayIteratorPrototype \
    ArgumentsObject \
    ArrayPrototype AsyncDisposableStack AsyncDisposableStackConstructor \
    AsyncDisposableStackPrototype AsyncFromSyncIterator \
    AsyncFromSyncIteratorPrototype AsyncFunctionConstructor \
    AsyncFunctionDriverWrapper AsyncFunctionPrototype AsyncGenerator \
    AsyncGeneratorFunctionConstructor AsyncGeneratorFunctionPrototype \
    AsyncGeneratorPrototype AsyncIteratorPrototype AtomicsObject BigInt \
    BigIntConstructor BigIntObject BigIntPrototype BooleanConstructor \
    BooleanObject BooleanPrototype BoundFunction ClassConstruction \
    ClassFieldDefinition Completion CompletionCell ConsoleObject \
    ConsoleObjectPrototype DataView DataViewConstructor DataViewPrototype \
    Date DateConstructor DatePrototype DeclarativeEnvironment DisposableStack \
    DisposableStackConstructor DisposableStackPrototype ECMAScriptFunctionObject \
    Environment Error ErrorConstructor ErrorData ErrorPrototype ErrorTypes \
    ExecutionContext FinalizationRegistry FinalizationRegistryConstructor \
    FinalizationRegistryPrototype FunctionConstructor FunctionEnvironment \
    FunctionObject FunctionPrototype GeneratorFunctionConstructor \
    GeneratorFunctionPrototype GeneratorObject GeneratorPrototype \
    GlobalEnvironment GlobalObject IndexedProperties InterpreterStack \
    Intrinsics Iterator IteratorConstructor IteratorHelper \
    IteratorHelperPrototype IteratorPrototype JobCallback JSONObject \
    KeyedCollections Map MapConstructor MapIterator MapIteratorPrototype \
    MapPrototype MathObject ModuleEnvironment ModuleNamespaceObject \
    NativeFunction NativeJavaScriptBackedFunction NumberConstructor \
    NumberObject NumberPrototype Object ObjectConstructor ObjectEnvironment \
    ObjectPrototype PrimitiveString PrivateEnvironment Promise PromiseCapability \
    PromiseConstructor PromiseJobs PromisePrototype PromiseReaction \
    PromiseResolvingElementFunctions PromiseResolvingFunction PropertyDescriptor \
    ProxyObject RawJSONObject Realm Reference ReflectObject RegExpConstructor \
    RegExpLegacyStaticProperties RegExpObject RegExpPrototype \
    RegExpStringIterator RegExpStringIteratorPrototype Set SetConstructor \
    SetIterator SetIteratorPrototype SetPrototype Shape \
    SharedArrayBufferConstructor SharedArrayBufferPrototype \
    SharedFunctionInstanceData StringConstructor StringIterator \
    StringIteratorPrototype StringObject StringPrototype SuppressedError \
    SuppressedErrorConstructor SuppressedErrorPrototype Symbol \
    SymbolConstructor SymbolObject SymbolPrototype TypedArray \
    TypedArrayConstructor TypedArrayPrototype Uint8Array Value VM \
    WeakMap WeakMapConstructor WeakMapPrototype WeakRef WeakRefConstructor \
    WeakRefPrototype WeakSet WeakSetConstructor WeakSetPrototype \
    WrapForValidIteratorPrototype

LIBJS_CPP_HEAP = Cell
LIBJS_CPP_BYTECODE = \
    BasicBlock Executable IdentifierTable Instruction Interpreter Label \
    Op PropertyKeyTable PropertyNameIterator RegexTable StringTable

LIBJS_ASM_INTERPRETER_OBJS = \
    $(LIBJS_DIR)/Bytecode/AsmInterpreter/AsmInterpreter.o \
    $(LIBJS_DIR)/Bytecode/AsmInterpreter/asmint_x86_64.o

LIBJS_OBJS = \
    $(addsuffix .o,$(addprefix $(LIBJS_DIR)/,$(LIBJS_CPP_BASE))) \
    $(addsuffix .o,$(addprefix $(LIBJS_DIR)/Runtime/,$(LIBJS_CPP_RUNTIME))) \
    $(addsuffix .o,$(addprefix $(LIBJS_DIR)/Heap/,$(LIBJS_CPP_HEAP))) \
    $(addsuffix .o,$(addprefix $(LIBJS_DIR)/Bytecode/,$(LIBJS_CPP_BYTECODE))) \
    $(LIBJS_ASM_INTERPRETER_OBJS) \
    $(SIMDJSON_DIR)/simdjson.o

$(LIBJS_DIR)/%.o: $(LIBJS_DIR)/%.cpp
	$(CXX) $(LIBJS_CXXFLAGS) -c $< -o $@

$(SIMDJSON_DIR)/%.o: $(SIMDJSON_DIR)/%.cpp
	$(CXX) $(LIBJS_CXXFLAGS) -c $< -o $@

$(LIBJS_DIR)/Runtime/%.o: $(LIBJS_DIR)/Runtime/%.cpp
	$(CXX) $(LIBJS_CXXFLAGS) -c $< -o $@

$(LIBJS_DIR)/Heap/%.o: $(LIBJS_DIR)/Heap/%.cpp
	$(CXX) $(LIBJS_CXXFLAGS) -c $< -o $@

$(LIBJS_DIR)/Bytecode/%.o: $(LIBJS_DIR)/Bytecode/%.cpp
	$(CXX) $(LIBJS_CXXFLAGS) -c $< -o $@

$(LIBJS_DIR)/Bytecode/AsmInterpreter/%.o: $(LIBJS_DIR)/Bytecode/AsmInterpreter/%.cpp
	$(CXX) $(LIBJS_CXXFLAGS) -c $< -o $@

$(LIBJS_DIR)/Bytecode/AsmInterpreter/%.o: $(LIBJS_DIR)/Bytecode/AsmInterpreter/%.S
	$(CC) $(UFLAGS) -c $< -o $@

.PHONY: libjs
libjs: $(LADYBIRD_DIR)/LibJS.a

$(LADYBIRD_DIR)/LibJS.a: $(LIBJS_OBJS)
	$(AR) rcs $@ $(LIBJS_OBJS)

# =====================================================================
# Ladybird port — LibFileSystem static library
# =====================================================================
LIBFILESYSTEM_DIR = $(LADYBIRD_DIR)/Libraries/LibFileSystem
LIBFILESYSTEM_CXXFLAGS = $(AK_CXXFLAGS) -I$(LADYBIRD_DIR)/Libraries $(LADY_POSIX_FLAGS)
LIBFILESYSTEM_OBJS = $(LIBFILESYSTEM_DIR)/FileSystem.o

$(LIBFILESYSTEM_DIR)/%.o: $(LIBFILESYSTEM_DIR)/%.cpp
	$(CXX) $(LIBFILESYSTEM_CXXFLAGS) -c $< -o $@

.PHONY: libfilesystem
libfilesystem: $(LADYBIRD_DIR)/LibFileSystem.a

$(LADYBIRD_DIR)/LibFileSystem.a: $(LIBFILESYSTEM_OBJS)
	$(AR) rcs $@ $(LIBFILESYSTEM_OBJS)

# =====================================================================
# Ladybird port — LibGfx static library
# =====================================================================
LIBGFX_DIR = $(LADYBIRD_DIR)/Libraries/LibGfx
LIBGFX_CXXFLAGS = $(AK_CXXFLAGS) -I$(LADYBIRD_DIR)/Libraries $(HARFBUZZ_CFLAGS) $(SKIA_CFLAGS) $(HOST_STDCXX_CFLAGS) $(LADY_POSIX_FLAGS)

LIBGFX_CPP_NAMES = \
    AffineTransform Bitmap Color ColorConversion ColorSpace Filter \
    ImmutableBitmap Painter PainterTaterTOS PaintingSurface Path PathSkia \
    Point Rect ShareableBitmap Size SkiaBackendContext SkiaUtils TextLayout \
    YUVData
LIBGFX_FONT_NAMES = Font FontDatabase Typeface TypefaceSkia

LIBGFX_OBJS = $(addsuffix .o,$(addprefix $(LIBGFX_DIR)/,$(LIBGFX_CPP_NAMES))) \
              $(addsuffix .o,$(addprefix $(LIBGFX_DIR)/Font/,$(LIBGFX_FONT_NAMES)))

$(LIBGFX_DIR)/%.o: $(LIBGFX_DIR)/%.cpp
	$(CXX) $(LIBGFX_CXXFLAGS) -c $< -o $@

.PHONY: libgfx
libgfx: $(LADYBIRD_DIR)/LibGfx.a

$(LADYBIRD_DIR)/LibGfx.a: $(LIBGFX_OBJS)
	$(AR) rcs $@ $(LIBGFX_OBJS)

# =====================================================================
# Ladybird port — Chunk 2 exit criterion: smoke test that links
# AK.a + LibCore.a + LibURL.a + libc objects, frypacks to .fry,
# verifies the chain from header parse → archive → ELF → fry container.
# Origin log: logs/fry841.txt
# =====================================================================
LADY_SMOKE = $(LADYBIRD_DIR)/test/ak_libcore_smoke

$(LADY_SMOKE).o: $(LADY_SMOKE).cpp
	$(CXX) $(LIBCORE_CXXFLAGS) -c $< -o $@

SIMDUTF_DIR = $(LADYBIRD_3RD)/simdutf
SIMDUTF_OBJ = $(SIMDUTF_DIR)/src/simdutf.o

$(SIMDUTF_OBJ): $(SIMDUTF_DIR)/src/simdutf.cpp
	$(CXX) $(AK_CXXFLAGS) -I$(SIMDUTF_DIR)/src -c $< -o $@

ak_libcore_smoke: $(LADY_SMOKE).o $(LADYBIRD_DIR)/AK.a $(LADYBIRD_DIR)/LibCore.a $(LADYBIRD_DIR)/LibURL.a $(SIMDUTF_OBJ) $(LIBC_OBJ) src/user/libc/cxx_runtime.o src/user/libc/libgcc_helpers.o src/user/libc/fmt_runtime.o
	$(LD) -T user_linker.ld -o ak_libcore_smoke.elf \
	    $(LADY_SMOKE).o \
	    $(LADYBIRD_DIR)/LibCore.a \
	    $(LADYBIRD_DIR)/LibURL.a \
	    $(LADYBIRD_DIR)/AK.a \
	    $(SIMDUTF_OBJ) \
	    src/user/libc/cxx_runtime.o \
	    src/user/libc/libgcc_helpers.o \
	    src/user/libc/fmt_runtime.o \
	    $(LIBC_OBJ) \
	    -L/usr/lib/gcc/x86_64-elf/15.2.0 -lgcc
	./tools/frypack.py ak_libcore_smoke.elf ak_libcore_smoke.fry

.PHONY: ak_libcore_smoke smoke
smoke: ak_libcore_smoke

# =====================================================================
# Ladybird port — LibGfx smoke test
# =====================================================================
LADY_GFX_SMOKE = src/user/apps/ladybird/test/gfx_smoke

$(LADY_GFX_SMOKE).o: $(LADY_GFX_SMOKE).cpp
	$(CXX) $(LIBGFX_CXXFLAGS) -c $< -o $@

gfx_smoke: $(LADY_GFX_SMOKE).o $(LADYBIRD_DIR)/LibGfx.a $(LADYBIRD_DIR)/AK.a $(SIMDUTF_OBJ) $(LIBC_OBJ) src/user/libc/cxx_runtime.o src/user/libc/libgcc_helpers.o
	$(LD) -T user_linker.ld -o gfx_smoke.elf \
	    $(LADY_GFX_SMOKE).o \
	    $(LADYBIRD_DIR)/LibGfx.a \
	    $(LADYBIRD_DIR)/AK.a \
	    $(SIMDUTF_OBJ) \
	    src/user/libc/cxx_runtime.o \
	    src/user/libc/libgcc_helpers.o \
	    $(LIBC_OBJ) \
	    $(SKIA_LIBS) \
	    -L/usr/lib -L/usr/lib/gcc/x86_64-elf/15.2.0 -lgcc
	./tools/frypack.py gfx_smoke.elf gfx_smoke.fry

# =====================================================================
# Ladybird port — LibCore event loop smoke test
# =====================================================================
LADY_EVENT_SMOKE = src/user/apps/ladybird/test/event_smoke

$(LADY_EVENT_SMOKE).o: $(LADY_EVENT_SMOKE).cpp
	$(CXX) $(LIBCORE_CXXFLAGS) -c $< -o $@

event_smoke: $(LADY_EVENT_SMOKE).o $(LADYBIRD_DIR)/LibCore.a $(LADYBIRD_DIR)/AK.a $(SIMDUTF_OBJ) $(LIBC_OBJ) src/user/libc/cxx_runtime.o src/user/libc/libgcc_helpers.o src/user/libc/fmt_runtime.o
	$(LD) -T user_linker.ld -o event_smoke.elf \
	    $(LADY_EVENT_SMOKE).o \
	    $(LADYBIRD_DIR)/LibCore.a \
	    $(LADYBIRD_DIR)/AK.a \
	    $(SIMDUTF_OBJ) \
	    src/user/libc/cxx_runtime.o \
	    src/user/libc/libgcc_helpers.o \
	    src/user/libc/fmt_runtime.o \
	    $(LIBC_OBJ) \
	    -L/usr/lib/gcc/x86_64-elf/15.2.0 -lgcc
	./tools/frypack.py event_smoke.elf event_smoke.fry

# =====================================================================
# Ladybird port — LibCore networking smoke test
# =====================================================================
LADY_NET_SMOKE = src/user/apps/ladybird/test/net_smoke

$(LADY_NET_SMOKE).o: $(LADY_NET_SMOKE).cpp
	$(CXX) $(LIBCORE_CXXFLAGS) -c $< -o $@

net_smoke: $(LADY_NET_SMOKE).o $(LADYBIRD_DIR)/LibCore.a $(LADYBIRD_DIR)/AK.a $(SIMDUTF_OBJ) $(LIBC_OBJ) src/user/libc/cxx_runtime.o src/user/libc/libgcc_helpers.o src/user/libc/fmt_runtime.o
	$(LD) -T user_linker.ld -o net_smoke.elf \
	    $(LADY_NET_SMOKE).o \
	    $(LADYBIRD_DIR)/LibCore.a \
	    $(LADYBIRD_DIR)/AK.a \
	    $(SIMDUTF_OBJ) \
	    src/user/libc/cxx_runtime.o \
	    src/user/libc/libgcc_helpers.o \
	    src/user/libc/fmt_runtime.o \
	    $(LIBC_OBJ) \
	    -L/usr/lib/gcc/x86_64-elf/15.2.0 -lgcc
	./tools/frypack.py net_smoke.elf net_smoke.fry

# =====================================================================
# Ladybird port — LibWeb + LibJS + LibGC Link Smoke Test
# =====================================================================
LADY_LINK_SMOKE = src/user/apps/ladybird/test/link_smoke

$(LADY_LINK_SMOKE).o: $(LADY_LINK_SMOKE).cpp
	$(CXX) $(LIBWEB_CXXFLAGS) -c $< -o $@

link_smoke: $(LADY_LINK_SMOKE).o $(LADYBIRD_DIR)/LibWeb.a $(LADYBIRD_DIR)/LibJS.a $(LADYBIRD_DIR)/LibGC.a $(LADYBIRD_DIR)/LibCrypto.a $(LADYBIRD_DIR)/LibTomMath.a $(LADYBIRD_DIR)/LibFileSystem.a $(LADYBIRD_DIR)/LibRegex.a $(LADYBIRD_DIR)/LibHTTP.a $(LADYBIRD_DIR)/LibIPC.a $(LADYBIRD_DIR)/LibURL.a $(LADYBIRD_DIR)/LibIDL.a $(LADYBIRD_DIR)/LibCore.a $(LADYBIRD_DIR)/LibGfx.a $(LADYBIRD_DIR)/AK.a $(SIMDUTF_OBJ) $(QUICKJS_REGEX_OBJS) $(LIBC_OBJ) src/user/libc/cxx_runtime.o src/user/libc/libgcc_helpers.o src/user/libc/fmt_runtime.o
	$(LD) --gc-sections -T user_linker.ld -o link_smoke.elf \
	    $(LADY_LINK_SMOKE).o \
	    --start-group \
	    $(LADYBIRD_DIR)/LibWeb.a \
	    $(LADYBIRD_DIR)/LibJS.a \
	    $(LADYBIRD_DIR)/LibGC.a \
	    $(LADYBIRD_DIR)/LibCrypto.a \
	    $(LADYBIRD_DIR)/LibTomMath.a \
	    $(LADYBIRD_DIR)/LibFileSystem.a \
	    $(LADYBIRD_DIR)/LibRegex.a \
	    $(LADYBIRD_DIR)/LibHTTP.a \
	    $(LADYBIRD_DIR)/LibIPC.a \
	    $(LADYBIRD_DIR)/LibURL.a \
	    $(LADYBIRD_DIR)/LibIDL.a \
	    $(LADYBIRD_DIR)/LibCore.a \
	    $(LADYBIRD_DIR)/LibGfx.a \
	    $(LADYBIRD_DIR)/AK.a \
	    --end-group \
	    $(SIMDUTF_OBJ) \
	    $(QUICKJS_REGEX_OBJS) \
	    src/user/libc/cxx_runtime.o \
	    src/user/libc/libgcc_helpers.o \
	    src/user/libc/fmt_runtime.o \
	    $(LIBC_OBJ) \
	    $(SKIA_LIBS) \
	    -L/usr/lib -L/usr/lib/gcc/x86_64-elf/15.2.0 -lgcc
	./tools/frypack.py link_smoke.elf link_smoke.fry

# =====================================================================
# Ladybird port — real LibWeb/LibJS/LibGC smoke test
# =====================================================================
LADY_WEB_SMOKE = src/user/apps/ladybird/test/web_smoke

$(LADY_WEB_SMOKE).o: $(LADY_WEB_SMOKE).cpp
	$(CXX) $(LIBWEB_CXXFLAGS) -c $< -o $@

LADY_WEB_SMOKE_STUBS = src/user/apps/ladybird/test/web_smoke_stubs

$(LADY_WEB_SMOKE_STUBS).o: $(LADY_WEB_SMOKE_STUBS).cpp
	$(CXX) $(LIBWEB_CXXFLAGS) -c $< -o $@

web_smoke: $(LADY_WEB_SMOKE).o $(LADY_WEB_SMOKE_STUBS).o $(LADYBIRD_DIR)/LibWeb.a $(LADYBIRD_DIR)/LibJS.a $(LADYBIRD_DIR)/LibGC.a $(LADYBIRD_DIR)/LibCrypto.a $(LADYBIRD_DIR)/LibTomMath.a $(LADYBIRD_DIR)/LibFileSystem.a $(LADYBIRD_DIR)/LibRegex.a $(LADYBIRD_DIR)/LibHTTP.a $(LADYBIRD_DIR)/LibIPC.a $(LADYBIRD_DIR)/LibURL.a $(LADYBIRD_DIR)/LibIDL.a $(LADYBIRD_DIR)/LibCore.a $(LADYBIRD_DIR)/LibGfx.a $(LADYBIRD_DIR)/AK.a $(SIMDUTF_OBJ) $(QUICKJS_REGEX_OBJS) $(LIBC_OBJ) src/user/libc/cxx_runtime.o src/user/libc/libgcc_helpers.o src/user/libc/fmt_runtime.o
	$(LD) --no-demangle --gc-sections -T user_linker.ld -o web_smoke.elf \
	    $(LADY_WEB_SMOKE).o \
	    $(LADY_WEB_SMOKE_STUBS).o \
	    --start-group \
	    $(LADYBIRD_DIR)/LibWeb.a \
	    $(LADYBIRD_DIR)/LibJS.a \
	    $(LADYBIRD_DIR)/LibGC.a \
	    $(LADYBIRD_DIR)/LibCrypto.a \
	    $(LADYBIRD_DIR)/LibTomMath.a \
	    $(LADYBIRD_DIR)/LibFileSystem.a \
	    $(LADYBIRD_DIR)/LibRegex.a \
	    $(LADYBIRD_DIR)/LibHTTP.a \
	    $(LADYBIRD_DIR)/LibIPC.a \
	    $(LADYBIRD_DIR)/LibURL.a \
	    $(LADYBIRD_DIR)/LibIDL.a \
	    $(LADYBIRD_DIR)/LibCore.a \
	    $(LADYBIRD_DIR)/LibGfx.a \
	    $(LADYBIRD_DIR)/AK.a \
	    --end-group \
	    $(SIMDUTF_OBJ) \
	    $(QUICKJS_REGEX_OBJS) \
	    src/user/libc/cxx_runtime.o \
	    src/user/libc/libgcc_helpers.o \
	    src/user/libc/fmt_runtime.o \
	    $(LIBC_OBJ) \
	    $(SKIA_LIBS) \
	    -L/usr/lib -L/usr/lib/gcc/x86_64-elf/15.2.0 -lgcc
	./tools/frypack.py web_smoke.elf web_smoke.fry

# =====================================================================
# Ladybird port — LibWeb static library (Phase 5)
# =====================================================================
LIBWEB_DIR = $(LADYBIRD_DIR)/Libraries/LibWeb
LIBWEB_CXXFLAGS = $(AK_CXXFLAGS) -frtti -I$(LADYBIRD_DIR)/Libraries $(LADY_POSIX_FLAGS)

LIBWEB_DOM_NAMES = \
    AbortController AbortSignal \
    AbstractElement AbstractRange Attr CharacterData Comment DOMImplementation \
    DOMTokenList Document DocumentFragment DocumentLoadEventDelayer \
    DocumentLoading DocumentObserver DocumentType EditingHostManager Element \
    ElementByIdMap ElementFactory Event EventDispatcher \
    EventTarget HTMLCollection LiveNodeList NamedNodeMap Node NodeList \
    NodeOperations ParentNode Position QualifiedName Range ShadowRoot Slot \
    SlotRegistry Slottable StaticNodeList StyleElementBase StyleInvalidator \
    Text Utils
LIBWEB_HTML_NAMES = \
    AttributeNames BarProp BrowsingContext BrowsingContextGroup CloseWatcher \
    CloseWatcherManager DOMStringMap DocumentState EventHandler EventNames \
    Focus FormAssociatedElement FormControlInfrastructure GlobalEventHandlers \
    HTMLAllCollection HTMLBodyElement HTMLDocument HTMLElement HTMLHeadElement \
    HTMLHtmlElement HTMLHyperlinkElementUtils HTMLIFrameElement HTMLInputElement \
    HTMLMetaElement HTMLOrSVGElement HTMLTitleElement History ListOfAvailableImages \
    Location MessageEvent MessagePort Navigable NavigableContainer Navigation \
    NavigationParams Navigator Numbers PolicyContainers SandboxingFlagSet \
    SerializedPolicyContainer SessionHistoryEntry SourceSnapshotParams Storage \
    StructuredSerialize TagNames Timer TraversableNavigable Window \
    WindowEventHandlers WindowOrWorkerGlobalScope WindowProxy
LIBWEB_HTML_EVENTLOOP_NAMES = EventLoop Task TaskQueue
LIBWEB_HTML_CUSTOM_ELEMENTS_NAMES = \
    CustomElementDefinition CustomElementName CustomElementReactionNames \
    CustomElementRegistry CustomStateSet
LIBWEB_HTML_SCRIPTING_NAMES = \
    Agent Environments SimilarOriginWindowAgent TemporaryExecutionContext \
    WindowEnvironmentSettingsObject
LIBWEB_HTML_PARSER_NAMES = \
    Entities HTMLEncodingDetection HTMLParser HTMLToken HTMLTokenizer \
    ListOfActiveFormattingElements NamedCharacterReferences StackOfOpenElements
LIBWEB_ARIA_NAMES = ARIAMixin AttributeNames
LIBWEB_CSS_NAMES = \
    Angle AnimationEvent BooleanExpression CascadedProperties Clip ComputedProperties \
    ContainerQuery CountersSet CounterStyle CounterStyleDefinition CSS \
    CSSAnimation CSSConditionRule CSSContainerRule CSSCounterStyleRule \
    CSSDescriptors CSSFontFaceDescriptors CSSFontFaceRule CSSFontFeatureValuesMap \
    CSSFontFeatureValuesRule CSSFunctionDeclarations CSSFunctionDescriptors \
    CSSFunctionRule CSSGroupingRule CSSImageValue CSSImportRule CSSKeyframeRule \
    CSSKeyframesRule CSSKeywordValue CSSLayerBlockRule CSSLayerStatementRule \
    CSSMarginRule CSSMathClamp CSSMathInvert CSSMathMax CSSMathMin CSSMathNegate \
    CSSMathProduct CSSMathSum CSSMathValue CSSMatrixComponent CSSMediaRule \
    CSSNamespaceRule CSSNestedDeclarations CSSNumericArray CSSNumericValue \
    CSSPageRule CSSPageDescriptors CSSPerspective CSSPropertyRule CSSRotate \
    CSSRule CSSRuleList CSSScale CSSSkew CSSSkewX CSSSkewY CSSStyleDeclaration \
    CSSStyleProperties CSSStyleRule CSSStyleSheet CSSStyleValue CSSSupportsRule \
    CSSTransformComponent CSSTransformValue CSSTransition CSSTranslate \
    CSSUnitValue CSSUnparsedValue CSSVariableReferenceValue ColorFunctionDescriptor \
    ColorInterpolation CustomPropertyData Descriptor Display EasingFunction \
    EdgeRect Fetch Filter Flex FontComputer FontFace FontLoading FontFaceSet \
    FontFaceSetLoadEvent FontFeatureData Frequency GridTrackPlacement \
    GridTrackSize Interpolation InvalidationSet Length LengthBox MediaList \
    MediaQuery MediaQueryList MediaQueryListEvent Number NumericType \
    PageSelector ParsedFontFace Percentage PreferredColorScheme \
    PreferredContrast PreferredMotion Ratio Resolution Screen ScreenOrientation \
    Selector SelectorEngine Serialize Size Sizing StyleComputer \
    StyleInvalidation StyleInvalidationData StyleProperty StylePropertyMapReadOnly \
    StylePropertyMap StyleScope StyleSheet StyleSheetIdentifier \
    StyleSheetInvalidation StyleSheetList Supports SystemColor Time \
    TransitionEvent URL ValueType VisualViewport
LIBWEB_CSS_PARSER_NAMES = \
    ArbitrarySubstitutionFunctions ComponentValue DescriptorParsing \
    ErrorReporter GradientParsing Helpers MediaParsing Parser PropertyParsing \
    RuleContext RuleParsing SelectorParsing Syntax SyntaxParsing Token \
    Tokenizer Types ValueParsing
LIBWEB_CSS_STYLEVALUES_NAMES = \
    AbstractImageStyleValue AnchorSizeStyleValue AnchorStyleValue \
    AngleStyleValue BackgroundSizeStyleValue BasicShapeStyleValue \
    BorderImageSliceStyleValue BorderRadiusRectStyleValue BorderRadiusStyleValue \
    CalculatedStyleValue ColorFunctionStyleValue ColorInterpolationMethodStyleValue \
    ColorSchemeStyleValue ColorStyleValue ConicGradientStyleValue ContentStyleValue \
    CounterDefinitionsStyleValue CounterStyleStyleValue CounterStyleSystemStyleValue \
    CounterStyleValue CursorStyleValue CustomIdentStyleValue DimensionStyleValue \
    DisplayStyleValue EasingStyleValue EdgeStyleValue FilterValueListStyleValue \
    FontSourceStyleValue FontStyleStyleValue FunctionStyleValue GridAutoFlowStyleValue \
    GridTemplateAreaStyleValue GridTrackPlacementStyleValue GridTrackSizeListStyleValue \
    ImageSetStyleValue ImageStyleValue IntegerStyleValue KeywordStyleValue \
    LengthStyleValue LinearGradientStyleValue NumberStyleValue OpacityValueStyleValue \
    OpenTypeTaggedStyleValue PositionStyleValue RadialGradientStyleValue \
    RadialSizeStyleValue RandomValueSharingStyleValue RatioStyleValue RectStyleValue \
    RepeatStyleStyleValue ScrollbarColorStyleValue ShadowStyleValue ShorthandStyleValue \
    StyleValue StyleValueList SuperellipseStyleValue TextIndentStyleValue \
    TextUnderlinePositionStyleValue TransformationStyleValue TreeCountingFunctionStyleValue \
    TupleStyleValue UnicodeRangeStyleValue UnresolvedStyleValue
LIBWEB_ANIMATIONS_NAMES = \
    Animatable Animation AnimationEffect AnimationPlaybackEvent AnimationTimeline \
    DocumentTimeline KeyframeEffect PseudoElementParsing ScrollTimeline TimeValue
LIBWEB_FETCH_NAMES = Body BodyInit Enums FetchMethod Headers Request Response
LIBWEB_INTERSECTIONOBSERVER_NAMES = IntersectionObserver IntersectionObserverEntry
LIBWEB_PLATFORM_NAMES = EventLoopPlugin Timer
LIBWEB_HRTIME_NAMES = Performance TimeOrigin
LIBWEB_VIEWTRANSITION_NAMES = ViewTransition
LIBWEB_EDITING_NAMES = CommandNames Commands
LIBWEB_ROOT_NAMES = Namespace
LIBWEB_PAINTING_NAMES = \
    AccumulatedVisualContext BackgroundPainting BorderPainting BorderRadiiData \
    BorderRadiusCornerClipper BordersData BoxModelMetrics DisplayList \
    DisplayListCommand DisplayListRecorder DisplayListRecordingContext \
    ExternalContentSource Paintable PaintableBox PaintableFragment \
    PaintableWithLines PaintStyle ResolvedCSSFilter ScrollFrame ScrollState \
    StackingContext ViewportPaintable
LIBWEB_INFRA_NAMES = Strings
LIBWEB_LAYOUT_NAMES = FormattingContext LayoutState SVGFormattingContext TreeBuilder
LIBWEB_PAGE_NAMES = Page
LIBWEB_WEBIDL_NAMES = AbstractOperations CallbackType DOMException Promise
LIBWEB_DOMURL_NAMES = DOMURL URLSearchParams
LIBWEB_SELECTION_NAMES = Selection
LIBWEB_BINDINGS_NAMES = \
    Intrinsics MainThreadVM PlatformObject Node EventTarget Document \
    Element DOMException DOMURL Location Window

LIBWEB_SVG_NAMES = AttributeNames AttributeParser Path SVGAElement SVGAnimatedEnumeration SVGAnimatedInteger SVGAnimatedLength SVGAnimatedLengthList SVGAnimatedNumber SVGAnimatedNumberList SVGAnimatedRect SVGAnimatedString SVGAnimatedTransformList SVGAnimationElement SVGCircleElement SVGClipPathElement SVGComponentTransferFunctionElement SVGDecodedImageData SVGDefsElement SVGDescElement SVGElement SVGEllipseElement SVGFEBlendElement SVGFEColorMatrixElement SVGFEComponentTransferElement SVGFECompositeElement SVGFEDisplacementMapElement SVGFEDropShadowElement SVGFEFloodElement SVGFEFuncAElement SVGFEFuncBElement SVGFEFuncGElement SVGFEFuncRElement SVGFEGaussianBlurElement SVGFEImageElement SVGFEMergeElement SVGFEMergeNodeElement SVGFEMorphologyElement SVGFEOffsetElement SVGFETurbulenceElement SVGFilterElement SVGFilterPrimitiveStandardAttributes SVGFitToViewBox SVGForeignObjectElement SVGGElement SVGGeometryElement SVGGradientElement SVGGraphicsElement SVGImageElement SVGLength SVGLengthList SVGLinearGradientElement SVGLineElement SVGList SVGMaskElement SVGMetadataElement SVGNumber SVGNumberList SVGPathElement SVGPatternElement SVGPolygonElement SVGPolylineElement SVGRadialGradientElement SVGRectElement SVGScriptElement SVGStopElement SVGStyleElement SVGSVGElement SVGSymbolElement SVGTextContentElement SVGTextElement SVGTextPathElement SVGTextPositioningElement SVGTitleElement SVGTransform SVGTransformList SVGTSpanElement SVGUseElement SVGViewElement TagNames

LIBWEB_OBJS = $(addsuffix .o,$(addprefix $(LIBWEB_DIR)/DOM/,$(LIBWEB_DOM_NAMES))) \
             $(addsuffix .o,$(addprefix $(LIBWEB_DIR)/HTML/,$(LIBWEB_HTML_NAMES))) \
             $(addsuffix .o,$(addprefix $(LIBWEB_DIR)/HTML/EventLoop/,$(LIBWEB_HTML_EVENTLOOP_NAMES))) \
             $(addsuffix .o,$(addprefix $(LIBWEB_DIR)/HTML/CustomElements/,$(LIBWEB_HTML_CUSTOM_ELEMENTS_NAMES))) \
             $(addsuffix .o,$(addprefix $(LIBWEB_DIR)/HTML/Scripting/,$(LIBWEB_HTML_SCRIPTING_NAMES))) \
             $(addsuffix .o,$(addprefix $(LIBWEB_DIR)/HTML/Parser/,$(LIBWEB_HTML_PARSER_NAMES))) \
             $(addsuffix .o,$(addprefix $(LIBWEB_DIR)/ARIA/,$(LIBWEB_ARIA_NAMES))) \
             $(addsuffix .o,$(addprefix $(LIBWEB_DIR)/CSS/,$(LIBWEB_CSS_NAMES))) \
             $(addsuffix .o,$(addprefix $(LIBWEB_DIR)/CSS/Parser/,$(LIBWEB_CSS_PARSER_NAMES))) \
             $(addsuffix .o,$(addprefix $(LIBWEB_DIR)/CSS/StyleValues/,$(LIBWEB_CSS_STYLEVALUES_NAMES))) \
             $(addsuffix .o,$(addprefix $(LIBWEB_DIR)/Animations/,$(LIBWEB_ANIMATIONS_NAMES))) \
             $(addsuffix .o,$(addprefix $(LIBWEB_DIR)/Fetch/,$(LIBWEB_FETCH_NAMES))) \
             $(addsuffix .o,$(addprefix $(LIBWEB_DIR)/IntersectionObserver/,$(LIBWEB_INTERSECTIONOBSERVER_NAMES))) \
             $(addsuffix .o,$(addprefix $(LIBWEB_DIR)/Platform/,$(LIBWEB_PLATFORM_NAMES))) \
             $(addsuffix .o,$(addprefix $(LIBWEB_DIR)/HighResolutionTime/,$(LIBWEB_HRTIME_NAMES))) \
             $(addsuffix .o,$(addprefix $(LIBWEB_DIR)/ViewTransition/,$(LIBWEB_VIEWTRANSITION_NAMES))) \
             $(addsuffix .o,$(addprefix $(LIBWEB_DIR)/Editing/,$(LIBWEB_EDITING_NAMES))) \
             $(addsuffix .o,$(addprefix $(LIBWEB_DIR)/,$(LIBWEB_ROOT_NAMES))) \
             $(addsuffix .o,$(addprefix $(LIBWEB_DIR)/Infra/,$(LIBWEB_INFRA_NAMES))) \
             $(addsuffix .o,$(addprefix $(LIBWEB_DIR)/Layout/,$(LIBWEB_LAYOUT_NAMES))) \
             $(addsuffix .o,$(addprefix $(LIBWEB_DIR)/Page/,$(LIBWEB_PAGE_NAMES))) \
             $(addsuffix .o,$(addprefix $(LIBWEB_DIR)/Painting/,$(LIBWEB_PAINTING_NAMES))) \
             $(addsuffix .o,$(addprefix $(LIBWEB_DIR)/WebIDL/,$(LIBWEB_WEBIDL_NAMES))) \
             $(addsuffix .o,$(addprefix $(LIBWEB_DIR)/DOMURL/,$(LIBWEB_DOMURL_NAMES))) \
             $(addsuffix .o,$(addprefix $(LIBWEB_DIR)/Selection/,$(LIBWEB_SELECTION_NAMES))) \
             $(addsuffix .o,$(addprefix $(LIBWEB_DIR)/SVG/,$(LIBWEB_SVG_NAMES))) \
             $(addsuffix .o,$(addprefix $(LIBWEB_DIR)/Bindings/,$(LIBWEB_BINDINGS_NAMES)))

$(LIBWEB_DIR)/%.o: $(LIBWEB_DIR)/%.cpp
	$(CXX) $(LIBWEB_CXXFLAGS) -c $< -o $@

$(LIBWEB_DIR)/SVG/%.o: $(LIBWEB_DIR)/SVG/%.cpp
	$(CXX) $(LIBWEB_CXXFLAGS) -c $< -o $@

$(LIBWEB_DIR)/DOM/%.o: $(LIBWEB_DIR)/DOM/%.cpp
	$(CXX) $(LIBWEB_CXXFLAGS) -c $< -o $@

$(LIBWEB_DIR)/HTML/%.o: $(LIBWEB_DIR)/HTML/%.cpp
	$(CXX) $(LIBWEB_CXXFLAGS) -c $< -o $@

$(LIBWEB_DIR)/HTML/EventLoop/%.o: $(LIBWEB_DIR)/HTML/EventLoop/%.cpp
	$(CXX) $(LIBWEB_CXXFLAGS) -c $< -o $@

$(LIBWEB_DIR)/HTML/CustomElements/%.o: $(LIBWEB_DIR)/HTML/CustomElements/%.cpp
	$(CXX) $(LIBWEB_CXXFLAGS) -c $< -o $@

$(LIBWEB_DIR)/HTML/Scripting/%.o: $(LIBWEB_DIR)/HTML/Scripting/%.cpp
	$(CXX) $(LIBWEB_CXXFLAGS) -c $< -o $@

$(LIBWEB_DIR)/HTML/Parser/%.o: $(LIBWEB_DIR)/HTML/Parser/%.cpp
	$(CXX) $(LIBWEB_CXXFLAGS) -c $< -o $@

$(LIBWEB_DIR)/ARIA/%.o: $(LIBWEB_DIR)/ARIA/%.cpp
	$(CXX) $(LIBWEB_CXXFLAGS) -c $< -o $@

$(LIBWEB_DIR)/CSS/%.o: $(LIBWEB_DIR)/CSS/%.cpp
	$(CXX) $(LIBWEB_CXXFLAGS) -c $< -o $@

$(LIBWEB_DIR)/CSS/Parser/%.o: $(LIBWEB_DIR)/CSS/Parser/%.cpp
	$(CXX) $(LIBWEB_CXXFLAGS) -c $< -o $@

$(LIBWEB_DIR)/CSS/StyleValues/%.o: $(LIBWEB_DIR)/CSS/StyleValues/%.cpp
	$(CXX) $(LIBWEB_CXXFLAGS) -c $< -o $@

$(LIBWEB_DIR)/Animations/%.o: $(LIBWEB_DIR)/Animations/%.cpp
	$(CXX) $(LIBWEB_CXXFLAGS) -c $< -o $@

$(LIBWEB_DIR)/Fetch/%.o: $(LIBWEB_DIR)/Fetch/%.cpp
	$(CXX) $(LIBWEB_CXXFLAGS) -c $< -o $@

$(LIBWEB_DIR)/IntersectionObserver/%.o: $(LIBWEB_DIR)/IntersectionObserver/%.cpp
	$(CXX) $(LIBWEB_CXXFLAGS) -c $< -o $@

$(LIBWEB_DIR)/Platform/%.o: $(LIBWEB_DIR)/Platform/%.cpp
	$(CXX) $(LIBWEB_CXXFLAGS) -c $< -o $@

$(LIBWEB_DIR)/HighResolutionTime/%.o: $(LIBWEB_DIR)/HighResolutionTime/%.cpp
	$(CXX) $(LIBWEB_CXXFLAGS) -c $< -o $@

$(LIBWEB_DIR)/ViewTransition/%.o: $(LIBWEB_DIR)/ViewTransition/%.cpp
	$(CXX) $(LIBWEB_CXXFLAGS) -c $< -o $@

$(LIBWEB_DIR)/Editing/%.o: $(LIBWEB_DIR)/Editing/%.cpp
	$(CXX) $(LIBWEB_CXXFLAGS) -c $< -o $@

$(LIBWEB_DIR)/Infra/%.o: $(LIBWEB_DIR)/Infra/%.cpp
	$(CXX) $(LIBWEB_CXXFLAGS) -c $< -o $@

$(LIBWEB_DIR)/Layout/%.o: $(LIBWEB_DIR)/Layout/%.cpp
	$(CXX) $(LIBWEB_CXXFLAGS) -c $< -o $@

$(LIBWEB_DIR)/Page/%.o: $(LIBWEB_DIR)/Page/%.cpp
	$(CXX) $(LIBWEB_CXXFLAGS) -c $< -o $@

$(LIBWEB_DIR)/Painting/%.o: $(LIBWEB_DIR)/Painting/%.cpp
	$(CXX) $(LIBWEB_CXXFLAGS) -c $< -o $@

$(LIBWEB_DIR)/WebIDL/%.o: $(LIBWEB_DIR)/WebIDL/%.cpp
	$(CXX) $(LIBWEB_CXXFLAGS) -c $< -o $@

$(LIBWEB_DIR)/DOMURL/%.o: $(LIBWEB_DIR)/DOMURL/%.cpp
	$(CXX) $(LIBWEB_CXXFLAGS) -c $< -o $@

$(LIBWEB_DIR)/Selection/%.o: $(LIBWEB_DIR)/Selection/%.cpp
	$(CXX) $(LIBWEB_CXXFLAGS) -c $< -o $@

$(LIBWEB_DIR)/Bindings/%.o: $(LIBWEB_DIR)/Bindings/%.cpp
	$(CXX) $(LIBWEB_CXXFLAGS) -c $< -o $@

$(info LIBWEB_OBJS=$(LIBWEB_OBJS))

.PHONY: libweb
libweb: $(LADYBIRD_DIR)/LibWeb.a

$(LADYBIRD_DIR)/LibWeb.a: $(LIBWEB_OBJS)
	$(AR) rcs $@ $(LIBWEB_OBJS)

# =====================================================================
# Ladybird port — LibGC static library (Phase 5)
# =====================================================================
LIBGC_DIR = $(LADYBIRD_DIR)/Libraries/LibGC
LIBGC_CXXFLAGS = $(AK_CXXFLAGS) -frtti -I$(LADYBIRD_DIR)/Libraries $(LADY_POSIX_FLAGS)

LIBGC_CPP_NAMES = \
    BlockAllocator Cell CellAllocator ConservativeVector Heap HeapBlock \
    Root RootHashMap RootVector WeakBlock WeakContainer

LIBGC_OBJS = $(addsuffix .o,$(addprefix $(LIBGC_DIR)/,$(LIBGC_CPP_NAMES)))

$(LIBGC_DIR)/%.o: $(LIBGC_DIR)/%.cpp
	$(CXX) $(LIBGC_CXXFLAGS) -c $< -o $@

.PHONY: libgc
libgc: $(LADYBIRD_DIR)/LibGC.a

$(LADYBIRD_DIR)/LibGC.a: $(LIBGC_OBJS)
	$(AR) rcs $@ $(LIBGC_OBJS)

endif

src/user/libc/cxx_runtime.o: src/user/libc/cxx_runtime.cpp
	$(CXX) $(UCXXFLAGS) -frtti -c $< -o $@
