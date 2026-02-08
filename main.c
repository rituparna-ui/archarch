/*
 * Bare metal AArch64 virtio demo on QEMU virt machine.
 *
 * Demonstrates:
 *   - PCI ECAM enumeration and BAR assignment (I/O + 64-bit MMIO)
 *   - Virtio PCI capability parsing
 *   - Full virtio 1.x device initialization (shared helpers)
 *   - Split virtqueue with multi-descriptor chains
 *   - GICv3 interrupt controller + exception vector table
 *   - Interrupt-driven I/O (WFI instead of busy-polling)
 *   - virtio-rng: single-descriptor device-writable buffers
 *   - virtio-blk: 3-descriptor chains (header/data/status), read + write
 *   - virtio-net: 2 virtqueues (RX + TX), packet headers, ARP, ICMP
 */
#include "uart.h"
#include "pci.h"
#include "gic.h"
#include "irq.h"
#include "virtio_rng.h"
#include "virtio_blk.h"
#include "virtio_net.h"
#include "virtio_gpu.h"
#include "virtio_input.h"
#include "timer.h"
#include "sched.h"

static struct virtio_rng rng_dev;
static struct virtio_blk blk_dev;
static struct virtio_net net_dev;
static struct virtio_gpu gpu_dev;

static uint8_t rng_buf[64] __attribute__((aligned(64)));
static uint8_t blk_buf[512] __attribute__((aligned(512)));
static uint8_t blk_readback[512] __attribute__((aligned(512)));

/* Network buffers */
static uint8_t tx_frame[1514] __attribute__((aligned(16)));
static uint8_t rx_frame[1514] __attribute__((aligned(16)));

static const char hexc[] = "0123456789abcdef";

static void print_hex_bytes(const uint8_t *data, int len) {
    for (int i = 0; i < len; i++) {
        uart_putc(hexc[data[i] >> 4]);
        uart_putc(hexc[data[i] & 0xf]);
        uart_putc(' ');
    }
}

static void print_mac(const uint8_t *mac) {
    for (int i = 0; i < 6; i++) {
        uart_putc(hexc[mac[i] >> 4]);
        uart_putc(hexc[mac[i] & 0xf]);
        if (i < 5) uart_putc(':');
    }
}

/* ---- helpers to build network packets ---- */

static void memset8(void *dst, uint8_t val, int n) {
    uint8_t *d = (uint8_t *)dst;
    for (int i = 0; i < n; i++) d[i] = val;
}

static void memcpy8(void *dst, const void *src, int n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (int i = 0; i < n; i++) d[i] = s[i];
}

static void write16be(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xff);
}

