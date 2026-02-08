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
 *   - virtio-rng, virtio-blk, virtio-net, virtio-gpu, virtio-input
 *   - ARM Generic Timer + preemptive round-robin scheduler
 *   - Live kernel update via virtio-blk
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
#include "liveupdate.h"

static struct virtio_rng rng_dev;
static struct virtio_blk blk_dev;
static struct virtio_net net_dev;
static struct virtio_gpu gpu_dev;

/* Track whether blk_dev was initialized (needed for update polling) */
static int blk_available;

static uint8_t rng_buf[64] __attribute__((aligned(64)));
static uint8_t blk_buf[512] __attribute__((aligned(512)));
static uint8_t blk_readback[512] __attribute__((aligned(512)));

/* Network buffers */
static uint8_t tx_frame[1514] __attribute__((aligned(16)));
static uint8_t rx_frame[1514] __attribute__((aligned(16)));

static const char hexc[] = "0123456789abcdef";

/*
 * Kernel version string — change this to see live updates work!
 * When you rebuild and push a new image, this string changes in
 * the boot banner, proving the new kernel is running.
 */
#define KERNEL_VERSION "1.0.0"

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
    while (off < 60) frame[off++] = 0;
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
    frame[off++] = 0x45; frame[off++] = 0x00;
    int total_len_off = off; off += 2;
    write16be(frame + off, 0x1234); off += 2;
    write16be(frame + off, 0x0000); off += 2;
    frame[off++] = 64; frame[off++] = 1;
    int ip_csum_off = off; off += 2;
    memcpy8(frame + off, src_ip, 4); off += 4;
    memcpy8(frame + off, dst_ip, 4); off += 4;
    int icmp_start = off;
    frame[off++] = 8; frame[off++] = 0;
    int icmp_csum_off = off; off += 2;
    write16be(frame + off, 0x0001); off += 2;
    write16be(frame + off, seq);    off += 2;
    for (int i = 0; i < 32; i++)
        frame[off++] = (uint8_t)(0x41 + (i % 26));
    write16be(frame + total_len_off, (uint16_t)(off - ip_start));
    write16be(frame + ip_csum_off, 0);
    write16be(frame + ip_csum_off, ip_checksum(frame + ip_start, 20));
    int icmp_len = off - icmp_start;
    write16be(frame + icmp_csum_off, 0);
    write16be(frame + icmp_csum_off, ip_checksum(frame + icmp_start, icmp_len));
    return off;
}

static void print_ethertype(uint16_t et) {
    if (et == 0x0806) uart_puts("ARP");
    else if (et == 0x0800) uart_puts("IPv4");
    else if (et == 0x86dd) uart_puts("IPv6");
    else { uart_puts("0x"); uart_puthex(et); }
}

