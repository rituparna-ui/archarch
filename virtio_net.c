/*
 * Virtio network device driver over PCI (spec 5.1).
 *
 * Implements:
 *   - receiveq (queue 0): pre-posted device-writable buffers
 *   - transmitq (queue 1): device-readable outgoing packets
 *   - Each buffer prefixed with virtio_net_hdr_v1 (12 bytes)
 *   - MAC address read from device config
 *   - No control queue, no multiqueue, no offloads
 */
#include "virtio_net.h"
#include "uart.h"

/* Device config offsets (spec 5.1.4) */
#define NET_CFG_MAC         0x00
#define NET_CFG_STATUS      0x06

/*
 * Static RX buffer pool.
 * Each buffer: virtio_net_hdr_v1 + ethernet frame.
 * We map descriptor index -> buffer slot via desc_to_rxbuf[].
 */
static uint8_t rx_buffers[NET_RX_RING_SIZE][NET_RX_BUF_SIZE]
    __attribute__((aligned(16)));

/* Maps descriptor index -> rx_buffers slot */
static int desc_to_rxbuf[VIRTQ_MAX_SIZE];
static int next_rx_slot;

/* TX header (one TX at a time) */
static struct virtio_net_hdr_v1 tx_hdr __attribute__((aligned(16)));

static void post_rx_buffer(struct virtio_net *dev, int slot) {
    struct vq_buf buf = {
        .addr  = rx_buffers[slot],
        .len   = NET_RX_BUF_SIZE,
        .flags = VRING_DESC_F_WRITE
    };
    uint16_t desc_idx = virtqueue_add_chain(&dev->rxq, &buf, 1);
    if (desc_idx != 0xFFFF)
        desc_to_rxbuf[desc_idx] = slot;
}

static void fill_rx_queue(struct virtio_net *dev) {
    next_rx_slot = 0;
    int count = NET_RX_RING_SIZE;
    if (count > dev->rxq.num)
        count = dev->rxq.num;

    for (int i = 0; i < count; i++) {
        post_rx_buffer(dev, i);
        next_rx_slot = i + 1;
    }
    virtqueue_kick(&dev->rxq);

    uart_puts("  Posted ");
    uart_putdec((uint64_t)count);
    uart_puts(" RX buffers\n");
}

int virtio_net_init(struct virtio_net *dev) {
    uart_puts("\n[VIRTIO-NET] === Init ===\n");

    if (!pci_find_virtio_net(&dev->pci)) {
        uart_puts("[VIRTIO-NET] Device not found\n");
        return -1;
    }

    if (virtio_pci_parse_caps(&dev->pci, &dev->vpci) < 0)
        return -1;

    /*
     * Feature negotiation:
     *   - Accept MAC to read device MAC address
     *   - Accept STATUS to read link status
     *   - Don't accept MRG_RXBUF (one buffer = one packet, simpler)
     *   - No offloads, no control queue, no multiqueue
     */
    uint32_t want_features = VIRTIO_NET_F_MAC | VIRTIO_NET_F_STATUS;

    if (virtio_pci_init_device(&dev->pci, &dev->vpci, want_features, 0) < 0)
        return -1;

    /* Read MAC address from device config */
    uintptr_t dcfg = dev->vpci.device_cfg;
    if (dcfg == 0) {
        uart_puts("[VIRTIO-NET] No device config!\n");
        return -1;
    }

    for (int i = 0; i < 6; i++)
        dev->mac[i] = mmio_read8(dcfg + NET_CFG_MAC + (uintptr_t)i);

    uart_puts("  MAC: ");
    const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 6; i++) {
        uart_putc(hex[dev->mac[i] >> 4]);
        uart_putc(hex[dev->mac[i] & 0xf]);
        if (i < 5) uart_putc(':');
    }
    uart_puts("\n");

    uint16_t status = mmio_read16(dcfg + NET_CFG_STATUS);
    uart_puts("  Link: ");
    uart_puts((status & 1) ? "UP" : "DOWN");
    uart_puts("\n");

    /* Setup receiveq (queue 0) and transmitq (queue 1) */
    if (virtio_pci_setup_queue(&dev->vpci, &dev->rxq, 0, 256) < 0)
        return -1;
    if (virtio_pci_setup_queue(&dev->vpci, &dev->txq, 1, 256) < 0)
        return -1;

    virtio_pci_set_driver_ok(&dev->vpci);

    /* Pre-fill RX queue */
    fill_rx_queue(dev);

    uart_puts("[VIRTIO-NET] === Ready ===\n\n");
    return 0;
}

int virtio_net_tx(struct virtio_net *dev, const void *frame, uint32_t len) {
    /* Plain packet, no offloads */
    tx_hdr.flags       = 0;
    tx_hdr.gso_type    = VIRTIO_NET_HDR_GSO_NONE;
    tx_hdr.hdr_len     = 0;
    tx_hdr.gso_size    = 0;
    tx_hdr.csum_start  = 0;
    tx_hdr.csum_offset = 0;
    tx_hdr.num_buffers = 0;

    /*
     * 2-descriptor chain (both device-readable):
     *   [0] virtio_net_hdr_v1
     *   [1] raw ethernet frame
     */
    struct vq_buf chain[2];
    chain[0].addr  = &tx_hdr;
    chain[0].len   = VIRTIO_NET_HDR_SIZE;
    chain[0].flags = 0;

    chain[1].addr  = (void *)frame;
    chain[1].len   = len;
    chain[1].flags = 0;

    uint16_t head = virtqueue_add_chain(&dev->txq, chain, 2);
    if (head == 0xFFFF)
        return -1;

    virtqueue_kick(&dev->txq);

    /* Poll for TX completion */
    uint16_t used_idx;
    uint32_t written;
    uint64_t timeout = 50000000;
    while (!virtqueue_get_used(&dev->txq, &used_idx, &written)) {
        if (--timeout == 0) {
            uart_puts("[NET] TX timeout!\n");
            return -1;
        }
    }

    return 0;
}

int virtio_net_rx(struct virtio_net *dev, void *buf, uint32_t buf_size) {
    uint16_t desc_idx;
    uint32_t total_len;

    if (!virtqueue_get_used(&dev->rxq, &desc_idx, &total_len))
        return 0;

    /* Look up which rx_buffers[] slot this descriptor was using */
    int slot = desc_to_rxbuf[desc_idx];
    uint8_t *rx_buf = rx_buffers[slot];

    int frame_len = 0;
    if (total_len > VIRTIO_NET_HDR_SIZE) {
        frame_len = (int)(total_len - VIRTIO_NET_HDR_SIZE);
        if ((uint32_t)frame_len > buf_size)
            frame_len = (int)buf_size;

        uint8_t *src = rx_buf + VIRTIO_NET_HDR_SIZE;
        uint8_t *dst = (uint8_t *)buf;
        for (int i = 0; i < frame_len; i++)
            dst[i] = src[i];
    }

    /* Re-post this buffer for future receives */
    post_rx_buffer(dev, slot);
    virtqueue_kick(&dev->rxq);

    return frame_len;
}

void virtio_net_rx_refill(struct virtio_net *dev) {
    (void)dev;
}