static uint16_t read16be(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

/*
 * Build an ARP request.
 * QEMU user-mode networking: guest is 10.0.2.15, gateway is 10.0.2.2.
 */
static int build_arp_request(uint8_t *frame, const uint8_t *src_mac,
                             const uint8_t *src_ip, const uint8_t *target_ip)
{
    int off = 0;
    memset8(frame + off, 0xff, 6);  off += 6;
    memcpy8(frame + off, src_mac, 6); off += 6;
    write16be(frame + off, 0x0806); off += 2;

    write16be(frame + off, 0x0001); off += 2;
    write16be(frame + off, 0x0800); off += 2;
    frame[off++] = 6;
    frame[off++] = 4;
    write16be(frame + off, 0x0001); off += 2;

    memcpy8(frame + off, src_mac, 6); off += 6;
    memcpy8(frame + off, src_ip, 4);  off += 4;
    memset8(frame + off, 0, 6);       off += 6;
    memcpy8(frame + off, target_ip, 4); off += 4;

    while (off < 60)
        frame[off++] = 0;
    return off;
}

static uint16_t ip_checksum(const uint8_t *data, int len) {
    uint32_t sum = 0;
    for (int i = 0; i < len - 1; i += 2)
        sum += (uint32_t)read16be(data + i);
    if (len & 1)
        sum += (uint32_t)data[len - 1] << 8;
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

static int build_icmp_echo(uint8_t *frame, const uint8_t *src_mac,
                           const uint8_t *dst_mac,
                           const uint8_t *src_ip, const uint8_t *dst_ip,
                           uint16_t seq)
{
    int off = 0;
    memcpy8(frame + off, dst_mac, 6); off += 6;
    memcpy8(frame + off, src_mac, 6); off += 6;
    write16be(frame + off, 0x0800);   off += 2;

    int ip_start = off;
    frame[off++] = 0x45;
    frame[off++] = 0x00;
    int total_len_off = off; off += 2;
    write16be(frame + off, 0x1234); off += 2;
    write16be(frame + off, 0x0000); off += 2;
    frame[off++] = 64;
    frame[off++] = 1;
    int ip_csum_off = off; off += 2;
    memcpy8(frame + off, src_ip, 4); off += 4;
    memcpy8(frame + off, dst_ip, 4); off += 4;

    int icmp_start = off;
    frame[off++] = 8;
    frame[off++] = 0;
    int icmp_csum_off = off; off += 2;
    write16be(frame + off, 0x0001); off += 2;
    write16be(frame + off, seq);    off += 2;

    for (int i = 0; i < 32; i++)
        frame[off++] = (uint8_t)(0x41 + (i % 26));

    uint16_t ip_total = (uint16_t)(off - ip_start);
    write16be(frame + total_len_off, ip_total);

    write16be(frame + ip_csum_off, 0);
    uint16_t ipcsum = ip_checksum(frame + ip_start, 20);
    write16be(frame + ip_csum_off, ipcsum);

    int icmp_len = off - icmp_start;
    write16be(frame + icmp_csum_off, 0);
    uint16_t icmpcsum = ip_checksum(frame + icmp_start, icmp_len);
    write16be(frame + icmp_csum_off, icmpcsum);

    return off;
}

static void print_ethertype(uint16_t et) {
    if (et == 0x0806) uart_puts("ARP");
    else if (et == 0x0800) uart_puts("IPv4");
    else if (et == 0x86dd) uart_puts("IPv6");
    else { uart_puts("0x"); uart_puthex(et); }
}

/* ---- Demo: virtio-rng with interrupt notification ---- */
static void demo_rng(void) {
    uart_puts("--- virtio-rng demo (interrupt-driven) ---\n");
    if (virtio_rng_init(&rng_dev) < 0) {
        uart_puts("SKIP: virtio-rng not available\n\n");
        return;
    }

    /* Register for interrupts */
    uint32_t irq = pci_get_irq(&rng_dev.pci);
    if (irq)
        irq_register_rng(&rng_dev.vpci, irq);

    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 64; j++) rng_buf[j] = 0;
        int got = virtio_rng_read(&rng_dev, rng_buf, 64);
        if (got < 0) { uart_puts("  read failed\n"); continue; }
        uart_puts("  ");
        uart_putdec((uint64_t)got);
        uart_puts("B: ");
        print_hex_bytes(rng_buf, got < 16 ? got : 16);
        uart_puts("...\n");
    }
    uart_puts("\n");
}

/* ---- Demo: virtio-blk with interrupt-driven completion ---- */
static void demo_blk(void) {
    uart_puts("--- virtio-blk demo (interrupt-driven) ---\n");
    if (virtio_blk_init(&blk_dev) < 0) {
        uart_puts("SKIP: virtio-blk not available\n\n");
        return;
    }

    /* Register for interrupts */
    uint32_t irq = pci_get_irq(&blk_dev.pci);
    if (irq)
        irq_register_blk(&blk_dev.vpci, irq);

    uart_puts("[BLK] Write sector 0...\n");
    for (int i = 0; i < 512; i++) blk_buf[i] = (uint8_t)(i & 0xFF);
    const char *sig = "VIRTIO-BLK-TEST!";
    for (int i = 0; sig[i]; i++) blk_buf[i] = (uint8_t)sig[i];

    if (virtio_blk_write(&blk_dev, 0, 1, blk_buf) < 0) {
        uart_puts("[BLK] Write FAILED\n"); return;
    }

    uart_puts("[BLK] Read sector 0...\n");
    memset8(blk_readback, 0, 512);
    if (virtio_blk_read(&blk_dev, 0, 1, blk_readback) < 0) {
        uart_puts("[BLK] Read FAILED\n"); return;
    }

    uart_puts("[BLK] Sig: \"");
    for (int i = 0; i < 16; i++) uart_putc((char)blk_readback[i]);
    uart_puts("\"\n");

    int match = 1;
    for (int i = 0; i < 512; i++) {
        if (blk_buf[i] != blk_readback[i]) { match = 0; break; }
    }
    uart_puts(match ? "[BLK] VERIFY OK\n" : "[BLK] VERIFY FAILED\n");
    uart_puts("\n");
}

