CROSS = aarch64-linux-gnu-
CC = $(CROSS)gcc
AS = $(CROSS)as
LD = $(CROSS)ld
OBJCOPY = $(CROSS)objcopy

CFLAGS = -ffreestanding -nostdlib -nostartfiles -Wall -Wextra -O2 -mcpu=cortex-a53
LDFLAGS = -nostdlib -T linker.ld

OBJS = start.o main.o uart.o pci.o virtio_pci.o virtqueue.o virtio_rng.o virtio_blk.o virtio_net.o gic.o irq.o

all: kernel.elf kernel.bin

kernel.elf: $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

kernel.bin: kernel.elf
	$(OBJCOPY) -O binary $< $@

start.o: start.S
	$(AS) -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f *.o kernel.elf kernel.bin

run: kernel.elf disk.img
	qemu-system-aarch64 \
		-machine virt,gic-version=3 \
		-cpu cortex-a53 \
		-m 128M \
		-nographic \
		-device virtio-rng-pci \
		-drive file=disk.img,if=none,format=raw,id=hd0 \
		-device virtio-blk-pci,drive=hd0 \
		-netdev user,id=net0 \
		-device virtio-net-pci,netdev=net0 \
		-kernel kernel.elf

disk.img:
	dd if=/dev/zero of=disk.img bs=1M count=1

.PHONY: all clean run
