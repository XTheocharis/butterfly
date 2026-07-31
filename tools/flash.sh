#!/bin/bash
set -e

UF2="${1:-dist/butterfly-clue.uf2}"

if [ ! -f "$UF2" ]; then
    echo "Usage: $0 [uf2-file]"
    exit 1
fi

TTY=$(ls /dev/ttyACM* 2>/dev/null | head -1)
if [ -z "$TTY" ]; then
    echo "No device found"
    exit 1
fi

if lsusb | grep -q "c0ff:eeee"; then
    echo "Sending DFU to $TTY..."
    echo -n "DFU" > "$TTY"
    sleep 3
fi

DEV=$(lsblk -o NAME -n -d | grep '^sd' | head -1)
if [ -z "$DEV" ]; then
    echo "Waiting for bootloader..."
    for i in $(seq 1 10); do
        sleep 1
        DEV=$(lsblk -o NAME -n -d | grep '^sd' | head -1)
        [ -n "$DEV" ] && break
    done
fi

if [ -z "$DEV" ]; then
    echo "Bootloader did not appear"
    exit 1
fi

echo "Mounting /dev/$DEV..."
MNT=$(udisksctl mount -b /dev/$DEV --no-user-interaction 2>/dev/null | grep -oP '/run/media/\S+' || true)
if [ -z "$MNT" ]; then
    MNT=/mnt
    sudo mount /dev/$DEV $MNT
    SUDO=1
fi

echo "Copying $UF2..."
cp "$UF2" "$MNT/"
sync

if [ "$SUDO" = "1" ]; then
    sudo umount "$MNT"
else
    udisksctl unmount -b /dev/$DEV --no-user-interaction 2>/dev/null || true
fi

echo "Flashed. Rebooting..."