/* ---- Demo: virtio-net with interrupt-driven RX ---- */
static void demo_net(void) {
    uart_puts("--- virtio-net demo (interrupt-driven) ---\n");
    if (virtio_net_init(&net_dev) < 0) {
        uart_puts("SKIP: virtio-net not available\n\n");
        return;
    }

    /* Register for interrupts */
    uint32_t irq = pci_get_irq(&net_dev.pci);
    if (irq)
        irq_register_net(&net_dev.vpci, irq);

    uint8_t our_ip[]  = {10, 0, 2, 15};
    uint8_t gw_ip[]   = {10, 0, 2, 2};
    uint8_t gw_mac[6] = {0};

    /* --- Step 1: ARP request for gateway --- */
    uart_puts("[NET] Sending ARP request: who-has 10.0.2.2?\n");
    int arp_len = build_arp_request(tx_frame, net_dev.mac, our_ip, gw_ip);
    if (virtio_net_tx(&net_dev, tx_frame, (uint32_t)arp_len) < 0) {
        uart_puts("[NET] ARP TX failed\n");
        return;
    }
    uart_puts("[NET] ARP sent, waiting for reply (WFI)...\n");

    /* Wait for ARP reply using interrupts */
    int got_arp = 0;
    irq_net_rx_pending = 0;
    for (uint64_t t = 0; t < 50000000 && !got_arp; t++) {
        /*
         * Hybrid wait: check used ring, then WFI if nothing.
         * The interrupt handler sets irq_net_rx_pending which
         * causes WFI to return immediately (pending IRQ wakes CPU).
         */
        int rxlen = virtio_net_rx(&net_dev, rx_frame, sizeof(rx_frame));
        if (rxlen <= 0) {
            /* No packet — sleep until next interrupt */
            wfi();
            continue;
        }

        uint16_t ethertype = read16be(rx_frame + 12);
        uart_puts("[NET] RX ");
        uart_putdec((uint64_t)rxlen);
        uart_puts("B: src=");
        print_mac(rx_frame + 6);
        uart_puts(" type=");
        print_ethertype(ethertype);
        uart_puts("\n");

        if (ethertype == 0x0806) {
            uint16_t arp_op = read16be(rx_frame + 20);
            if (arp_op == 2) {
                memcpy8(gw_mac, rx_frame + 22, 6);
                uart_puts("[NET] ARP reply: 10.0.2.2 is at ");
                print_mac(gw_mac);
                uart_puts("\n");
                got_arp = 1;
            }
        }
    }

    if (!got_arp) {
        uart_puts("[NET] No ARP reply (timeout)\n");
        memset8(gw_mac, 0xff, 6);
    }

    /* --- Step 2: ICMP echo (ping) to gateway --- */
    uart_puts("\n[NET] Sending ICMP echo to 10.0.2.2...\n");
    int ping_len = build_icmp_echo(tx_frame, net_dev.mac, gw_mac,
                                   our_ip, gw_ip, 1);
    uart_puts("[NET] TX ping: ");
    uart_putdec((uint64_t)ping_len);
    uart_puts(" bytes\n");

    if (virtio_net_tx(&net_dev, tx_frame, (uint32_t)ping_len) < 0) {
        uart_puts("[NET] Ping TX failed\n");
        return;
    }

    /* Wait for echo reply using interrupts */
    uart_puts("[NET] Waiting for echo reply (WFI)...\n");
    int got_pong = 0;
    irq_net_rx_pending = 0;
    for (uint64_t t = 0; t < 50000000 && !got_pong; t++) {
        int rxlen = virtio_net_rx(&net_dev, rx_frame, sizeof(rx_frame));
        if (rxlen <= 0) {
            wfi();
            continue;
        }

        uint16_t ethertype = read16be(rx_frame + 12);
        uart_puts("[NET] RX ");
        uart_putdec((uint64_t)rxlen);
        uart_puts("B: src=");
        print_mac(rx_frame + 6);
        uart_puts(" type=");
        print_ethertype(ethertype);

        if (ethertype == 0x0800 && rxlen >= 34) {
            uint8_t proto = rx_frame[23];
            if (proto == 1) {
                uint8_t icmp_type = rx_frame[34];
                uart_puts(" ICMP type=");
                uart_putdec(icmp_type);
                if (icmp_type == 0) {
                    uart_puts(" (echo reply!)");
                    got_pong = 1;
                }
            }
        }
        uart_puts("\n");
    }

    if (got_pong)
        uart_puts("[NET] PING SUCCESS — got echo reply from gateway!\n");
    else
        uart_puts("[NET] No echo reply (timeout)\n");

    /* --- Step 3: Drain remaining packets --- */
    uart_puts("\n[NET] Draining remaining packets...\n");
    int drained = 0;
    for (uint64_t t = 0; t < 5000000; t++) {
        int rxlen = virtio_net_rx(&net_dev, rx_frame, sizeof(rx_frame));
        if (rxlen <= 0) continue;
        drained++;
        uint16_t et = read16be(rx_frame + 12);
        uart_puts("[NET] RX ");
        uart_putdec((uint64_t)rxlen);
        uart_puts("B type=");
        print_ethertype(et);
        uart_puts("\n");
    }
    uart_puts("[NET] Drained ");
    uart_putdec((uint64_t)drained);
    uart_puts(" extra packets\n\n");
}

