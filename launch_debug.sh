DISPLAY=:0 qemu-system-x86_64 -m 4G -machine q35,accel=kvm:tcg -cpu host \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/x64/OVMF_CODE.4m.fd \
  -drive if=pflash,format=raw,file=./out/OVMF_VARS.fd \
  -drive file=./out/tater_nvme.img,if=none,id=nvm \
  -device nvme,drive=nvm,serial=deadbeef \
  -cdrom ./out/tatertos64v3.iso \
  -netdev user,id=n0 -device e1000,netdev=n0 \
  -serial stdio -vga std -display gtk
