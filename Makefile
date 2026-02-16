CROSS = aarch64-linux-gnu-
CC = $(CROSS)gcc
AS = $(CROSS)as
LD = $(CROSS)ld
OBJCOPY = $(CROSS)objcopy

CFLAGS = -ffreestanding -nostdlib -nostartfiles -Wall -Wextra -O2 -mcpu=cortex-a53 -mstrict-align
LDFLAGS = -nostdlib -T linker.ld

OBJS = start.o main.o uart.o pci.o virtio_rng.o virtqueue.o

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

run3: kernel.elf
	/usr/bin/qemu-system-aarch64 \
		-machine virt \
		-cpu cortex-a53 \
		-m 128M \
		-nographic \
		-device virtio-rng-pci \
		-kernel kernel.elf

run10: kernel.elf
	 /home/linuxbrew/.linuxbrew/bin/qemu-system-aarch64 \
		-machine virt \
		-cpu cortex-a53 \
		-m 128M \
		-nographic \
		-device virtio-rng-pci \
		-kernel kernel.elf

.PHONY: all clean run
