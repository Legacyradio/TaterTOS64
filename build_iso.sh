#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "$0")" && pwd)
OUT_DIR="$ROOT_DIR/out"
ISO_DIR="$OUT_DIR/isodir"
EFI_DIR="$ISO_DIR/EFI/BOOT"
FS_IMG="$OUT_DIR/fs.img"
EFI_IMG="$OUT_DIR/efiboot.img"
NVME_IMG="$OUT_DIR/tater_nvme.img"
NVME_PART_OFFSET=$((2048 * 512))
WIFI_FW_CANON=iwlwifi-9260.ucode
WIFI_FW_SRC=""
AUTOTEST_VM="${TATERTOS_AUTOTEST_VM:-0}"
AUTOTEST_VMFAULT="${TATERTOS_AUTOTEST_VMFAULT:-0}"
VMTEST_RUN_MARKER="$OUT_DIR/VMTEST.RUN"
VMFAULT_RUN_MARKER="$OUT_DIR/VMFAULT.RUN"
TBRIDGE_RUN_MARKER="$OUT_DIR/LXTEST.RUN"
TBRIDGE_DATA_FILE="$OUT_DIR/LXTEST.TXT"
LINUX_DYN_LD_SRC="${LINUX_DYN_LD_SRC:-/lib64/ld-linux-x86-64.so.2}"
LINUX_DYN_LIB_DIR="${LINUX_DYN_LIB_DIR:-/usr/lib}"
LINUX_DYN_LIBS=(librt.so.1 libc.so.6 libpthread.so.0 libdl.so.2 libm.so.6 libgcc_s.so.1)

. "$ROOT_DIR/tools/host/env.sh"

for candidate in \
  "$ROOT_DIR/firmware/$WIFI_FW_CANON" \
  "$ROOT_DIR/firmware/iwlwifi-9260-th-b0-jf-b0-46.ucode"
do
  if [ -f "$candidate" ]; then
    WIFI_FW_SRC="$candidate"
    break
  fi
done

copy_wifi_fw_mtools() {
  local img="$1"
  if [ -z "${WIFI_FW_SRC:-}" ]; then
    return 0
  fi
  if command -v mmd >/dev/null 2>&1; then
    mmd -i "$img" ::/firmware 2>/dev/null || true
  fi
  mcopy -o -i "$img" "$WIFI_FW_SRC" "::/firmware/$WIFI_FW_CANON" || true
}

copy_wifi_fw_dir() {
  local dir="$1"
  if [ -z "${WIFI_FW_SRC:-}" ]; then
    return 0
  fi
  mkdir -p "$dir/firmware"
  cp "$WIFI_FW_SRC" "$dir/firmware/$WIFI_FW_CANON"
}

prepare_autotest_markers() {
  rm -f "$VMTEST_RUN_MARKER" "$VMFAULT_RUN_MARKER"
  if [ "$AUTOTEST_VM" = "1" ]; then
    printf "vmtest\n" > "$VMTEST_RUN_MARKER"
  fi
  if [ "$AUTOTEST_VMFAULT" = "1" ]; then
    printf "vmfault\n" > "$VMFAULT_RUN_MARKER"
  fi
}

copy_autotest_markers_mtools() {
  local img="$1"
  if [ -f "$VMTEST_RUN_MARKER" ]; then
    mcopy -o -i "$img" "$VMTEST_RUN_MARKER" ::/VMTEST.RUN || true
  fi
  if [ -f "$VMFAULT_RUN_MARKER" ]; then
    mcopy -o -i "$img" "$VMFAULT_RUN_MARKER" ::/VMFAULT.RUN || true
  fi
}

copy_autotest_markers_dir() {
  local dir="$1"
  if [ -f "$VMTEST_RUN_MARKER" ]; then
    cp "$VMTEST_RUN_MARKER" "$dir/VMTEST.RUN"
  fi
  if [ -f "$VMFAULT_RUN_MARKER" ]; then
    cp "$VMFAULT_RUN_MARKER" "$dir/VMFAULT.RUN"
  fi
}

copy_autotest_markers_totfs() {
  if [ -f "$VMTEST_RUN_MARKER" ]; then
    "$ROOT_DIR/tools/totcopy" "$NVME_IMG" "$NVME_PART_OFFSET" \
      "$VMTEST_RUN_MARKER" "/VMTEST.RUN"
  fi
  if [ -f "$VMFAULT_RUN_MARKER" ]; then
    "$ROOT_DIR/tools/totcopy" "$NVME_IMG" "$NVME_PART_OFFSET" \
      "$VMFAULT_RUN_MARKER" "/VMFAULT.RUN"
  fi
}

build_tbridge_test() {
  if [ -f "$ROOT_DIR/tools/host/lhello.S" ]; then
    gcc -nostdlib -static -no-pie -o "$ROOT_DIR/lhello.lxe" \
      "$ROOT_DIR/tools/host/lhello.S"
  fi
  if [ -f "$ROOT_DIR/tools/host/lfile.S" ]; then
    gcc -nostdlib -static -no-pie -o "$ROOT_DIR/lfile.lxe" \
      "$ROOT_DIR/tools/host/lfile.S"
  fi
  if [ -f "$ROOT_DIR/tools/host/lmmap.S" ]; then
    gcc -nostdlib -static -no-pie -o "$ROOT_DIR/lmmap.lxe" \
      "$ROOT_DIR/tools/host/lmmap.S"
  fi
  if [ -f "$ROOT_DIR/tools/host/ldyn.S" ]; then
    gcc -nostartfiles -no-pie \
      -Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2 \
      -Wl,-e,_start \
      -o "$ROOT_DIR/ldyn.lxe" "$ROOT_DIR/tools/host/ldyn.S" -lc
  fi
  if [ -f "$ROOT_DIR/tools/host/llibc.c" ]; then
    gcc -no-pie -o "$ROOT_DIR/llibc.lxe" "$ROOT_DIR/tools/host/llibc.c"
  fi
  if [ -f "$ROOT_DIR/tools/host/lthr.c" ]; then
    gcc -no-pie -pthread -o "$ROOT_DIR/lthr.lxe" "$ROOT_DIR/tools/host/lthr.c"
  fi
  if [ -f "$ROOT_DIR/tools/host/lgap.c" ]; then
    gcc -no-pie -o "$ROOT_DIR/lgap.lxe" "$ROOT_DIR/tools/host/lgap.c"
  fi
  if [ -f "$ROOT_DIR/tools/host/lasync.c" ]; then
    gcc -no-pie -o "$ROOT_DIR/lasync.lxe" "$ROOT_DIR/tools/host/lasync.c"
  fi
  if [ -f "$ROOT_DIR/tools/host/lfutex.c" ]; then
    gcc -no-pie -pthread -o "$ROOT_DIR/lfutex.lxe" "$ROOT_DIR/tools/host/lfutex.c"
  fi
}

