#!/bin/bash
# smoketest_runner.sh v7 — Write all commands to a temp file, read at once
set -e
cd "$(dirname "$0")/.."

ISO="out/tatertos64v3.iso"
OVMF_CODE="/usr/share/edk2/x64/OVMF_CODE.4m.fd"
OVMF_VARS="OVMF_VARS_copy.fd"
SERIAL_LOG="logs/serial_smoketest.log"
MON_CMDS="logs/smoketest_mon_cmds.txt"

rm -f "$OVMF_VARS" "$SERIAL_LOG"
cp /usr/share/edk2/x64/OVMF_VARS.4m.fd "$OVMF_VARS"

# Create the monitor command file
# QEMU's monitor reads commands sequentially. After each command it prints (qemu)
# The sleep steps are handled by QEMU's own timer
cat > "$MON_CMDS" << 'CMDS'
screendump logs/smoketest_boot.ppm
sendkey s
sendkey m
sendkey o
sendkey k
sendkey e
sendkey t
sendkey e
sendkey s
sendkey t
sendkey ret
screendump logs/smoketest_result.ppm
quit
CMDS

echo "Starting QEMU..."
# Pipe the commands into the monitor after waiting for boot
# We use a subshell that waits, then cats the commands
(
  sleep 55
  cat "$MON_CMDS"
) | qemu-system-x86_64 \
  -m 4G -machine q35,accel=tcg -cpu max -smp 2 \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file="$OVMF_VARS" \
  -cdrom "$ISO" \
  -vga std \
  -serial none \
  -display none \
  -monitor stdio 2>&1 | tail -5

QEMU_EXIT=$?

echo ""
echo "=== Screenshots ==="
ls -la logs/smoketest_boot.ppm logs/smoketest_result.ppm 2>/dev/null || echo "No screenshots"

# Check result
if [ -f logs/smoketest_result.ppm ]; then
  # Quick pixel check — look for any non-dark pixels (PASS/FAIL text)
  PASS_COUNT=$(xxd logs/smoketest_result.ppm | grep -c 'ffff\|f0f0\|e0e0' 2>/dev/null || echo 0)
  echo "Pixel brightness indicator: $PASS_COUNT"
fi

rm -f "$MON_CMDS" "$OVMF_VARS"
exit $QEMU_EXIT
