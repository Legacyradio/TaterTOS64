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

if [ -d /opt/cross/bin ]; then
  export PATH="/opt/cross/bin:$PATH"
else
  echo "Missing /opt/cross/bin (cross toolchain)."
  exit 1
fi

# Ensure old ISO-tree artifacts (like stale SHELL.FRY) do not leak into new builds.
rm -rf "$ISO_DIR"
mkdir -p "$EFI_DIR"

# Build kernel and user apps
make -C "$ROOT_DIR" clean
make -C "$ROOT_DIR" kernel init shell gui sysinfo uptime ps fileman

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
  mcopy -o -i "$FS_IMG" "$ROOT_DIR/shell.tot" ::/SHELL.TOT || true
  mcopy -o -i "$FS_IMG" "$ROOT_DIR/gui.fry" ::/GUI.FRY || true
  mcopy -o -i "$FS_IMG" "$ROOT_DIR/sysinfo.fry" ::/SYSINFO.FRY || true
  mcopy -o -i "$FS_IMG" "$ROOT_DIR/uptime.fry" ::/UPTIME.FRY || true
  mcopy -o -i "$FS_IMG" "$ROOT_DIR/ps.fry" ::/PS.FRY || true
  mcopy -o -i "$FS_IMG" "$ROOT_DIR/fileman.fry" ::/FILEMAN.FRY || true
fi

# Build host tools for ToTFS
echo "Building ToTFS host tools..."
gcc -O2 -o "$ROOT_DIR/tools/mktotfs" "$ROOT_DIR/tools/mktotfs.c"
gcc -O2 -o "$ROOT_DIR/tools/totcopy" "$ROOT_DIR/tools/totcopy.c"

# Create/update NVMe image with ToTFS partition
NVME_SIZE=$((100 * 1024 * 1024))  # 100 MB
NVME_PART_SIZE=$((NVME_SIZE - NVME_PART_OFFSET))

if [ ! -f "$NVME_IMG" ]; then
  echo "Creating NVMe image: $NVME_IMG ($NVME_SIZE bytes)"
  dd if=/dev/zero of="$NVME_IMG" bs=1M count=100 >/dev/null 2>&1
fi

# Write GPT header with ToTFS partition type
# Use sgdisk if available, otherwise do manual GPT write
if command -v sgdisk >/dev/null 2>&1; then
  # Delete all existing partitions, create a new one
  sgdisk --zap-all "$NVME_IMG" >/dev/null 2>&1 || true
  # Create partition 1 at LBA 2048 through end, with custom GUID type
  sgdisk --new=1:2048:0 \
         --typecode=1:46544F54-0053-5441-4552-544F53363400 \
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
for fry in gui sysinfo uptime ps fileman; do
  FRY_UPPER=$(echo "$fry" | tr 'a-z' 'A-Z')
  if [ -f "$ROOT_DIR/${fry}.fry" ]; then
    "$ROOT_DIR/tools/totcopy" "$NVME_IMG" "$NVME_PART_OFFSET" \
      "$ROOT_DIR/${fry}.fry" "/${FRY_UPPER}.FRY"
  fi
done

# Verify
echo "NVMe ToTFS contents:"
"$ROOT_DIR/tools/totcopy" "$NVME_IMG" "$NVME_PART_OFFSET" --list /
echo "NVMe image updated: $NVME_IMG"

# Build UEFI-only BOOTX64.EFI (no GRUB)
if command -v x86_64-w64-mingw32-ld >/dev/null 2>&1 && command -v x86_64-w64-mingw32-objcopy >/dev/null 2>&1; then
  x86_64-w64-mingw32-gcc -ffreestanding -fshort-wchar -fno-stack-protector -mno-red-zone \
    -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-ident \
    -c "$ROOT_DIR/src/boot/efi_loader.c" -o "$OUT_DIR/efi_loader.o" -O2 -Wall -Wextra -std=gnu11
  mkdir -p "$EFI_DIR"
  x86_64-w64-mingw32-ld -mi386pep --subsystem 10 -e efi_main "$OUT_DIR/efi_loader.o" -o "$EFI_DIR/BOOTX64.EFI"
  x86_64-w64-mingw32-objcopy --remove-section .comment "$EFI_DIR/BOOTX64.EFI"
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
  # Keep userspace binaries in every primary-root directory the loader/gui
  # actually probe on live media.
  efi_app_dirs=("/" "/fry" "/FRY" "/EFI/fry" "/EFI/FRY" "/EFI/BOOT" "/EFI/BOOT/fry" "/EFI/BOOT/FRY")
  for dir in "${efi_app_dirs[@]}"; do
    if [ "$dir" != "/" ]; then
      mmd -i "$EFI_IMG" "::${dir}" 2>/dev/null || true
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
  done
else
  echo "Missing mtools (mmd/mcopy) for efiboot.img."
  exit 1
fi

# Place efiboot.img into ISO tree for El Torito reference
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
done

# No GRUB, pure UEFI BOOTX64.EFI

# Build UEFI-bootable ISO using xorriso (EFI El Torito)
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

echo "Verifying packaged live-app fallbacks..."
for path in \
  "::/INIT.FRY" \
  "::/system/INIT.FRY" \
  "::/system/GUI.FRY" \
  "::/apps/SHELL.TOT" \
  "::/SHELL.TOT" \
  "::/fry/SHELL.TOT" \
  "::/FRY/SHELL.TOT" \
  "::/EFI/fry/SHELL.TOT" \
  "::/EFI/FRY/SHELL.TOT" \
  "::/EFI/BOOT/SHELL.TOT" \
  "::/EFI/BOOT/fry/SHELL.TOT" \
  "::/EFI/BOOT/FRY/SHELL.TOT"; do
  mdir -i "$EFI_IMG" "$path" >/dev/null
done

for path in \
  /INIT.FRY \
  /system/INIT.FRY \
  /system/GUI.FRY \
  /apps/SHELL.TOT \
  /SHELL.TOT \
  /fry/SHELL.TOT \
  /FRY/SHELL.TOT \
  /EFI/fry/SHELL.TOT \
  /EFI/FRY/SHELL.TOT \
  /EFI/BOOT/SHELL.TOT \
  /EFI/BOOT/fry/SHELL.TOT \
  /EFI/BOOT/FRY/SHELL.TOT; do
  xorriso -indev "$OUT_DIR/tatertos64v3.iso" -ls "$path" >/dev/null 2>&1
done

# Optional: export ISO to Windows path if available
WIN_EXPORT="/mnt/c/Users/jjsako/Documents/tateriso"
if [ -d /mnt/c/Users/jjsako/Documents ]; then
  mkdir -p "$WIN_EXPORT"
  if cp "$OUT_DIR/tatertos64v3.iso" "$WIN_EXPORT/tatertos64v3.iso"; then
    echo "ISO exported: $WIN_EXPORT/tatertos64v3.iso"
  else
    echo "Warning: could not export ISO to $WIN_EXPORT"
  fi
fi
