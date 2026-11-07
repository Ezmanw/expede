#!/bin/sh
# Boot the Expede test VM.
#   ./run.sh          headless (no window); sound is recorded to vm/audio.wav, not played
#   ./run.sh --show   open a window, with sound, so you can use the desktop yourself
cd "$(dirname "$0")"
display="-display none"
audio="-audiodev wav,id=snd0,path=audio.wav"
if [ "$1" = "--show" ]; then
	display="-display gtk,zoom-to-fit=on"
	audio="-audiodev pa,id=snd0"
fi
exec qemu-system-x86_64 \
	-enable-kvm -cpu host -smp 4 -m 4096 \
	-drive file=expede.qcow2,if=virtio \
	-drive file=seed.iso,media=cdrom,readonly=on \
	-device VGA,xres=1024,yres=768,edid=on \
	-device virtio-tablet-pci -device virtio-keyboard-pci \
	-nic user,model=virtio-net-pci,hostfwd=tcp:127.0.0.1:2222-:22 \
	-qmp unix:qmp.sock,server=on,wait=off \
	$audio -device intel-hda -device hda-output,audiodev=snd0 \
	$display
