CROSS = aarch64-linux-gnu-
CC = $(CROSS)gcc
AS = $(CROSS)as
LD = $(CROSS)ld
OBJCOPY = $(CROSS)objcopy

CFLAGS = -ffreestanding -nostdlib -nostartfiles -Wall -Wextra -O2 -mcpu=cortex-a53 -mstrict-align
LDFLAGS = -nostdlib -T linker.ld

OBJS = start.o main.o uart.o pci.o virtio_pci.o virtqueue.o virtio_rng.o virtio_blk.o virtio_net.o virtio_gpu.o virtio_input.o gic.o irq.o timer.o sched.o context_switch.o pmm.o mmu.o kmalloc.o syscall.o user.o el0_entry.o user_prog.o user_prog2.o user_prog3.o

all: kernel.elf kernel.bin

kernel.elf: $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

kernel.bin: kernel.elf
	$(OBJCOPY) -O binary $< $@

start.o: start.S
	$(AS) -o $@ $<

context_switch.o: context_switch.S
	$(AS) -o $@ $<

el0_entry.o: el0_entry.S
	$(AS) -o $@ $<

user_prog.o: user_prog.S
	$(AS) -o $@ $<

user_prog2.o: user_prog2.S
	$(AS) -o $@ $<

user_prog3.o: user_prog3.S
	$(AS) -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f *.o kernel.elf kernel.bin

run: kernel.elf disk.img
	@echo "VNC on :5900 (password: virtio), monitor on telnet :4444"
	qemu-system-aarch64 \
		-machine virt,gic-version=3 \
		-cpu cortex-a53 \
		-m 128M \
		-serial stdio \
		-device virtio-rng-pci \
		-drive file=disk.img,if=none,format=raw,id=hd0 \
		-device virtio-blk-pci,drive=hd0 \
		-netdev user,id=net0 \
		-device virtio-net-pci,netdev=net0 \
		-device virtio-gpu-pci \
		-device virtio-keyboard-pci \
		-device virtio-mouse-pci \
		-vnc :0,password=on \
		-monitor telnet:127.0.0.1:4444,server,nowait \
		-kernel kernel.elf

disk.img:
	dd if=/dev/zero of=disk.img bs=1M count=1

.PHONY: all clean run