/* ---- Demo: virtio-gpu 2D rendering ---- */
static void demo_gpu(void) {
    uart_puts("--- virtio-gpu demo (2D framebuffer) ---\n");
    if (virtio_gpu_init(&gpu_dev) < 0) {
        uart_puts("SKIP: virtio-gpu not available\n\n");
        return;
    }

    /* Register for interrupts */
    uint32_t irq = pci_get_irq(&gpu_dev.pci);
    if (irq)
        irq_register_gpu(&gpu_dev.vpci, irq);

    uint32_t *fb = virtio_gpu_get_framebuffer();
    uint32_t w = gpu_dev.width;
    uint32_t h = gpu_dev.height;

    /*
     * Draw a colorful test pattern:
     *   - Red/green/blue/white quadrants
     *   - Gradient bars within each quadrant
     *   - A centered yellow rectangle
     */
    uart_puts("[GPU] Drawing test pattern...\n");

    uint32_t hw = w / 2;
    uint32_t hh = h / 2;

    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint32_t pixel;
            uint8_t intensity = (uint8_t)((x % hw) * 255 / hw);

            if (y < hh) {
                if (x < hw)
                    pixel = 0xFF000000 | (uint32_t)intensity;          /* Red */
                else
                    pixel = 0xFF000000 | ((uint32_t)intensity << 8);   /* Green */
            } else {
                if (x < hw)
                    pixel = 0xFF000000 | ((uint32_t)intensity << 16);  /* Blue */
                else
                    pixel = 0xFF000000 | (uint32_t)intensity |
                            ((uint32_t)intensity << 8) |
                            ((uint32_t)intensity << 16);               /* White */
            }
            fb[y * w + x] = pixel;
        }
    }

    /* Draw a centered yellow rectangle (160x120) */
    uint32_t rx = (w - 160) / 2;
    uint32_t ry = (h - 120) / 2;
    for (uint32_t y = ry; y < ry + 120; y++) {
        for (uint32_t x = rx; x < rx + 160; x++) {
            fb[y * w + x] = 0xFF00FFFF; /* Yellow in RGBA (R=0xFF, G=0xFF, B=0, A=0xFF) */
        }
    }

    /* Flush entire framebuffer to display */
    uart_puts("[GPU] Flushing to display...\n");
    if (virtio_gpu_flush(&gpu_dev, 0, 0, w, h) == 0) {
        uart_puts("[GPU] FLUSH OK — test pattern displayed!\n");
        uart_puts("[GPU] Pattern: 4 quadrants (R/G/B/W gradients) + yellow center rect\n");
    } else {
        uart_puts("[GPU] FLUSH FAILED\n");
    }

    /*
     * Draw a second frame: color bars.
     * Delay ~2 seconds so the first frame is visible on VNC.
     */
    uart_puts("[GPU] Pausing 2s...\n");
    for (volatile uint64_t d = 0; d < 600000000; d++)
        ;

    uart_puts("[GPU] Drawing color bars...\n");
    uint32_t colors[] = {
        0xFF0000FF, /* Red */
        0xFF00FF00, /* Green */
        0xFFFF0000, /* Blue */
        0xFF00FFFF, /* Yellow */
        0xFFFF00FF, /* Magenta */
        0xFFFFFF00, /* Cyan */
        0xFFFFFFFF, /* White */
        0xFF808080, /* Gray */
    };
    uint32_t bar_h = h / 8;
    for (uint32_t i = 0; i < 8; i++) {
        uint32_t y0 = i * bar_h;
        uint32_t y1 = (i == 7) ? h : y0 + bar_h;
        for (uint32_t y = y0; y < y1; y++)
            for (uint32_t x = 0; x < w; x++)
                fb[y * w + x] = colors[i];
    }

    if (virtio_gpu_flush(&gpu_dev, 0, 0, w, h) == 0) {
        uart_puts("[GPU] Color bars displayed!\n");
    }

    uart_puts("[GPU] Demo complete.\n");
    uart_puts("[GPU] Connect VNC viewer to :5900 to see the display,\n");
    uart_puts("[GPU] or use QEMU monitor 'screendump /tmp/frame.ppm'\n\n");
}