prepare_tbridge_marker() {
  if [ -f "$ROOT_DIR/lhello.lxe" ] || [ -f "$ROOT_DIR/lfile.lxe" ] || [ -f "$ROOT_DIR/lmmap.lxe" ] || [ -f "$ROOT_DIR/ldyn.lxe" ] || [ -f "$ROOT_DIR/llibc.lxe" ] || [ -f "$ROOT_DIR/lthr.lxe" ] || [ -f "$ROOT_DIR/lgap.lxe" ] || [ -f "$ROOT_DIR/lasync.lxe" ] || [ -f "$ROOT_DIR/lfutex.lxe" ]; then
    printf "lxtest\n" > "$TBRIDGE_RUN_MARKER"
    printf "Tater Bridge VFS probe\n" > "$TBRIDGE_DATA_FILE"
  fi
}

copy_linux_runtime_mtools() {
  local img="$1"
  if [ ! -f "$LINUX_DYN_LD_SRC" ]; then
    return 0
  fi
  if command -v mmd >/dev/null 2>&1; then
    mmd -i "$img" ::/lib64 2>/dev/null || true
    mmd -i "$img" ::/usr 2>/dev/null || true
    mmd -i "$img" ::/usr/lib 2>/dev/null || true
  fi
  mcopy -o -i "$img" "$LINUX_DYN_LD_SRC" ::/lib64/ld-linux-x86-64.so.2 || true
  for lib in "${LINUX_DYN_LIBS[@]}"; do
    [ -f "$LINUX_DYN_LIB_DIR/$lib" ] && \
      mcopy -o -i "$img" "$LINUX_DYN_LIB_DIR/$lib" "::/usr/lib/$lib" || true
  done
}

copy_linux_runtime_dir() {
  local dir="$1"
  if [ ! -f "$LINUX_DYN_LD_SRC" ]; then
    return 0
  fi
  mkdir -p "$dir/lib64" "$dir/usr/lib"
  cp "$LINUX_DYN_LD_SRC" "$dir/lib64/ld-linux-x86-64.so.2"
  for lib in "${LINUX_DYN_LIBS[@]}"; do
    [ -f "$LINUX_DYN_LIB_DIR/$lib" ] && cp "$LINUX_DYN_LIB_DIR/$lib" "$dir/usr/lib/$lib"
  done
}

copy_tbridge_test_mtools() {
  local img="$1"
  if [ ! -f "$ROOT_DIR/lhello.lxe" ] && [ ! -f "$ROOT_DIR/lfile.lxe" ] && [ ! -f "$ROOT_DIR/lmmap.lxe" ] && [ ! -f "$ROOT_DIR/ldyn.lxe" ] && [ ! -f "$ROOT_DIR/llibc.lxe" ] && [ ! -f "$ROOT_DIR/lthr.lxe" ] && [ ! -f "$ROOT_DIR/lgap.lxe" ] && [ ! -f "$ROOT_DIR/lasync.lxe" ] && [ ! -f "$ROOT_DIR/lfutex.lxe" ]; then
    return 0
  fi
  prepare_tbridge_marker
  [ -f "$ROOT_DIR/ldyn.lxe" ] && copy_linux_runtime_mtools "$img"
  if command -v mmd >/dev/null 2>&1; then
    mmd -i "$img" ::/apps 2>/dev/null || true
  fi
  [ -f "$ROOT_DIR/lhello.lxe" ] && mcopy -o -i "$img" "$ROOT_DIR/lhello.lxe" ::/apps/LHELLO.LXE || true
  [ -f "$ROOT_DIR/lhello.lxe" ] && mcopy -o -i "$img" "$ROOT_DIR/lhello.lxe" ::/LHELLO.LXE || true
  [ -f "$ROOT_DIR/lfile.lxe" ] && mcopy -o -i "$img" "$ROOT_DIR/lfile.lxe" ::/apps/LFILE.LXE || true
  [ -f "$ROOT_DIR/lfile.lxe" ] && mcopy -o -i "$img" "$ROOT_DIR/lfile.lxe" ::/LFILE.LXE || true
  [ -f "$ROOT_DIR/lmmap.lxe" ] && mcopy -o -i "$img" "$ROOT_DIR/lmmap.lxe" ::/apps/LMMAP.LXE || true
  [ -f "$ROOT_DIR/lmmap.lxe" ] && mcopy -o -i "$img" "$ROOT_DIR/lmmap.lxe" ::/LMMAP.LXE || true
  [ -f "$ROOT_DIR/ldyn.lxe" ] && mcopy -o -i "$img" "$ROOT_DIR/ldyn.lxe" ::/apps/LDYN.LXE || true
  [ -f "$ROOT_DIR/ldyn.lxe" ] && mcopy -o -i "$img" "$ROOT_DIR/ldyn.lxe" ::/LDYN.LXE || true
  [ -f "$ROOT_DIR/llibc.lxe" ] && mcopy -o -i "$img" "$ROOT_DIR/llibc.lxe" ::/apps/LLIBC.LXE || true
  [ -f "$ROOT_DIR/llibc.lxe" ] && mcopy -o -i "$img" "$ROOT_DIR/llibc.lxe" ::/LLIBC.LXE || true
  [ -f "$ROOT_DIR/lthr.lxe" ] && mcopy -o -i "$img" "$ROOT_DIR/lthr.lxe" ::/apps/LTHR.LXE || true
  [ -f "$ROOT_DIR/lthr.lxe" ] && mcopy -o -i "$img" "$ROOT_DIR/lthr.lxe" ::/LTHR.LXE || true
  [ -f "$ROOT_DIR/lgap.lxe" ] && mcopy -o -i "$img" "$ROOT_DIR/lgap.lxe" ::/apps/LGAP.LXE || true
  [ -f "$ROOT_DIR/lgap.lxe" ] && mcopy -o -i "$img" "$ROOT_DIR/lgap.lxe" ::/LGAP.LXE || true
  [ -f "$ROOT_DIR/lasync.lxe" ] && mcopy -o -i "$img" "$ROOT_DIR/lasync.lxe" ::/apps/LASYNC.LXE || true
  [ -f "$ROOT_DIR/lasync.lxe" ] && mcopy -o -i "$img" "$ROOT_DIR/lasync.lxe" ::/LASYNC.LXE || true
  [ -f "$ROOT_DIR/lfutex.lxe" ] && mcopy -o -i "$img" "$ROOT_DIR/lfutex.lxe" ::/apps/LFUTEX.LXE || true
  [ -f "$ROOT_DIR/lfutex.lxe" ] && mcopy -o -i "$img" "$ROOT_DIR/lfutex.lxe" ::/LFUTEX.LXE || true
  mcopy -o -i "$img" "$TBRIDGE_RUN_MARKER" ::/LXTEST.RUN || true
  mcopy -o -i "$img" "$TBRIDGE_DATA_FILE" ::/LXTEST.TXT || true
  # REAL unmodified Linux coreutils. Must end in .LXE so the EFI ramdisk
  # scanner (ramdisk_should_load_name) includes them.
  [ -f /usr/bin/env ] && mcopy -o -i "$img" /usr/bin/env ::/apps/RENV.LXE || true
  [ -f /bin/cat ] && mcopy -o -i "$img" /bin/cat ::/apps/RCAT.LXE || true
  [ -f /bin/ls ] && mcopy -o -i "$img" /bin/ls ::/apps/RLS.LXE || true
  [ -f /usr/lib/libcap.so.2 ] && mcopy -o -i "$img" /usr/lib/libcap.so.2 ::/usr/lib/libcap.so.2 || true
}