/* ---- Demo: virtio-rng ---- */
static void demo_rng(void) {
    uart_puts("--- virtio-rng demo (interrupt-driven) ---\n");
    if (virtio_rng_init(&rng_dev) < 0) {
        uart_puts("SKIP: virtio-rng not available\n\n");
        return;
    }
    uint32_t irq = pci_get_irq(&rng_dev.pci);
    if (irq) irq_register_rng(&rng_dev.vpci, irq);

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

/* ---- Demo: virtio-blk ---- */
static void demo_blk(void) {
    uart_puts("--- virtio-blk demo (interrupt-driven) ---\n");
    if (virtio_blk_init(&blk_dev) < 0) {
        uart_puts("SKIP: virtio-blk not available\n\n");
        return;
    }
    blk_available = 1;

    uint32_t irq = pci_get_irq(&blk_dev.pci);
    if (irq) irq_register_blk(&blk_dev.vpci, irq);

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
    for (int i = 0; i < 512; i++)
        if (blk_buf[i] != blk_readback[i]) { match = 0; break; }
    uart_puts(match ? "[BLK] VERIFY OK\n" : "[BLK] VERIFY FAILED\n");
    uart_puts("\n");
}

/* ---- Demo: virtio-net ---- */
static void demo_net(void) {
    uart_puts("--- virtio-net demo (interrupt-driven) ---\n");
    if (virtio_net_init(&net_dev) < 0) {
        uart_puts("SKIP: virtio-net not available\n\n");
        return;
    }
    uint32_t irq = pci_get_irq(&net_dev.pci);
    if (irq) irq_register_net(&net_dev.vpci, irq);

    uint8_t our_ip[] = {10,0,2,15}, gw_ip[] = {10,0,2,2};
    uint8_t gw_mac[6] = {0};

    uart_puts("[NET] Sending ARP request: who-has 10.0.2.2?\n");
    int arp_len = build_arp_request(tx_frame, net_dev.mac, our_ip, gw_ip);
    if (virtio_net_tx(&net_dev, tx_frame, (uint32_t)arp_len) < 0) {
        uart_puts("[NET] ARP TX failed\n"); return;
    }

    int got_arp = 0;
    irq_net_rx_pending = 0;
    for (uint64_t t = 0; t < 50000000 && !got_arp; t++) {
        int rxlen = virtio_net_rx(&net_dev, rx_frame, sizeof(rx_frame));
        if (rxlen <= 0) { wfi(); continue; }
        uint16_t ethertype = read16be(rx_frame + 12);
        if (ethertype == 0x0806 && read16be(rx_frame + 20) == 2) {
            memcpy8(gw_mac, rx_frame + 22, 6);
            uart_puts("[NET] ARP reply: 10.0.2.2 is at ");
            print_mac(gw_mac); uart_puts("\n");
            got_arp = 1;
        }
    }
    if (!got_arp) { uart_puts("[NET] No ARP reply\n"); memset8(gw_mac, 0xff, 6); }

    uart_puts("[NET] Sending ICMP echo to 10.0.2.2...\n");
    int ping_len = build_icmp_echo(tx_frame, net_dev.mac, gw_mac, our_ip, gw_ip, 1);
    if (virtio_net_tx(&net_dev, tx_frame, (uint32_t)ping_len) < 0) {
        uart_puts("[NET] Ping TX failed\n"); return;
    }

    int got_pong = 0;
    irq_net_rx_pending = 0;
    for (uint64_t t = 0; t < 50000000 && !got_pong; t++) {
        int rxlen = virtio_net_rx(&net_dev, rx_frame, sizeof(rx_frame));
        if (rxlen <= 0) { wfi(); continue; }
        uint16_t ethertype = read16be(rx_frame + 12);
        if (ethertype == 0x0800 && rxlen >= 34 && rx_frame[23] == 1 && rx_frame[34] == 0)
            got_pong = 1;
    }
    uart_puts(got_pong ? "[NET] PING SUCCESS\n" : "[NET] No echo reply\n");

    /* Drain */
    for (uint64_t t = 0; t < 5000000; t++)
        virtio_net_rx(&net_dev, rx_frame, sizeof(rx_frame));
    uart_puts("\n");
}

/* ---- Demo: virtio-gpu ---- */
static void demo_gpu(void) {
    uart_puts("--- virtio-gpu demo (2D framebuffer) ---\n");
    if (virtio_gpu_init(&gpu_dev) < 0) {
        uart_puts("SKIP: virtio-gpu not available\n\n");
        return;
    }
    uint32_t irq = pci_get_irq(&gpu_dev.pci);
    if (irq) irq_register_gpu(&gpu_dev.vpci, irq);

    uint32_t *fb = virtio_gpu_get_framebuffer();
    uint32_t w = gpu_dev.width, h = gpu_dev.height;
    uint32_t hw = w/2, hh = h/2;

    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint8_t i = (uint8_t)((x % hw) * 255 / hw);
            uint32_t p;
            if (y < hh) p = (x < hw) ? 0xFF000000|i : 0xFF000000|((uint32_t)i<<8);
            else p = (x < hw) ? 0xFF000000|((uint32_t)i<<16) : 0xFF000000|i|((uint32_t)i<<8)|((uint32_t)i<<16);
            fb[y*w+x] = p;
        }

    uint32_t rx = (w-160)/2, ry = (h-120)/2;
    for (uint32_t y = ry; y < ry+120; y++)
        for (uint32_t x = rx; x < rx+160; x++)
            fb[y*w+x] = 0xFF00FFFF;

    if (virtio_gpu_flush(&gpu_dev, 0, 0, w, h) == 0)
        uart_puts("[GPU] Test pattern displayed\n");

    for (volatile uint64_t d = 0; d < 600000000; d++) ;

    uint32_t colors[] = {0xFF0000FF,0xFF00FF00,0xFFFF0000,0xFF00FFFF,
                         0xFFFF00FF,0xFFFFFF00,0xFFFFFFFF,0xFF808080};
    uint32_t bar_h = h/8;
    for (uint32_t i = 0; i < 8; i++) {
        uint32_t y0 = i*bar_h, y1 = (i==7)?h:y0+bar_h;
        for (uint32_t y = y0; y < y1; y++)
            for (uint32_t x = 0; x < w; x++)
                fb[y*w+x] = colors[i];
    }
    if (virtio_gpu_flush(&gpu_dev, 0, 0, w, h) == 0)
        uart_puts("[GPU] Color bars displayed\n");
    uart_puts("\n");
}

/* ---- Demo: virtio-input ---- */
static void demo_input(void) {
    uart_puts("--- virtio-input demo (keyboard/mouse) ---\n");
    int count = virtio_input_init_all();
    if (count == 0) { uart_puts("SKIP: no input devices\n\n"); return; }

    for (int i = 0; i < count; i++) {
        struct virtio_input *dev = virtio_input_get_dev(i);
        uint32_t irq = pci_get_irq(&dev->pci);
        if (irq) irq_register_input(&dev->vpci, irq);
    }

    uart_puts("[INPUT] Listening (auto-exit timeout)...\n");
    int done = 0, evt_count = 0;
    uint64_t max_loops = 50000000;
    struct virtio_input_event evt;

    for (uint64_t lc = 0; !done && lc < max_loops; lc++) {
        if (virtio_input_poll(&evt)) {
            if (evt.type == EV_SYN) { evt_count++; continue; }
            evt_count++;
            if (evt.type == EV_KEY && evt.code == KEY_ESC && evt.value == 0)
                done = 1;
        }
    }
    uart_puts("[INPUT] ");
    uart_putdec((uint64_t)evt_count);
    uart_puts(" events\n\n");
}

