#!/usr/bin/expect -f
# smoketest_runner.tcl — Use expect for proper QEMU monitor interaction
set timeout 120

set ROOT_DIR [file dirname [file dirname [info script]]]
set ISO "$ROOT_DIR/out/tatertos64v3.iso"
set OVMF_CODE "/usr/share/edk2/x64/OVMF_CODE.4m.fd"
set OVMF_VARS "$ROOT_DIR/OVMF_VARS_copy.fd"

# Prepare OVMF vars
catch { exec cp /usr/share/edk2/x64/OVMF_VARS.4m.fd $OVMF_VARS }

# Start QEMU with serial to file
set qemu_pid [spawn qemu-system-x86_64 \
  -m 4G -machine q35,accel=tcg -cpu max -smp 2 \
  -drive if=pflash,format=raw,readonly=on,file=$OVMF_CODE \
  -drive if=pflash,format=raw,file=$OVMF_VARS \
  -cdrom $ISO \
  -vga std \
  -serial file:$ROOT_DIR/logs/serial_smoketest.log \
  -display none \
  -monitor stdio]

# The monitor is the main I/O channel
# Wait for QEMU to boot, then interact
sleep 55

# Take boot screenshot
send "screendump $ROOT_DIR/logs/smoketest_boot.ppm\r"
sleep 2

# Type "smoketest" at the shell
send "sendkey s\r"
sleep 0.2
send "sendkey m\r"
sleep 0.2
send "sendkey o\r"
sleep 0.2
send "sendkey k\r"
sleep 0.2
send "sendkey e\r"
sleep 0.2
send "sendkey t\r"
sleep 0.2
send "sendkey e\r"
sleep 0.2
send "sendkey s\r"
sleep 0.2
send "sendkey t\r"
sleep 0.2
send "sendkey ret\r"

sleep 25

# Take result screenshot
send "screendump $ROOT_DIR/logs/smoketest_result.ppm\r"
sleep 3

# Quit
send "quit\r"
sleep 2

catch { exec rm -f $OVMF_VARS }