copy_tbridge_test_dir() {
  local dir="$1"
  if [ ! -f "$ROOT_DIR/lhello.lxe" ] && [ ! -f "$ROOT_DIR/lfile.lxe" ] && [ ! -f "$ROOT_DIR/lmmap.lxe" ] && [ ! -f "$ROOT_DIR/ldyn.lxe" ] && [ ! -f "$ROOT_DIR/llibc.lxe" ] && [ ! -f "$ROOT_DIR/lthr.lxe" ] && [ ! -f "$ROOT_DIR/lgap.lxe" ] && [ ! -f "$ROOT_DIR/lasync.lxe" ] && [ ! -f "$ROOT_DIR/lfutex.lxe" ]; then
    return 0
  fi
  prepare_tbridge_marker
  [ -f "$ROOT_DIR/ldyn.lxe" ] && copy_linux_runtime_dir "$dir"
  mkdir -p "$dir/apps"
  [ -f "$ROOT_DIR/lhello.lxe" ] && cp "$ROOT_DIR/lhello.lxe" "$dir/apps/LHELLO.LXE"
  [ -f "$ROOT_DIR/lhello.lxe" ] && cp "$ROOT_DIR/lhello.lxe" "$dir/LHELLO.LXE"
  [ -f "$ROOT_DIR/lfile.lxe" ] && cp "$ROOT_DIR/lfile.lxe" "$dir/apps/LFILE.LXE"
  [ -f "$ROOT_DIR/lfile.lxe" ] && cp "$ROOT_DIR/lfile.lxe" "$dir/LFILE.LXE"
  [ -f "$ROOT_DIR/lmmap.lxe" ] && cp "$ROOT_DIR/lmmap.lxe" "$dir/apps/LMMAP.LXE"
  [ -f "$ROOT_DIR/lmmap.lxe" ] && cp "$ROOT_DIR/lmmap.lxe" "$dir/LMMAP.LXE"
  [ -f "$ROOT_DIR/ldyn.lxe" ] && cp "$ROOT_DIR/ldyn.lxe" "$dir/apps/LDYN.LXE"
  [ -f "$ROOT_DIR/ldyn.lxe" ] && cp "$ROOT_DIR/ldyn.lxe" "$dir/LDYN.LXE"
  [ -f "$ROOT_DIR/llibc.lxe" ] && cp "$ROOT_DIR/llibc.lxe" "$dir/apps/LLIBC.LXE"
  [ -f "$ROOT_DIR/llibc.lxe" ] && cp "$ROOT_DIR/llibc.lxe" "$dir/LLIBC.LXE"
  [ -f "$ROOT_DIR/lthr.lxe" ] && cp "$ROOT_DIR/lthr.lxe" "$dir/apps/LTHR.LXE"
  [ -f "$ROOT_DIR/lthr.lxe" ] && cp "$ROOT_DIR/lthr.lxe" "$dir/LTHR.LXE"
  [ -f "$ROOT_DIR/lgap.lxe" ] && cp "$ROOT_DIR/lgap.lxe" "$dir/apps/LGAP.LXE"
  [ -f "$ROOT_DIR/lgap.lxe" ] && cp "$ROOT_DIR/lgap.lxe" "$dir/LGAP.LXE"
  [ -f "$ROOT_DIR/lasync.lxe" ] && cp "$ROOT_DIR/lasync.lxe" "$dir/apps/LASYNC.LXE"
  [ -f "$ROOT_DIR/lasync.lxe" ] && cp "$ROOT_DIR/lasync.lxe" "$dir/LASYNC.LXE"
  [ -f "$ROOT_DIR/lfutex.lxe" ] && cp "$ROOT_DIR/lfutex.lxe" "$dir/apps/LFUTEX.LXE"
  [ -f "$ROOT_DIR/lfutex.lxe" ] && cp "$ROOT_DIR/lfutex.lxe" "$dir/LFUTEX.LXE"
  cp "$TBRIDGE_RUN_MARKER" "$dir/LXTEST.RUN"
  cp "$TBRIDGE_DATA_FILE" "$dir/LXTEST.TXT"
  # REAL unmodified Linux coreutils (ISO-dir tree = the EFI ramdisk source).
  # Must end in .LXE so ramdisk_should_load_name includes them.
  mkdir -p "$dir/apps" "$dir/usr/lib"
  [ -f /usr/bin/env ] && cp /usr/bin/env "$dir/apps/RENV.LXE"
  [ -f /bin/cat ] && cp /bin/cat "$dir/apps/RCAT.LXE"
  [ -f /bin/ls ] && cp /bin/ls "$dir/apps/RLS.LXE"
  [ -f /usr/lib/libcap.so.2 ] && cp /usr/lib/libcap.so.2 "$dir/usr/lib/libcap.so.2"
  # (The real 243MB claude binary is NOT staged in the ramdisk — it can't
  #  fit below 4GB. It goes on the NVMe ToTFS and is streamed from disk.)
}

