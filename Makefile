CROSS = aarch64-linux-gnu-
CC = $(CROSS)gcc
AS = $(CROSS)as
LD = $(CROSS)ld
OBJCOPY = $(CROSS)objcopy

CFLAGS = -ffreestanding -nostdlib -nostartfiles -Wall -Wextra -O0 -march=armv8-a -mstrict-align
LDFLAGS = -nostdlib -T linker.ld

OBJS = start.o main.o uart.o pci.o virtio_pci.o virtqueue.o virtio_rng.o \
       virtio_blk.o virtio_net.o virtio_gpu.o virtio_input.o gic.o irq.o \
       timer.o sched.o context_switch.o pmm.o mmu.o kmalloc.o syscall.o \
       user.o el0_entry.o user_prog.o user_prog2.o user_prog3.o fat16.o fd.o \
       signal.o

# User programs to put on the FAT16 disk
UPROGS = uprogs/hello.bin uprogs/fib.bin uprogs/spin.bin uprogs/shell.bin uprogs/badmem.bin uprogs/stksmash.bin uprogs/forktest.bin uprogs/pipetest.bin uprogs/sigtest.bin uprogs/sbrktest.bin

# User program C flags
UCFLAGS = -ffreestanding -nostdlib -nostartfiles -O2 -march=armv8-a -mstrict-align -I uprogs

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

# Build standalone user programs (assembly) as flat binaries
uprogs/%.bin: uprogs/%.S user_link.ld
	$(AS) -o uprogs/$*.o $<
	$(LD) -nostdlib -T user_link.ld -o uprogs/$*.elf uprogs/$*.o
	$(OBJCOPY) -O binary uprogs/$*.elf $@
	@echo "  UPROG $@ ($$(stat -c%s $@) bytes)"

# Build C user programs (crt0.S + .c) as flat binaries
uprogs/shell.bin: uprogs/shell.c uprogs/crt0.S uprogs/usys.h user_link.ld
	$(AS) -o uprogs/crt0.o uprogs/crt0.S
	$(CC) $(UCFLAGS) -c -o uprogs/shell.o uprogs/shell.c
	$(LD) -nostdlib -T user_link.ld -o uprogs/shell.elf uprogs/crt0.o uprogs/shell.o
	$(OBJCOPY) -O binary uprogs/shell.elf $@
	@echo "  UPROG $@ ($$(stat -c%s $@) bytes)"

uprogs/forktest.bin: uprogs/forktest.c uprogs/crt0.S uprogs/usys.h user_link.ld
	$(AS) -o uprogs/crt0.o uprogs/crt0.S
	$(CC) $(UCFLAGS) -c -o uprogs/forktest.o uprogs/forktest.c
	$(LD) -nostdlib -T user_link.ld -o uprogs/forktest.elf uprogs/crt0.o uprogs/forktest.o
	$(OBJCOPY) -O binary uprogs/forktest.elf $@
	@echo "  UPROG $@ ($$(stat -c%s $@) bytes)"

uprogs/pipetest.bin: uprogs/pipetest.c uprogs/crt0.S uprogs/usys.h user_link.ld
	$(AS) -o uprogs/crt0.o uprogs/crt0.S
	$(CC) $(UCFLAGS) -c -o uprogs/pipetest.o uprogs/pipetest.c
	$(LD) -nostdlib -T user_link.ld -o uprogs/pipetest.elf uprogs/crt0.o uprogs/pipetest.o
	$(OBJCOPY) -O binary uprogs/pipetest.elf $@
	@echo "  UPROG $@ ($$(stat -c%s $@) bytes)"

uprogs/sigtest.bin: uprogs/sigtest.c uprogs/crt0.S uprogs/usys.h user_link.ld
	$(AS) -o uprogs/crt0.o uprogs/crt0.S
	$(CC) $(UCFLAGS) -c -o uprogs/sigtest.o uprogs/sigtest.c
	$(LD) -nostdlib -T user_link.ld -o uprogs/sigtest.elf uprogs/crt0.o uprogs/sigtest.o
	$(OBJCOPY) -O binary uprogs/sigtest.elf $@
	@echo "  UPROG $@ ($$(stat -c%s $@) bytes)"

# Create FAT16 disk image with user programs
disk.img: $(UPROGS)
	@echo "Creating FAT16 disk image..."
	dd if=/dev/zero of=disk.img bs=1M count=32 2>/dev/null
	mkfs.fat -F 16 -n VIRTDISK disk.img >/dev/null
	for f in $(UPROGS); do mcopy -i disk.img $$f ::$$(basename $$f); done
	mcopy -i disk.img uprogs/readme.txt ::README.TXT
	@echo "Disk contents:"
	@mdir -i disk.img :: 2>/dev/null || true

clean:
	rm -f *.o kernel.elf kernel.bin disk.img
	rm -f uprogs/*.o uprogs/*.elf uprogs/*.bin

run: kernel.elf disk.img
	qemu-system-aarch64 \
		-machine virt,gic-version=3 \
		-cpu max \
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
		-display none \
		-kernel kernel.elf

.PHONY: all clean run

uprogs/sbrktest.bin: uprogs/sbrktest.c uprogs/crt0.S uprogs/usys.h user_link.ld
	$(AS) -o uprogs/crt0.o uprogs/crt0.S
	$(CC) $(UCFLAGS) -c -o uprogs/sbrktest.o uprogs/sbrktest.c
	$(LD) -nostdlib -T user_link.ld -o uprogs/sbrktest.elf uprogs/crt0.o uprogs/sbrktest.o
	$(OBJCOPY) -O binary uprogs/sbrktest.elf $@
	@echo "  UPROG $@ ($(stat -c%s $@) bytes)"

