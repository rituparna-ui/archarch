#!/bin/bash
set -e

# Clean up
pkill -9 qemu-system-aarch64 2>/dev/null || true
sleep 1
dd if=/dev/zero of=disk.img bs=1M count=1 2>/dev/null
rm -f /tmp/qemu_serial.txt

echo "=== Starting QEMU ==="
qemu-system-aarch64 \
    -machine virt,gic-version=3 \
    -cpu cortex-a53 \
    -m 128M \
    -display none \
    -serial file:/tmp/qemu_serial.txt \
    -drive file=disk.img,if=none,format=raw,id=hd0 \
    -device virtio-blk-pci,drive=hd0 \
    -kernel kernel.elf &
QEMU_PID=$!
echo "QEMU PID: $QEMU_PID"

echo "=== Waiting 10s for boot ==="
sleep 10

echo "=== Boot output (first 15 lines) ==="
head -15 /tmp/qemu_serial.txt

echo ""
echo "=== Pushing update gen=1 ==="
python3 push_update.py disk.img kernel.bin 1

echo ""
echo "=== Waiting 15s for update detection ==="
sleep 15

echo ""
echo "=== FULL OUTPUT ==="
cat /tmp/qemu_serial.txt

echo ""
echo "=== Cleanup ==="
kill $QEMU_PID 2>/dev/null || true
wait $QEMU_PID 2>/dev/null || true
echo "DONE"