copy_tbridge_test_totfs() {
  if [ ! -f "$ROOT_DIR/lhello.lxe" ] && [ ! -f "$ROOT_DIR/lfile.lxe" ] && [ ! -f "$ROOT_DIR/lmmap.lxe" ] && [ ! -f "$ROOT_DIR/ldyn.lxe" ] && [ ! -f "$ROOT_DIR/llibc.lxe" ] && [ ! -f "$ROOT_DIR/lthr.lxe" ] && [ ! -f "$ROOT_DIR/lgap.lxe" ] && [ ! -f "$ROOT_DIR/lasync.lxe" ] && [ ! -f "$ROOT_DIR/lfutex.lxe" ]; then
    return 0
  fi
  prepare_tbridge_marker
  if [ -f "$ROOT_DIR/lhello.lxe" ]; then
    "$ROOT_DIR/tools/totcopy" "$NVME_IMG" "$NVME_PART_OFFSET" \
      "$ROOT_DIR/lhello.lxe" "/LHELLO.LXE"
  fi
  if [ -f "$ROOT_DIR/lfile.lxe" ]; then
    "$ROOT_DIR/tools/totcopy" "$NVME_IMG" "$NVME_PART_OFFSET" \
      "$ROOT_DIR/lfile.lxe" "/LFILE.LXE"
  fi
  if [ -f "$ROOT_DIR/lmmap.lxe" ]; then
    "$ROOT_DIR/tools/totcopy" "$NVME_IMG" "$NVME_PART_OFFSET" \
      "$ROOT_DIR/lmmap.lxe" "/LMMAP.LXE"
  fi
  if [ -f "$ROOT_DIR/ldyn.lxe" ]; then
    "$ROOT_DIR/tools/totcopy" "$NVME_IMG" "$NVME_PART_OFFSET" \
      "$ROOT_DIR/ldyn.lxe" "/LDYN.LXE"
  fi
  if [ -f "$ROOT_DIR/llibc.lxe" ]; then
    "$ROOT_DIR/tools/totcopy" "$NVME_IMG" "$NVME_PART_OFFSET" \
      "$ROOT_DIR/llibc.lxe" "/LLIBC.LXE"
  fi
  if [ -f "$ROOT_DIR/lthr.lxe" ]; then
    "$ROOT_DIR/tools/totcopy" "$NVME_IMG" "$NVME_PART_OFFSET" \
      "$ROOT_DIR/lthr.lxe" "/LTHR.LXE"
  fi
  if [ -f "$ROOT_DIR/lgap.lxe" ]; then
    "$ROOT_DIR/tools/totcopy" "$NVME_IMG" "$NVME_PART_OFFSET" \
      "$ROOT_DIR/lgap.lxe" "/LGAP.LXE"
  fi
  if [ -f "$ROOT_DIR/lasync.lxe" ]; then
    "$ROOT_DIR/tools/totcopy" "$NVME_IMG" "$NVME_PART_OFFSET" \
      "$ROOT_DIR/lasync.lxe" "/LASYNC.LXE"
  fi
  if [ -f "$ROOT_DIR/lfutex.lxe" ]; then
    "$ROOT_DIR/tools/totcopy" "$NVME_IMG" "$NVME_PART_OFFSET" \
      "$ROOT_DIR/lfutex.lxe" "/LFUTEX.LXE"
  fi
  "$ROOT_DIR/tools/totcopy" "$NVME_IMG" "$NVME_PART_OFFSET" \
    "$TBRIDGE_RUN_MARKER" "/LXTEST.RUN"
  "$ROOT_DIR/tools/totcopy" "$NVME_IMG" "$NVME_PART_OFFSET" \
    "$TBRIDGE_DATA_FILE" "/LXTEST.TXT"
}

# Ensure old ISO-tree artifacts (like stale SHELL.FRY) do not leak into new builds.
rm -rf "$ISO_DIR"
mkdir -p "$EFI_DIR"
prepare_autotest_markers

# Build kernel and user apps
# make -C "$ROOT_DIR" clean
make -C "$ROOT_DIR" kernel init shell gui sysinfo uptime ps fileman netmgr vmtest vmfault abitest thtest smoketest evloop chrome_probe claude
build_tbridge_test
prepare_tbridge_marker

# Create FAT32 image for userspace files
rm -f "$FS_IMG"
# 16MB image
if command -v dd >/dev/null 2>&1; then
  dd if=/dev/zero of="$FS_IMG" bs=1M count=16 >/dev/null 2>&1
fi
if command -v mkfs.fat >/dev/null 2>&1; then
  mkfs.fat -F 32 "$FS_IMG" >/dev/null 2>&1
elif command -v mformat >/dev/null 2>&1; then
  mformat -i "$FS_IMG" -F :: >/dev/null 2>&1
else
  echo "Missing mkfs.fat or mformat (cannot format FAT32 fs.img)."
  exit 1
fi

