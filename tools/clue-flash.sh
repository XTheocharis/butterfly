#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)

ENTER_DFU_ONLY=0
if [ "${1:-}" = "--dfu" ]; then
    ENTER_DFU_ONLY=1
    shift
fi

if [ "$#" -gt 1 ]; then
    echo "Usage: $0 [--dfu] [uf2-file]" >&2
    exit 2
fi

# Default path matches the Makefile `make dist PLATFORM=BOARD_CLUE` output.
UF2="${1:-$REPO_ROOT/dist/butterfly-clue-fwupgrade.uf2}"

if [ "$ENTER_DFU_ONLY" -eq 0 ] && [ ! -f "$UF2" ]; then
    echo "Usage: $0 [--dfu] [uf2-file]" >&2
    exit 1
fi

TTY=""
for candidate in /dev/ttyACM*; do
    if [ -e "$candidate" ]; then
        TTY=$candidate
        break
    fi
done

if [ -z "$TTY" ]; then
    echo "No device found"
    exit 1
fi

# Send "DFU" bytes to butterfly firmware over USB CDC. The firmware parses
# the bytes 'D','F','U' anywhere in the CDC RX buffer (see src/serial.cpp)
# and reboots into the Adafruit UF2 bootloader via GPREGRET=0x57.
#
# Must use pyserial rather than `printf > $TTY`: the firmware's CDC PORT_OPEN
# handler force-asserts DTR (workaround for kernels without TIOCMBIS) and
# only processes RX after DTR is asserted and termios is set to raw byte
# framing. A bare bash redirect fails to assert DTR in time and the firmware
# never sees the bytes.
send_dfu() {
    python3 - "$1" <<'PY'
import serial, sys, time
try:
    s = serial.Serial(sys.argv[1], baudrate=115200, timeout=0.2)
except Exception as e:
    print(f"DFU open failed: {e}", file=sys.stderr)
    sys.exit(1)
s.dtr = True
s.rts = False
time.sleep(0.15)
if s.in_waiting:
    s.read(s.in_waiting)
s.write(b"DFU")
s.flush()
time.sleep(0.4)
try:
    s.close()
except Exception:
    pass
PY
}

if [ "$ENTER_DFU_ONLY" -eq 1 ]; then
    echo "Requesting DFU on $TTY..."
    send_dfu "$TTY"
    echo "DFU requested."
    exit 0
fi

if command -v lsusb >/dev/null && lsusb | grep -q "c0ff:eeee"; then
    echo "Sending DFU to $TTY..."
    send_dfu "$TTY"
    sleep 3
fi

find_bootloader_device() {
    lsblk -p -r -o NAME,TYPE,RM |
        awk '$3 == 1 && ($2 == "part" || $2 == "disk") { print $1; exit }'
}

DEV=$(find_bootloader_device)
if [ -z "$DEV" ]; then
    echo "Waiting for bootloader..."
    for i in $(seq 1 10); do
        sleep 1
        DEV=$(find_bootloader_device)
        [ -n "$DEV" ] && break
    done
fi

if [ -z "$DEV" ]; then
    echo "Bootloader did not appear"
    exit 1
fi

echo "Mounting $DEV..."
MNT=""
udisksctl mount -b "$DEV" --no-user-interaction >/dev/null 2>&1 || true
MNT=$(lsblk -n -o MOUNTPOINT "$DEV" 2>/dev/null | head -1 || true)
SUDO=0
if [ -z "$MNT" ]; then
    MNT=$(mktemp -d)
    sudo mount "$DEV" "$MNT"
    SUDO=1
fi

echo "Copying $UF2..."
cp "$UF2" "$MNT/"
# The UF2 bootloader consumes the file and resets immediately. Post-write
# sync/umount may report I/O errors because the device has already dropped
# off the bus. The firmware has been flashed; the errors are expected noise.
sync || true

if [ "$SUDO" = "1" ]; then
    sudo umount "$MNT" 2>/dev/null || true
    rmdir "$MNT" 2>/dev/null || true
else
    udisksctl unmount -b "$DEV" --no-user-interaction 2>/dev/null || true
fi

echo "Flashed. Rebooting..."