/* ---- Demo: virtio-input (keyboard + mouse from VNC) ---- */
static void demo_input(void) {
    uart_puts("--- virtio-input demo (keyboard/mouse) ---\n");
    int count = virtio_input_init_all();
    if (count == 0) {
        uart_puts("SKIP: no virtio-input devices found\n\n");
        return;
    }

    /* Register all input devices for interrupts */
    for (int i = 0; i < count; i++) {
        struct virtio_input *dev = virtio_input_get_dev(i);
        uint32_t irq = pci_get_irq(&dev->pci);
        if (irq)
            irq_register_input(&dev->vpci, irq);
    }

    uart_puts("[INPUT] Listening for events...\n");
    uart_puts("[INPUT] Use QEMU monitor to send keys:\n");
    uart_puts("[INPUT]   telnet 127.0.0.1 4444\n");
    uart_puts("[INPUT]   sendkey a\n");
    uart_puts("[INPUT]   sendkey ret\n");
    uart_puts("[INPUT] Press ESC (sendkey esc) to end demo.\n\n");

    int done = 0;
    int evt_count = 0;
    uint64_t loop_count = 0;
    uint64_t max_loops = 50000000; /* Auto-exit after ~5s if no ESC */
    struct virtio_input_event evt;

    while (!done && loop_count < max_loops) {
        loop_count++;

        /* Periodic heartbeat every ~10M iterations */
        if ((loop_count % 10000000) == 0) {
            uart_puts("[INPUT] heartbeat, loops=");
            uart_putdec(loop_count);
            uart_puts(" events=");
            uart_putdec((uint64_t)evt_count);
            /* Dump used ring index for keyboard eventq */
            struct virtio_input *kbd = virtio_input_get_dev(0);
            if (kbd) {
                uart_puts(" kbd_used_idx=");
                uart_putdec(kbd->evtq.used->idx);
                uart_puts("/");
                uart_putdec(kbd->evtq.last_used_idx);
            }
            uart_puts("\n");
        }

        if (virtio_input_poll(&evt)) {
            /* Filter out EV_SYN noise */
            if (evt.type == EV_SYN) {
                /* Still count SYN to prove events are arriving */
                evt_count++;
                continue;
            }

            evt_count++;

            if (evt.type == EV_KEY) {
                const char *name = key_name(evt.code);
                if (evt.value == 1) {
                    uart_puts("[KEY] DOWN: ");
                    uart_puts(name);
                    uart_puts(" (code=");
                    uart_putdec(evt.code);
                    uart_puts(")\n");
                } else if (evt.value == 0) {
                    uart_puts("[KEY] UP:   ");
                    uart_puts(name);
                    uart_puts("\n");
                }

                /* ESC key release ends the demo */
                if (evt.code == KEY_ESC && evt.value == 0)
                    done = 1;

            } else if (evt.type == EV_REL) {
                if (evt.code == REL_X) {
                    uart_puts("[MOUSE] REL_X: ");
                    if (evt.value & 0x80000000) {
                        uart_puts("-");
                        uart_putdec((uint64_t)(-(int64_t)(int32_t)evt.value));
                    } else {
                        uart_putdec(evt.value);
                    }
                    uart_puts("\n");
                } else if (evt.code == REL_Y) {
                    uart_puts("[MOUSE] REL_Y: ");
                    if (evt.value & 0x80000000) {
                        uart_puts("-");
                        uart_putdec((uint64_t)(-(int64_t)(int32_t)evt.value));
                    } else {
                        uart_putdec(evt.value);
                    }
                    uart_puts("\n");
                } else if (evt.code == REL_WHEEL) {
                    uart_puts("[MOUSE] WHEEL: ");
                    uart_putdec(evt.value);
                    uart_puts("\n");
                }

            } else if (evt.type == EV_ABS) {
                if (evt.code == ABS_X) {
                    uart_puts("[TABLET] ABS_X: ");
                    uart_putdec(evt.value);
                    uart_puts("\n");
                } else if (evt.code == ABS_Y) {
                    uart_puts("[TABLET] ABS_Y: ");
                    uart_putdec(evt.value);
                    uart_puts("\n");
                }

            } else {
                uart_puts("[INPUT] type=");
                uart_putdec(evt.type);
                uart_puts(" code=");
                uart_putdec(evt.code);
                uart_puts(" value=");
                uart_putdec(evt.value);
                uart_puts("\n");
            }
        } else {
            /* No event — busy poll */
        }
    }

    uart_puts("\n[INPUT] Demo ended. Received ");
    uart_putdec((uint64_t)evt_count);
    uart_puts(" events.\n\n");
}