# Copy shell.tot + app .fry files into fs.img if mtools is available
if command -v mcopy >/dev/null 2>&1; then
  if command -v mmd >/dev/null 2>&1; then
    mmd -i "$FS_IMG" ::/system 2>/dev/null || true
    mmd -i "$FS_IMG" ::/apps 2>/dev/null || true
  fi
  mcopy -o -i "$FS_IMG" "$ROOT_DIR/init.fry" ::/system/INIT.FRY || true
  mcopy -o -i "$FS_IMG" "$ROOT_DIR/init.fry" ::/INIT.FRY || true
  mcopy -o -i "$FS_IMG" "$ROOT_DIR/gui.fry" ::/system/GUI.FRY || true
  mcopy -o -i "$FS_IMG" "$ROOT_DIR/shell.tot" ::/apps/SHELL.TOT || true
  mcopy -o -i "$FS_IMG" "$ROOT_DIR/sysinfo.fry" ::/apps/SYSINFO.FRY || true
  mcopy -o -i "$FS_IMG" "$ROOT_DIR/uptime.fry" ::/apps/UPTIME.FRY || true
  mcopy -o -i "$FS_IMG" "$ROOT_DIR/ps.fry" ::/apps/PS.FRY || true
  mcopy -o -i "$FS_IMG" "$ROOT_DIR/fileman.fry" ::/apps/FILEMAN.FRY || true
  mcopy -o -i "$FS_IMG" "$ROOT_DIR/netmgr.fry" ::/apps/NETMGR.FRY || true
  mcopy -o -i "$FS_IMG" "$ROOT_DIR/vmtest.fry" ::/apps/VMTEST.FRY || true
  mcopy -o -i "$FS_IMG" "$ROOT_DIR/vmfault.fry" ::/apps/VMFAULT.FRY || true
  mcopy -o -i "$FS_IMG" "$ROOT_DIR/thtest.fry" ::/apps/THTEST.FRY || true
  mcopy -o -i "$FS_IMG" "$ROOT_DIR/smoketest.fry" ::/apps/SMOKETEST.FRY || true
  mcopy -o -i "$FS_IMG" "$ROOT_DIR/evloop.fry" ::/apps/EVLOOP.FRY || true
  mcopy -o -i "$FS_IMG" "$ROOT_DIR/chrome_probe.fry" ::/apps/CHROME_PROBE.FRY || true
  mcopy -o -i "$FS_IMG" "$ROOT_DIR/claude.fry" ::/apps/CLAUDE.FRY || true
  [ -f "$ROOT_DIR/DSKEY.TXT" ] && mcopy -o -i "$FS_IMG" "$ROOT_DIR/DSKEY.TXT" ::/apps/DSKEY.TXT || true
  copy_tbridge_test_mtools "$FS_IMG"
  mmd -i "$FS_IMG" ::/fonts 2>/dev/null || true
  mcopy -o -i "$FS_IMG" "$ROOT_DIR/DejaVuSans.ttf" ::/fonts/DEJAVU.TTF 2>/dev/null || true

  # Desktop icons
  mmd -i "$FS_IMG" ::/icons 2>/dev/null || true
  for icon in "$ROOT_DIR"/out/icons/*.ICON; do
    [ -f "$icon" ] && mcopy -o -i "$FS_IMG" "$icon" ::/icons/ || true
  done

  mcopy -o -i "$FS_IMG" "$ROOT_DIR/vmtest_fixture.txt" ::/apps/VMTEST.TXT || true
  mcopy -o -i "$FS_IMG" "$ROOT_DIR/shell.tot" ::/SHELL.TOT || true
  mcopy -o -i "$FS_IMG" "$ROOT_DIR/gui.fry" ::/GUI.FRY || true
  mcopy -o -i "$FS_IMG" "$ROOT_DIR/sysinfo.fry" ::/SYSINFO.FRY || true
  mcopy -o -i "$FS_IMG" "$ROOT_DIR/uptime.fry" ::/UPTIME.FRY || true
  mcopy -o -i "$FS_IMG" "$ROOT_DIR/ps.fry" ::/PS.FRY || true
  mcopy -o -i "$FS_IMG" "$ROOT_DIR/fileman.fry" ::/FILEMAN.FRY || true
  mcopy -o -i "$FS_IMG" "$ROOT_DIR/netmgr.fry" ::/NETMGR.FRY || true
  mcopy -o -i "$FS_IMG" "$ROOT_DIR/vmtest.fry" ::/VMTEST.FRY || true
  mcopy -o -i "$FS_IMG" "$ROOT_DIR/vmfault.fry" ::/VMFAULT.FRY || true
  mcopy -o -i "$FS_IMG" "$ROOT_DIR/thtest.fry" ::/THTEST.FRY || true
  mcopy -o -i "$FS_IMG" "$ROOT_DIR/smoketest.fry" ::/SMOKETEST.FRY || true
  mcopy -o -i "$FS_IMG" "$ROOT_DIR/evloop.fry" ::/EVLOOP.FRY || true
  mcopy -o -i "$FS_IMG" "$ROOT_DIR/DejaVuSans.ttf" ::/DEJAVU.TTF 2>/dev/null || true

  mcopy -o -i "$FS_IMG" "$ROOT_DIR/vmtest_fixture.txt" ::/VMTEST.TXT || true
  copy_wifi_fw_mtools "$FS_IMG"
  copy_autotest_markers_mtools "$FS_IMG"
fi

# Build host tools for ToTFS
echo "Building ToTFS host tools..."
gcc -O2 -o "$ROOT_DIR/tools/mktotfs" "$ROOT_DIR/tools/mktotfs.c"
gcc -O2 -o "$ROOT_DIR/tools/totcopy" "$ROOT_DIR/tools/totcopy.c"

# Create/update NVMe image with ToTFS partition
NVME_SIZE=$((400 * 1024 * 1024))  # 400 MB (holds large binaries e.g. claude)
NVME_PART_SIZE=$((NVME_SIZE - NVME_PART_OFFSET))

if [ ! -f "$NVME_IMG" ]; then
  echo "Creating NVMe image: $NVME_IMG ($NVME_SIZE bytes)"
  dd if=/dev/zero of="$NVME_IMG" bs=1M count=400 >/dev/null 2>&1
fi

# Write GPT header with ToTFS partition type
# Use sgdisk if available, otherwise do manual GPT write
if command -v sgdisk >/dev/null 2>&1; then
  # Delete all existing partitions, create a new one
  sgdisk --zap-all "$NVME_IMG" >/dev/null 2>&1 || true
  # Create partition 1 at LBA 2048 through end, with custom GUID type
  # Typecode must serialize (GPT mixed-endian) to the kernel's TOTFS_PART_GUID
  # byte array (src/kernel/fs/totfs.h): 46 54 4F 54 00 53 54 41 45 52 54 4F 53 36 34 00.
  # The first group is little-endian, so 544F5446 -> on-disk 46 54 4F 54 ("TOTF",
  # matching the superblock magic's on-disk form). The previous value
  # (46544F54-0053-5441-...) serialized to 54 4F 54 46 53 00 41 54 ... and failed
  # guid_eq(), so the ToTFS partition was never recognized and /nvme never mounted.
  sgdisk --new=1:2048:0 \
         --typecode=1:544F5446-5300-4154-4552-544F53363400 \
         --change-name=1:"TaterTOS" \
         "$NVME_IMG" >/dev/null 2>&1
  echo "GPT partition table created with ToTFS type GUID"
else
  echo "Warning: sgdisk not found, NVMe image may lack GPT header"
  echo "Install: sudo apt install gdisk"
fi

# Format partition as ToTFS
echo "Formatting ToTFS partition at offset $NVME_PART_OFFSET..."
"$ROOT_DIR/tools/mktotfs" "$NVME_IMG" "$NVME_PART_OFFSET" "$NVME_PART_SIZE"

# Copy shell.tot + app .fry files into ToTFS partition
  if [ -f "$ROOT_DIR/shell.tot" ]; then
  "$ROOT_DIR/tools/totcopy" "$NVME_IMG" "$NVME_PART_OFFSET" \
    "$ROOT_DIR/shell.tot" "/SHELL.TOT"
fi
if [ -f "$ROOT_DIR/init.fry" ]; then
  "$ROOT_DIR/tools/totcopy" "$NVME_IMG" "$NVME_PART_OFFSET" \
    "$ROOT_DIR/init.fry" "/INIT.FRY"
fi
for fry in gui sysinfo uptime ps fileman netmgr vmtest vmfault thtest; do
  FRY_UPPER=$(echo "$fry" | tr 'a-z' 'A-Z')
  if [ -f "$ROOT_DIR/${fry}.fry" ]; then
    "$ROOT_DIR/tools/totcopy" "$NVME_IMG" "$NVME_PART_OFFSET" \
      "$ROOT_DIR/${fry}.fry" "/${FRY_UPPER}.FRY"
  fi
done
if [ -f "$ROOT_DIR/vmtest_fixture.txt" ]; then
  "$ROOT_DIR/tools/totcopy" "$NVME_IMG" "$NVME_PART_OFFSET" \
    "$ROOT_DIR/vmtest_fixture.txt" "/VMTEST.TXT"
fi
copy_tbridge_test_totfs
copy_autotest_markers_totfs
# The REAL claude binary (ET_EXEC, ~243MB) onto the NVMe ToTFS, opt-in via
# STAGE_CLAUDE=1. Launched from /nvme/RCLAUDE.LXE and STREAMED from disk by
# the loader (never staged in the ramdisk). Live boot is unaffected.
if [ "$STAGE_CLAUDE" = "1" ] && [ -n "$CLAUDE_BIN" ] && [ -f "$CLAUDE_BIN" ]; then
  echo "Staging real claude binary onto NVMe ToTFS: $CLAUDE_BIN"
  "$ROOT_DIR/tools/totcopy" "$NVME_IMG" "$NVME_PART_OFFSET" \
    "$CLAUDE_BIN" "/RCLAUDE.LXE"
fi

# Verify
echo "NVMe ToTFS contents:"
"$ROOT_DIR/tools/totcopy" "$NVME_IMG" "$NVME_PART_OFFSET" --list /
echo "NVMe image updated: $NVME_IMG"

# Build UEFI-only BOOTX64.EFI (no GRUB)
echo "Building UEFI loader..."
if command -v x86_64-w64-mingw32-ld >/dev/null 2>&1 && command -v x86_64-w64-mingw32-objcopy >/dev/null 2>&1; then
  x86_64-w64-mingw32-gcc -ffreestanding -fshort-wchar -fno-stack-protector -mno-red-zone \
    -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-ident \
    -c "$ROOT_DIR/src/boot/efi_loader.c" -o "$OUT_DIR/efi_loader.o" -O2 -Wall -Wextra -std=gnu11
  mkdir -p "$EFI_DIR"
  x86_64-w64-mingw32-ld -mi386pep --subsystem 10 -e efi_main "$OUT_DIR/efi_loader.o" -o "$EFI_DIR/BOOTX64.EFI"
  x86_64-w64-mingw32-objcopy --remove-section .comment "$EFI_DIR/BOOTX64.EFI"
  echo "UEFI loader ready: $EFI_DIR/BOOTX64.EFI"
else
  echo "Missing mingw-w64 PE/COFF tools (x86_64-w64-mingw32-ld/objcopy)."
  echo "Install: sudo apt install -y binutils-mingw-w64-x86-64 gcc-mingw-w64-x86-64"
  exit 1
fi

# UEFI shell fallback: auto-run BOOTX64.EFI if Shell appears
cat > "$ISO_DIR/startup.nsh" <<'NSH'
fs0:
\EFI\BOOT\BOOTX64.EFI
NSH

# Build EFI System Partition image with BOOTX64.EFI
echo "Creating EFI system partition image..."
rm -f "$EFI_IMG"
if command -v dd >/dev/null 2>&1; then
  dd if=/dev/zero of="$EFI_IMG" bs=1M count=64 >/dev/null 2>&1
fi
if command -v mkfs.fat >/dev/null 2>&1; then
  mkfs.fat -F 32 "$EFI_IMG"
elif command -v mformat >/dev/null 2>&1; then
  mformat -i "$EFI_IMG" -F ::
else
  echo "Missing mkfs.fat or mformat (cannot format efiboot.img)."
  exit 1
fi
echo "Populating EFI system partition image..."
if command -v mmd >/dev/null 2>&1 && command -v mcopy >/dev/null 2>&1; then
  mmd -i "$EFI_IMG" ::/EFI
  mmd -i "$EFI_IMG" ::/EFI/BOOT
  mcopy -i "$EFI_IMG" "$EFI_DIR/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
  mmd -i "$EFI_IMG" ::/boot
  mcopy -i "$EFI_IMG" "$ROOT_DIR/kernel.elf" ::/boot/kernel.elf
  mcopy -i "$EFI_IMG" "$ISO_DIR/startup.nsh" ::/startup.nsh
  mmd -i "$EFI_IMG" ::/system 2>/dev/null || true
  mmd -i "$EFI_IMG" ::/apps 2>/dev/null || true
  mcopy -o -i "$EFI_IMG" "$ROOT_DIR/init.fry"    ::/system/INIT.FRY   || true
  mcopy -o -i "$EFI_IMG" "$ROOT_DIR/init.fry"    ::/INIT.FRY          || true
  mcopy -o -i "$EFI_IMG" "$ROOT_DIR/gui.fry"     ::/system/GUI.FRY    || true
  mcopy -o -i "$EFI_IMG" "$ROOT_DIR/shell.tot"   ::/apps/SHELL.TOT    || true
  mcopy -o -i "$EFI_IMG" "$ROOT_DIR/sysinfo.fry" ::/apps/SYSINFO.FRY  || true
  mcopy -o -i "$EFI_IMG" "$ROOT_DIR/uptime.fry"  ::/apps/UPTIME.FRY   || true
  mcopy -o -i "$EFI_IMG" "$ROOT_DIR/ps.fry"      ::/apps/PS.FRY       || true
  mcopy -o -i "$EFI_IMG" "$ROOT_DIR/fileman.fry" ::/apps/FILEMAN.FRY  || true
  mcopy -o -i "$EFI_IMG" "$ROOT_DIR/netmgr.fry"  ::/apps/NETMGR.FRY   || true
  mcopy -o -i "$EFI_IMG" "$ROOT_DIR/vmtest.fry"  ::/apps/VMTEST.FRY   || true
  mcopy -o -i "$EFI_IMG" "$ROOT_DIR/vmfault.fry" ::/apps/VMFAULT.FRY  || true
  mcopy -o -i "$EFI_IMG" "$ROOT_DIR/thtest.fry"  ::/apps/THTEST.FRY   || true
  mcopy -o -i "$EFI_IMG" "$ROOT_DIR/smoketest.fry" ::/apps/SMOKETEST.FRY || true
  mcopy -o -i "$EFI_IMG" "$ROOT_DIR/evloop.fry"  ::/apps/EVLOOP.FRY   || true
  mcopy -o -i "$EFI_IMG" "$ROOT_DIR/chrome_probe.fry" ::/apps/CHROME_PROBE.FRY || true
  mcopy -o -i "$EFI_IMG" "$ROOT_DIR/claude.fry" ::/apps/CLAUDE.FRY || true
  [ -f "$ROOT_DIR/DSKEY.TXT" ] && mcopy -o -i "$EFI_IMG" "$ROOT_DIR/DSKEY.TXT" ::/apps/DSKEY.TXT || true
  copy_tbridge_test_mtools "$EFI_IMG"

  mmd -i "$EFI_IMG" ::/fonts 2>/dev/null || true
  mcopy -o -i "$EFI_IMG" "$ROOT_DIR/DejaVuSans.ttf" ::/fonts/DEJAVU.TTF 2>/dev/null || true

  # Desktop icons
  mmd -i "$EFI_IMG" ::/icons 2>/dev/null || true
  for icon in "$ROOT_DIR"/out/icons/*.ICON; do
    [ -f "$icon" ] && mcopy -o -i "$EFI_IMG" "$icon" ::/icons/ || true
  done

  mcopy -o -i "$EFI_IMG" "$ROOT_DIR/vmtest_fixture.txt" ::/apps/VMTEST.TXT || true
  mcopy -o -i "$EFI_IMG" "$ROOT_DIR/vmtest_fixture.txt" ::/VMTEST.TXT || true
  # FAT is case-insensitive, so only populate one canonical spelling per
  # directory branch here. Writing both /fry and /FRY makes mtools prompt
  # interactively and the build appears to freeze in a TTY.
  efi_app_dirs=("/" "/FRY" "/EFI/FRY" "/EFI/BOOT" "/EFI/BOOT/FRY")
  for dir in "${efi_app_dirs[@]}"; do
    if [ "$dir" != "/" ]; then
      if ! mdir -i "$EFI_IMG" "::${dir}" >/dev/null 2>&1; then
        mmd -i "$EFI_IMG" "::${dir}" >/dev/null 2>&1 || true
      fi
      mcopy_dir="::${dir}"
    else
      mcopy_dir="::"
    fi
    mcopy -o -i "$EFI_IMG" "$ROOT_DIR/init.fry"    "$mcopy_dir/INIT.FRY"    || true
    mcopy -o -i "$EFI_IMG" "$ROOT_DIR/gui.fry"     "$mcopy_dir/GUI.FRY"     || true
    mcopy -o -i "$EFI_IMG" "$ROOT_DIR/shell.tot"   "$mcopy_dir/SHELL.TOT"   || true
    mcopy -o -i "$EFI_IMG" "$ROOT_DIR/sysinfo.fry" "$mcopy_dir/SYSINFO.FRY" || true
    mcopy -o -i "$EFI_IMG" "$ROOT_DIR/uptime.fry"  "$mcopy_dir/UPTIME.FRY"  || true
    mcopy -o -i "$EFI_IMG" "$ROOT_DIR/ps.fry"      "$mcopy_dir/PS.FRY"      || true
    mcopy -o -i "$EFI_IMG" "$ROOT_DIR/fileman.fry" "$mcopy_dir/FILEMAN.FRY" || true
    mcopy -o -i "$EFI_IMG" "$ROOT_DIR/netmgr.fry"  "$mcopy_dir/NETMGR.FRY"  || true
    mcopy -o -i "$EFI_IMG" "$ROOT_DIR/vmtest.fry"  "$mcopy_dir/VMTEST.FRY"  || true
    mcopy -o -i "$EFI_IMG" "$ROOT_DIR/vmfault.fry" "$mcopy_dir/VMFAULT.FRY" || true
    mcopy -o -i "$EFI_IMG" "$ROOT_DIR/thtest.fry"  "$mcopy_dir/THTEST.FRY"  || true
    mcopy -o -i "$EFI_IMG" "$ROOT_DIR/smoketest.fry" "$mcopy_dir/SMOKETEST.FRY" || true
    mcopy -o -i "$EFI_IMG" "$ROOT_DIR/evloop.fry"  "$mcopy_dir/EVLOOP.FRY"  || true
    mcopy -o -i "$EFI_IMG" "$ROOT_DIR/chrome_probe.fry" "$mcopy_dir/CHROME_PROBE.FRY" || true
    mcopy -o -i "$EFI_IMG" "$ROOT_DIR/claude.fry" "$mcopy_dir/CLAUDE.FRY" || true

  done
  copy_wifi_fw_mtools "$EFI_IMG"
  copy_autotest_markers_mtools "$EFI_IMG"
else
  echo "Missing mtools (mmd/mcopy) for efiboot.img."
  exit 1
fi
echo "EFI system partition image ready: $EFI_IMG"

# Place efiboot.img into ISO tree for El Torito reference
echo "Populating ISO staging tree..."
mkdir -p "$ISO_DIR/EFI"
cp "$EFI_IMG" "$ISO_DIR/EFI/efiboot.img"

# Copy kernel
mkdir -p "$ISO_DIR/boot"
cp "$ROOT_DIR/kernel.elf" "$ISO_DIR/boot/kernel.elf"
mkdir -p "$ISO_DIR/system" "$ISO_DIR/apps"
cp "$ROOT_DIR/init.fry"    "$ISO_DIR/system/INIT.FRY"
cp "$ROOT_DIR/init.fry"    "$ISO_DIR/INIT.FRY"
cp "$ROOT_DIR/gui.fry"     "$ISO_DIR/system/GUI.FRY"
cp "$ROOT_DIR/shell.tot"   "$ISO_DIR/apps/SHELL.TOT"
cp "$ROOT_DIR/sysinfo.fry" "$ISO_DIR/apps/SYSINFO.FRY"
cp "$ROOT_DIR/uptime.fry"  "$ISO_DIR/apps/UPTIME.FRY"
cp "$ROOT_DIR/ps.fry"      "$ISO_DIR/apps/PS.FRY"
cp "$ROOT_DIR/fileman.fry" "$ISO_DIR/apps/FILEMAN.FRY"
cp "$ROOT_DIR/netmgr.fry"  "$ISO_DIR/apps/NETMGR.FRY"
cp "$ROOT_DIR/vmtest.fry"  "$ISO_DIR/apps/VMTEST.FRY"
cp "$ROOT_DIR/vmfault.fry" "$ISO_DIR/apps/VMFAULT.FRY"
cp "$ROOT_DIR/thtest.fry"  "$ISO_DIR/apps/THTEST.FRY"
cp "$ROOT_DIR/smoketest.fry" "$ISO_DIR/apps/SMOKETEST.FRY"
cp "$ROOT_DIR/evloop.fry"  "$ISO_DIR/apps/EVLOOP.FRY"
cp "$ROOT_DIR/chrome_probe.fry" "$ISO_DIR/apps/CHROME_PROBE.FRY"
cp "$ROOT_DIR/claude.fry" "$ISO_DIR/apps/CLAUDE.FRY"
[ -f "$ROOT_DIR/DSKEY.TXT" ] && cp "$ROOT_DIR/DSKEY.TXT" "$ISO_DIR/apps/DSKEY.TXT"
copy_tbridge_test_dir "$ISO_DIR"

# Fonts
mkdir -p "$ISO_DIR/fonts"
cp "$ROOT_DIR/DejaVuSans.ttf" "$ISO_DIR/fonts/DEJAVU.TTF" 2>/dev/null || true

# Desktop icons
mkdir -p "$ISO_DIR/icons"
cp "$ROOT_DIR"/out/icons/*.ICON "$ISO_DIR/icons/" 2>/dev/null || true

cp "$ROOT_DIR/vmtest_fixture.txt" "$ISO_DIR/apps/VMTEST.TXT"
cp "$ROOT_DIR/vmtest_fixture.txt" "$ISO_DIR/VMTEST.TXT"
copy_wifi_fw_dir "$ISO_DIR"
copy_autotest_markers_dir "$ISO_DIR"

# Copy userspace payloads into every primary-root directory the live runtime
# probes. On Dell (and similar firmware) li->DeviceHandle can expose the ISO
# 9660 tree instead of efiboot.img, so this layout must match runtime fallbacks.
iso_app_dirs=("/" "/fry" "/FRY" "/EFI/fry" "/EFI/FRY" "/EFI/BOOT" "/EFI/BOOT/fry" "/EFI/BOOT/FRY")
for dir in "${iso_app_dirs[@]}"; do
  if [ "$dir" = "/" ]; then
    target_dir="$ISO_DIR"
  else
    target_dir="$ISO_DIR$dir"
    mkdir -p "$target_dir"
  fi
  cp "$ROOT_DIR/init.fry"    "$target_dir/INIT.FRY"
  cp "$ROOT_DIR/gui.fry"     "$target_dir/GUI.FRY"
  cp "$ROOT_DIR/shell.tot"   "$target_dir/SHELL.TOT"
  cp "$ROOT_DIR/sysinfo.fry" "$target_dir/SYSINFO.FRY"
  cp "$ROOT_DIR/uptime.fry"  "$target_dir/UPTIME.FRY"
  cp "$ROOT_DIR/ps.fry"      "$target_dir/PS.FRY"
  cp "$ROOT_DIR/fileman.fry" "$target_dir/FILEMAN.FRY"
  cp "$ROOT_DIR/netmgr.fry"  "$target_dir/NETMGR.FRY"
  cp "$ROOT_DIR/vmtest.fry"  "$target_dir/VMTEST.FRY"
  cp "$ROOT_DIR/vmfault.fry" "$target_dir/VMFAULT.FRY"
  cp "$ROOT_DIR/thtest.fry"  "$target_dir/THTEST.FRY"
  cp "$ROOT_DIR/smoketest.fry" "$target_dir/SMOKETEST.FRY"
  cp "$ROOT_DIR/evloop.fry"  "$target_dir/EVLOOP.FRY"
  cp "$ROOT_DIR/chrome_probe.fry" "$target_dir/CHROME_PROBE.FRY"
  cp "$ROOT_DIR/claude.fry" "$target_dir/CLAUDE.FRY"

  copy_wifi_fw_dir "$target_dir"
done
echo "ISO staging tree ready: $ISO_DIR"

# No GRUB, pure UEFI BOOTX64.EFI

# Build UEFI-bootable ISO using xorriso (EFI El Torito)
echo "Packaging final ISO..."
if command -v xorriso >/dev/null 2>&1; then
  xorriso -as mkisofs \
    -o "$OUT_DIR/tatertos64v3.iso" \
    --efi-boot EFI/efiboot.img \
    -no-emul-boot \
    -isohybrid-gpt-basdat \
    -append_partition 2 0xEF "$EFI_IMG" \
    -appended_part_as_gpt \
    "$ISO_DIR"
else
  echo "No ISO tool found (xorriso)."
  exit 1
fi

echo "ISO output: $OUT_DIR/tatertos64v3.iso"

echo "Verifying packaged live-app fallbacks in EFI image..."
for path in \
  "::/INIT.FRY" \
  "::/system/INIT.FRY" \
  "::/system/GUI.FRY" \
  "::/apps/SHELL.TOT" \
  "::/apps/NETMGR.FRY" \
  "::/apps/THTEST.FRY" \
  "::/SHELL.TOT" \
  "::/NETMGR.FRY" \
  "::/THTEST.FRY" \
  "::/fry/NETMGR.FRY" \
  "::/fry/THTEST.FRY" \
  "::/FRY/NETMGR.FRY" \
  "::/FRY/THTEST.FRY" \
  "::/EFI/fry/NETMGR.FRY" \
  "::/EFI/fry/THTEST.FRY" \
  "::/EFI/FRY/NETMGR.FRY" \
  "::/EFI/FRY/THTEST.FRY" \
  "::/EFI/BOOT/NETMGR.FRY" \
  "::/EFI/BOOT/THTEST.FRY" \
  "::/EFI/BOOT/fry/NETMGR.FRY" \
  "::/EFI/BOOT/fry/THTEST.FRY" \
  "::/EFI/BOOT/FRY/NETMGR.FRY" \
  "::/EFI/BOOT/FRY/THTEST.FRY" \
  "::/fry/SHELL.TOT" \
  "::/FRY/SHELL.TOT" \
  "::/EFI/fry/SHELL.TOT" \
  "::/EFI/FRY/SHELL.TOT" \
  "::/EFI/BOOT/SHELL.TOT" \
  "::/EFI/BOOT/fry/SHELL.TOT" \
  "::/EFI/BOOT/FRY/SHELL.TOT"; do
  mdir -i "$EFI_IMG" "$path" >/dev/null
done

echo "Verifying packaged live-app fallbacks in ISO..."
for path in \
  /INIT.FRY \
  /system/INIT.FRY \
  /system/GUI.FRY \
  /apps/SHELL.TOT \
  /apps/NETMGR.FRY \
  /apps/THTEST.FRY \
  /SHELL.TOT \
  /NETMGR.FRY \
  /THTEST.FRY \
  /fry/NETMGR.FRY \
  /fry/THTEST.FRY \
  /FRY/NETMGR.FRY \
  /FRY/THTEST.FRY \
  /EFI/fry/NETMGR.FRY \
  /EFI/fry/THTEST.FRY \
  /EFI/FRY/NETMGR.FRY \
  /EFI/FRY/THTEST.FRY \
  /EFI/BOOT/NETMGR.FRY \
  /EFI/BOOT/THTEST.FRY \
  /EFI/BOOT/fry/NETMGR.FRY \
  /EFI/BOOT/fry/THTEST.FRY \
  /EFI/BOOT/FRY/NETMGR.FRY \
  /EFI/BOOT/FRY/THTEST.FRY \
  /fry/SHELL.TOT \
  /FRY/SHELL.TOT \
  /EFI/fry/SHELL.TOT \
  /EFI/FRY/SHELL.TOT \
  /EFI/BOOT/SHELL.TOT \
  /EFI/BOOT/fry/SHELL.TOT \
  /EFI/BOOT/FRY/SHELL.TOT; do
  xorriso -indev "$OUT_DIR/tatertos64v3.iso" -ls "$path" >/dev/null 2>&1
done

echo "Live-app fallback verification complete."

# Optional: export ISO to Windows path if available
WIN_EXPORT_ROOT="/mnt/c/Users/jjsako/Documents"
WIN_EXPORT="/mnt/c/Users/jjsako/Documents/tateriso"
if [ "${SKIP_WIN_EXPORT:-0}" = "1" ]; then
  echo "Skipping Windows ISO export (SKIP_WIN_EXPORT=1)."
elif [ -d "$WIN_EXPORT_ROOT" ]; then
  echo "Exporting ISO to $WIN_EXPORT/tatertos64v3.iso..."
  if mkdir -p "$WIN_EXPORT" && \
     cp "$OUT_DIR/tatertos64v3.iso" "$WIN_EXPORT/tatertos64v3.iso"; then
    echo "ISO exported: $WIN_EXPORT/tatertos64v3.iso"
  else
    echo "Warning: could not export ISO to $WIN_EXPORT"
    echo "The directory exists, but this process could not write there."
  fi
fi
