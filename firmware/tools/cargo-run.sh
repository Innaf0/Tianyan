#!/bin/sh
# cargo runner: flash the firmware with picotool, then open a serial
# terminal (screen) on the USB CDC device to watch the logs.
set -e

# The ELF path is the last argument cargo passes to the runner.
for elf in "$@"; do :; done

echo "Flashing $elf with picotool..."
picotool load -u -v -x -t elf "$elf"

DEV=$(ls -t /dev/cu.usbmodem* 2>/dev/null | head -n 1)
if [ -z "$DEV" ]; then
    DEV=$(ls -t /dev/tty.usbmodem* 2>/dev/null | head -n 1)
fi

if [ -n "$DEV" ]; then
    echo "Opening serial terminal on $DEV (detach: Ctrl-A d, quit: Ctrl-A k)"
    exec env TERM=xterm screen "$DEV" 115200
else
    echo "Warning: no USB serial device found, skipping terminal."
fi