/* ---- Demo: Timer + Preemptive Scheduler ---- */

/*
 * Three demo tasks that run concurrently, preempted by the timer.
 * Each task does some "work" (busy loop + prints), then finishes.
 * The scheduler round-robins between them on every timer tick.
 */

static void task_counter(void *arg) {
    const char *label = (const char *)arg;
    for (int i = 1; i <= 5; i++) {
        uart_puts("  [");
        uart_puts(label);
        uart_puts("] count=");
        uart_putdec((uint64_t)i);
        uart_puts(" (task ");
        uart_putdec((uint64_t)sched_current_id());
        uart_puts(", t=");
        uart_putdec(timer_ms());
        uart_puts("ms)\n");

        /* Busy work — will get preempted by timer */
        for (volatile uint64_t d = 0; d < 20000000; d++)
            ;
    }
}

static void task_fibonacci(void *arg) {
    (void)arg;
    uint64_t a = 0, b = 1;
    for (int i = 0; i < 10; i++) {
        uart_puts("  [FIB] fib(");
        uart_putdec((uint64_t)i);
        uart_puts(")=");
        uart_putdec(a);
        uart_puts(" (task ");
        uart_putdec((uint64_t)sched_current_id());
        uart_puts(", t=");
        uart_putdec(timer_ms());
        uart_puts("ms)\n");

        uint64_t next = a + b;
        a = b;
        b = next;

        /* Busy work */
        for (volatile uint64_t d = 0; d < 15000000; d++)
            ;
    }
}

