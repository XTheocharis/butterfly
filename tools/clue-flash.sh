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

UF2="${1:-$REPO_ROOT/dist/butterfly-clue.uf2}"

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

if [ "$ENTER_DFU_ONLY" -eq 1 ]; then
    echo "Requesting DFU on $TTY..."
    printf 'DFU' > "$TTY"
    echo "DFU requested."
    exit 0
fi

if command -v lsusb >/dev/null && lsusb | grep -q "c0ff:eeee"; then
    echo "Sending DFU to $TTY..."
    printf 'DFU' > "$TTY"
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
MNT=$(udisksctl mount -b "$DEV" --no-user-interaction 2>/dev/null |
    sed -n 's/.* at \(\/run\/media\/.*\)\.$/\1/p' || true)
SUDO=0
if [ -z "$MNT" ]; then
    MNT=/mnt
    sudo mount "$DEV" "$MNT"
    SUDO=1
fi

echo "Copying $UF2..."
cp "$UF2" "$MNT/"
sync

if [ "$SUDO" = "1" ]; then
    sudo umount "$MNT"
else
    udisksctl unmount -b "$DEV" --no-user-interaction 2>/dev/null || true
fi

echo "Flashed. Rebooting..."