/* ---- Demo: Timer + Preemptive Scheduler ---- */

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
        for (volatile uint64_t d = 0; d < 20000000; d++) ;
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
        uart_puts(" (t=");
        uart_putdec(timer_ms());
        uart_puts("ms)\n");
        uint64_t next = a + b; a = b; b = next;
        for (volatile uint64_t d = 0; d < 15000000; d++) ;
    }
}

static void task_dots(void *arg) {
    (void)arg;
    for (int i = 0; i < 20; i++) {
        uart_puts("  [DOTS] ");
        for (int j = 0; j <= i % 10; j++) uart_putc('.');
        uart_puts(" (t=");
        uart_putdec(timer_ms());
        uart_puts("ms)\n");
        for (volatile uint64_t d = 0; d < 10000000; d++) ;
    }
}

/*
 * Update poller task — runs as a scheduler task.
 * Periodically checks the disk for a new kernel image.
 * If found, triggers the live update sequence.
 */
static void task_update_poller(void *arg) {
    (void)arg;
    uart_puts("  [UPDATE-POLLER] Started, checking every ~2s\n");

    for (;;) {
        /* Busy-wait ~2 seconds between checks */
        for (volatile uint64_t d = 0; d < 100000000; d++) ;

        if (!blk_available)
            continue;

        if (liveupdate_check_disk(&blk_dev)) {
            uart_puts("\n  [UPDATE-POLLER] *** UPDATE DETECTED! ***\n");
            uart_puts("  [UPDATE-POLLER] Applying live update...\n\n");

            /* Apply the update — does not return */
            liveupdate_apply(&blk_dev, timer_ms(), irq_timer_ticks);
        }
    }
}

static void demo_sched(void) {
    uart_puts("--- timer + scheduler demo (preemptive round-robin) ---\n");

    timer_init(10);
    irq_register_timer();
    sched_init();

    sched_create("counter-A", task_counter, (void *)"A");
    sched_create("counter-B", task_counter, (void *)"B");
    sched_create("fibonacci", task_fibonacci, NULL);
    sched_create("dots",      task_dots,      NULL);
    sched_create("update-poller", task_update_poller, NULL);

    uart_puts("\n[SCHED] Starting tasks...\n\n");

    /* Idle loop */
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
    uart_puts("ms\n\n");
}

/* ---- Main entry point ---- */

void main(void) {
    uart_init();

    /* Check for live update handoff */
    struct handoff_data *handoff = liveupdate_check_handoff();

    uart_puts("\n==========================================\n");
    if (handoff) {
        uart_puts("  LIVE UPDATE — kernel v" KERNEL_VERSION "\n");
        uart_puts("  Generation: ");
        uart_putdec(handoff->generation);
        uart_puts("\n");
        uart_puts("  Previous uptime: ");
        uart_putdec(handoff->prev_uptime_ms);
        uart_puts("ms\n");
        uart_puts("  Total boots: ");
        uart_putdec(handoff->total_boots);
        uart_puts("\n");
        uart_puts("  Accumulated ticks: ");
        uart_putdec(handoff->total_ticks);
        uart_puts("\n");
        uart_puts("  Message: ");
        uart_puts(handoff->message);
        uart_puts("\n");
    } else {
        uart_puts("  AArch64 Bare Metal Virtio Demo v" KERNEL_VERSION "\n");
        uart_puts("  Cold boot — first run\n");
        liveupdate_cold_init();
    }
    uart_puts("  PCI ECAM / Virtio 1.x / GICv3 / Timer\n");
    uart_puts("  Live Update via virtio-blk\n");
    uart_puts("==========================================\n\n");

    /* Initialize GICv3 and interrupt dispatch */
    gic_init();
    irq_init();
    irq_enable();
    uart_puts("[IRQ] CPU interrupts enabled\n\n");

    pci_enumerate();
    uart_puts("\n");

    blk_available = 0;

    /* Init blk for update poller */
    demo_blk();
    demo_sched();

    irq_disable();

    uart_puts("==========================================\n");
    uart_puts("  All demos complete. System halted.\n");
    uart_puts("  To test live update:\n");
    uart_puts("    1. Edit KERNEL_VERSION in main.c\n");
    uart_puts("    2. make\n");
    uart_puts("    3. ./push_update.sh\n");
    uart_puts("==========================================\n");

    for (;;)
        __asm__ volatile("wfe");
}