static void task_dots(void *arg) {
    (void)arg;
    for (int i = 0; i < 20; i++) {
        uart_puts("  [DOTS] ");
        for (int j = 0; j <= i % 10; j++)
            uart_putc('.');
        uart_puts(" (task ");
        uart_putdec((uint64_t)sched_current_id());
        uart_puts(", t=");
        uart_putdec(timer_ms());
        uart_puts("ms)\n");

        /* Busy work */
        for (volatile uint64_t d = 0; d < 10000000; d++)
            ;
    }
}

static void demo_sched(void) {
    uart_puts("--- timer + scheduler demo (preemptive round-robin) ---\n");

    /* Initialize timer: 10ms tick */
    timer_init(10);
    irq_register_timer();

    /* Initialize scheduler (current context becomes idle task) */
    sched_init();

    /* Create worker tasks */
    sched_create("counter-A", task_counter, (void *)"A");
    sched_create("counter-B", task_counter, (void *)"B");
    sched_create("fibonacci", task_fibonacci, NULL);
    sched_create("dots",      task_dots,      NULL);

    uart_puts("\n[SCHED] Starting tasks...\n\n");

    /*
     * Idle loop: yield to worker tasks.
     * When all workers finish, pick_next returns idle (us),
     * and we break out.
     */
    int alive;
    do {
        sched_yield();

        /* Check if any non-idle tasks are still running */
        alive = 0;
        for (int i = 1; i < sched_task_count(); i++) {
            struct task *t = sched_get_task(i);
            if (t && (t->state == TASK_READY || t->state == TASK_RUNNING))
                alive = 1;
        }
    } while (alive);

    /* Stop the timer */
    timer_disable();

    uart_puts("\n[SCHED] All tasks finished!\n");
    uart_puts("[SCHED] Timer ticks: ");
    uart_putdec(irq_timer_ticks);
    uart_puts(", elapsed: ");
    uart_putdec(timer_ms());
    uart_puts("ms\n");

    /* Print per-task stats */
    uart_puts("[SCHED] Task stats:\n");
    for (int i = 0; i < sched_task_count(); i++) {
        struct task *t = sched_get_task(i);
        if (!t) continue;
        uart_puts("  task ");
        uart_putdec((uint64_t)i);
        uart_puts(" \"");
        uart_puts(t->name);
        uart_puts("\": ");
        uart_putdec(t->ticks);
        uart_puts(" ticks, state=");
        if (t->state == TASK_FINISHED) uart_puts("finished");
        else if (t->state == TASK_READY) uart_puts("ready");
        else if (t->state == TASK_RUNNING) uart_puts("running");
        else uart_puts("?");
        uart_puts("\n");
    }
    uart_puts("\n");
}

void main(void) {
    uart_init();
    uart_puts("\n==========================================\n");
    uart_puts("  AArch64 Bare Metal Virtio Demo\n");
    uart_puts("  PCI ECAM / Virtio 1.x / Split VQ\n");
    uart_puts("  GICv3 Interrupts / WFI\n");
    uart_puts("  Devices: RNG + Block + Network + GPU + Input\n");
    uart_puts("  Timer + Preemptive Scheduler\n");
    uart_puts("==========================================\n\n");

    /* Initialize GICv3 and interrupt dispatch */
    gic_init();
    irq_init();

    /* Enable IRQs at CPU level */
    irq_enable();
    uart_puts("[IRQ] CPU interrupts enabled\n\n");

    pci_enumerate();
    uart_puts("\n");

    // demo_rng();
    // demo_blk();
    // demo_net();
    // demo_gpu();
    // demo_input();
    demo_sched();

    /* Disable IRQs before halting */
    irq_disable();

    uart_puts("==========================================\n");
    uart_puts("  All demos complete. System halted.\n");
    uart_puts("==========================================\n");

    for (;;)
        __asm__ volatile("wfe");
}
