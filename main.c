#include "uart.h"
#include "pci.h"
#include "gic.h"
#include "irq.h"
#include "pmm.h"
#include "mmu.h"
#include "kmalloc.h"
#include "virtio_rng.h"
#include "virtio_blk.h"
#include "virtio_net.h"
#include "virtio_gpu.h"
#include "virtio_input.h"
#include "timer.h"
#include "sched.h"
#include "user.h"
#include "fat16.h"

static struct virtio_rng rng_dev;
static struct virtio_blk blk_dev;
static struct virtio_net net_dev;
static struct virtio_gpu gpu_dev;

/* Global filesystem — referenced by syscall.c */
struct fat16_fs root_fs;

static uint8_t rng_buf[64] __attribute__((aligned(64)));
static uint8_t blk_buf[512] __attribute__((aligned(512)));
static uint8_t blk_readback[512] __attribute__((aligned(512)));

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


static void demo_rng(void) {
    uart_puts("--- virtio-rng demo (interrupt-driven) ---\n");
    if (virtio_rng_init(&rng_dev) < 0) {
        uart_puts("SKIP: virtio-rng not available\n\n");
        return;
    }

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

static void demo_blk(void) {
    uart_puts("--- virtio-blk demo (interrupt-driven) ---\n");
    if (virtio_blk_init(&blk_dev) < 0) {
        uart_puts("SKIP: virtio-blk not available\n\n");
        return;
    }

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

static void demo_net(void) {
    uart_puts("--- virtio-net demo (interrupt-driven) ---\n");
    if (virtio_net_init(&net_dev) < 0) {
        uart_puts("SKIP: virtio-net not available\n\n");
        return;
    }

    uint32_t irq = pci_get_irq(&net_dev.pci);
    if (irq)
        irq_register_net(&net_dev.vpci, irq);

    uint8_t our_ip[]  = {10, 0, 2, 15};
    uint8_t gw_ip[]   = {10, 0, 2, 2};
    uint8_t gw_mac[6] = {0};

    uart_puts("[NET] Sending ARP request: who-has 10.0.2.2?\n");
    int arp_len = build_arp_request(tx_frame, net_dev.mac, our_ip, gw_ip);
    if (virtio_net_tx(&net_dev, tx_frame, (uint32_t)arp_len) < 0) {
        uart_puts("[NET] ARP TX failed\n");
        return;
    }
    uart_puts("[NET] ARP sent, waiting for reply (WFI)...\n");

    int got_arp = 0;
    irq_net_rx_pending = 0;
    for (uint64_t t = 0; t < 50000000 && !got_arp; t++) {
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

static void demo_gpu(void) {
    uart_puts("--- virtio-gpu demo (2D framebuffer) ---\n");
    if (virtio_gpu_init(&gpu_dev) < 0) {
        uart_puts("SKIP: virtio-gpu not available\n\n");
        return;
    }

    uint32_t irq = pci_get_irq(&gpu_dev.pci);
    if (irq)
        irq_register_gpu(&gpu_dev.vpci, irq);

    uint32_t *fb = virtio_gpu_get_framebuffer();
    uint32_t w = gpu_dev.width;
    uint32_t h = gpu_dev.height;

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

    uint32_t rx = (w - 160) / 2;
    uint32_t ry = (h - 120) / 2;
    for (uint32_t y = ry; y < ry + 120; y++) {
        for (uint32_t x = rx; x < rx + 160; x++) {
            fb[y * w + x] = 0xFF00FFFF;
        }
    }

    uart_puts("[GPU] Flushing to display...\n");
    if (virtio_gpu_flush(&gpu_dev, 0, 0, w, h) == 0) {
        uart_puts("[GPU] FLUSH OK — test pattern displayed!\n");
        uart_puts("[GPU] Pattern: 4 quadrants (R/G/B/W gradients) + yellow center rect\n");
    } else {
        uart_puts("[GPU] FLUSH FAILED\n");
    }

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

static void demo_input(void) {
    uart_puts("--- virtio-input demo (keyboard/mouse) ---\n");
    int count = virtio_input_init_all();
    if (count == 0) {
        uart_puts("SKIP: no virtio-input devices found\n\n");
        return;
    }

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
    uint64_t max_loops = 5000000000;
    struct virtio_input_event evt;

    while (!done && loop_count < max_loops) {
        loop_count++;

        if ((loop_count % 10000000) == 0) {
            uart_puts("[INPUT] heartbeat, loops=");
            uart_putdec(loop_count);
            uart_puts(" events=");
            uart_putdec((uint64_t)evt_count);
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
            if (evt.type == EV_SYN) {
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

        /* Busy wait — will get preempted */
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

    sched_init();

    sched_create("counter-A", task_counter, (void *)"A");
    sched_create("counter-B", task_counter, (void *)"B");
    sched_create("fibonacci", task_fibonacci, NULL);
    sched_create("dots",      task_dots,      NULL);

    uart_puts("\n[SCHED] Starting tasks...\n\n");

    
    int alive;
    do {
        sched_yield();

        alive = 0;
        for (int i = 1; i < sched_task_count(); i++) {
            struct task *t = sched_get_task(i);
            if (t && (t->state == TASK_READY || t->state == TASK_RUNNING))
                alive = 1;
        }
    } while (alive);

    timer_disable();

    uart_puts("\n[SCHED] All tasks finished!\n");
    uart_puts("[SCHED] Timer ticks: ");
    uart_putdec(irq_timer_ticks);
    uart_puts(", elapsed: ");
    uart_putdec(timer_ms());
    uart_puts("ms\n");

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

static void demo_memory(void) {
    uart_puts("--- memory management demo ---\n\n");

    /* PMM: allocate and free individual pages */
    uart_puts("[TEST] Allocating 4 pages...\n");
    uintptr_t p1 = pmm_alloc_page();
    uintptr_t p2 = pmm_alloc_page();
    uintptr_t p3 = pmm_alloc_page();
    uintptr_t p4 = pmm_alloc_page();
    uart_puts("  p1="); uart_puthex(p1); uart_puts("\n");
    uart_puts("  p2="); uart_puthex(p2); uart_puts("\n");
    uart_puts("  p3="); uart_puthex(p3); uart_puts("\n");
    uart_puts("  p4="); uart_puthex(p4); uart_puts("\n");

    uart_puts("[TEST] Freeing p2 and p3...\n");
    pmm_free_page(p2);
    pmm_free_page(p3);
    uart_puts("  Free pages: "); uart_putdec(pmm_free_count()); uart_puts("\n");

    uart_puts("[TEST] Allocating 8 contiguous pages...\n");
    uintptr_t contig = pmm_alloc_pages(8);
    uart_puts("  contig="); uart_puthex(contig); uart_puts("\n");
    pmm_free_pages(contig, 8);
    pmm_free_page(p1);
    pmm_free_page(p4);

    /* Heap: kmalloc / kfree */
    uart_puts("\n[TEST] kmalloc tests...\n");
    uint8_t *a = kmalloc(128);
    uint8_t *b = kmalloc(4096);
    uint8_t *c = kzalloc(256);

    uart_puts("  a="); uart_puthex((uintptr_t)a);
    uart_puts(" b="); uart_puthex((uintptr_t)b);
    uart_puts(" c="); uart_puthex((uintptr_t)c); uart_puts("\n");

    /* Verify kzalloc zeroed the memory */
    int zero_ok = 1;
    for (int i = 0; i < 256; i++) {
        if (c[i] != 0) { zero_ok = 0; break; }
    }
    uart_puts("  kzalloc zero check: ");
    uart_puts(zero_ok ? "PASS" : "FAIL");
    uart_puts("\n");

    /* Write and read back */
    for (int i = 0; i < 128; i++) a[i] = (uint8_t)i;
    int rw_ok = 1;
    for (int i = 0; i < 128; i++) {
        if (a[i] != (uint8_t)i) { rw_ok = 0; break; }
    }
    uart_puts("  heap R/W check: ");
    uart_puts(rw_ok ? "PASS" : "FAIL");
    uart_puts("\n");

    kmalloc_dump_stats();

    kfree(b);
    kfree(a);
    kfree(c);

    uart_puts("  After free:\n");
    kmalloc_dump_stats();

    uart_puts("\n[TEST] Memory management OK!\n\n");
}

/* User program binary — defined in user_prog.S */
extern char user_program_start[];
extern char user_program_end[];

/* User program binaries — defined in user_prog*.S */
extern char user_program_start[], user_program_end[];
extern char user_program2_start[], user_program2_end[];
extern char user_program3_start[], user_program3_end[];

static void demo_userspace(void) {
    uart_puts("--- multi-process user space demo (EL0) ---\n\n");

    /* MMU stays ON — user pages are mapped with AP=01 per-page */

    /* Initialize scheduler */
    sched_init();

    /* Create 3 user tasks */
    uint32_t s1 = (uint32_t)(user_program_end - user_program_start);
    uint32_t s2 = (uint32_t)(user_program2_end - user_program2_start);
    uint32_t s3 = (uint32_t)(user_program3_end - user_program3_start);

    sched_create_user("hello",     user_program_start,  s1);
    sched_create_user("fibonacci", user_program2_start, s2);
    sched_create_user("ticker",    user_program3_start, s3);

    uart_puts("\n[USER] Starting scheduler — 3 user processes (MMU ON)\n\n");

    while (sched_task_count() > 1) {
        int any_alive = 0;
        for (int i = 1; i < sched_task_count(); i++) {
            struct task *t = sched_get_task(i);
            if (t && t->state != TASK_FINISHED && t->state != TASK_UNUSED)
                any_alive = 1;
        }
        if (!any_alive) break;
        sched_yield();
    }

    uart_puts("\n[USER] All user tasks finished!\n");
    uart_puts("[USER] Demo complete.\n\n");
}

static void list_callback(const char *name, uint32_t size) {
    uart_puts("  ");
    uart_puts(name);
    uart_puts(" (");
    uart_putdec(size);
    uart_puts(" bytes)\n");
}

static void demo_filesystem(void) {
    uart_puts("--- filesystem + preemptive scheduling demo ---\n\n");

    /* Init block device */
    if (virtio_blk_init(&blk_dev) < 0) {
        uart_puts("SKIP: virtio-blk not available\n\n");
        return;
    }

    /* Mount filesystem */
    if (fat16_mount(&root_fs, &blk_dev) < 0) {
        uart_puts("[FS] Mount failed\n\n");
        return;
    }

    /* List files */
    uart_puts("\n[FS] Files on disk:\n");
    int nfiles = fat16_list_root(&root_fs, list_callback);
    uart_puts("[FS] ");
    uart_putdec((uint64_t)nfiles);
    uart_puts(" file(s) found\n\n");

    if (nfiles == 0) {
        uart_puts("[FS] No files to run\n\n");
        return;
    }

    /* Start preemptive timer — 50ms time slice */
    timer_init(1000);
    irq_register_timer();
    uart_puts("\n");

    /* Initialize scheduler and load programs from disk */
    sched_init();

    /* Load all .bin programs found on disk */
    struct fat16_file file;
    const char *programs[] = {"hello.bin", "fib.bin", "spin.bin", NULL};

    for (int i = 0; programs[i]; i++) {
        if (fat16_open(&root_fs, programs[i], &file) == 0) {
            void *code = kmalloc(file.file_size);
            if (code) {
                int bytes = fat16_read_file(&root_fs, &file, code, file.file_size);
                if (bytes > 0) {
                    uart_puts("[FS] Loaded ");
                    uart_puts(programs[i]);
                    uart_puts(" (");
                    uart_putdec((uint64_t)bytes);
                    uart_puts(" bytes)\n");
                    sched_create_user(programs[i], code, (uint32_t)bytes);
                }
                kfree(code);
            }
        }
    }

    uart_puts("\n[FS] Running programs with preemptive scheduling (50ms slice)...\n\n");

    /* Run all tasks — timer preemption handles switching */
    while (1) {
        int any_alive = 0;
        for (int i = 1; i < sched_task_count(); i++) {
            struct task *t = sched_get_task(i);
            if (t && t->state != TASK_FINISHED && t->state != TASK_UNUSED)
                any_alive = 1;
        }
        if (!any_alive) break;
        sched_yield();
    }

    /* Stop timer */
    timer_disable();

    /* Print task stats */
    uart_puts("\n[FS] All programs finished! Task stats:\n");
    for (int i = 1; i < sched_task_count(); i++) {
        struct task *t = sched_get_task(i);
        if (t) {
            uart_puts("  Task ");
            uart_putdec((uint64_t)i);
            uart_puts(" (\"");
            uart_puts(t->name);
            uart_puts("\"): ");
            uart_putdec(t->ticks);
            uart_puts(" timer ticks\n");
        }
    }
    uart_puts("\n");
}




extern uintptr_t __kernel_end;

extern uintptr_t __kernel_end;

extern void mmu_test(int use_el0_ap);
extern uintptr_t __kernel_end;

extern uintptr_t __kernel_end;

void kernel_main(void) {
    uart_init();
    uart_puts("\n==========================================\n");

    /* Memory management */
    pmm_init((uintptr_t)&__kernel_end);
    mmu_init();
    kmalloc_init();
    uart_puts("\n");

    gic_init();
    irq_init();

    irq_enable();
    uart_puts("[IRQ] CPU interrupts enabled\n\n");

    pci_enumerate();
    uart_puts("\n");

    demo_memory();
    demo_filesystem();

    // demo_userspace();
    // demo_rng();
    // demo_blk();
    // demo_net();
    // demo_gpu();
    // demo_input();
    // demo_sched();

    irq_disable();

    uart_puts("==============HALT !!!=============\n");

    for (;;)
        __asm__ volatile("wfe");
}
