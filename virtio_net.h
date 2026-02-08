#ifndef VIRTIO_NET_H
#define VIRTIO_NET_H

#include "pci.h"
#include "virtio_pci.h"
#include "virtqueue.h"

/*
 * Virtio network device (spec 5.1)
 *
 * Virtqueues:
 *   0: receiveq   — pre-posted device-writable buffers for incoming packets
 *   1: transmitq  — outgoing packets (device-readable)
 *   2: controlq   — if VIRTIO_NET_F_CTRL_VQ (we don't use it)
 *
 * Every packet on both queues is prefixed with virtio_net_hdr_v1.
 *
 * Device config (at device_cfg):
 *   0x00: uint8_t mac[6]
 *   0x06: uint16_t status
 *   0x08: uint16_t max_virtqueue_pairs
 *   0x0A: uint16_t mtu
 */

/* Feature bits (spec 5.1.3) */
#define VIRTIO_NET_F_CSUM           (1 << 0)
#define VIRTIO_NET_F_GUEST_CSUM     (1 << 1)
#define VIRTIO_NET_F_MAC            (1 << 5)
#define VIRTIO_NET_F_GSO            (1 << 6)
#define VIRTIO_NET_F_GUEST_TSO4     (1 << 7)
#define VIRTIO_NET_F_GUEST_TSO6     (1 << 8)
#define VIRTIO_NET_F_GUEST_ECN      (1 << 9)
#define VIRTIO_NET_F_GUEST_UFO      (1 << 10)
#define VIRTIO_NET_F_HOST_TSO4      (1 << 11)
#define VIRTIO_NET_F_HOST_TSO6      (1 << 12)
#define VIRTIO_NET_F_HOST_ECN       (1 << 13)
#define VIRTIO_NET_F_HOST_UFO       (1 << 14)
#define VIRTIO_NET_F_MRG_RXBUF      (1 << 15)
#define VIRTIO_NET_F_STATUS         (1 << 16)
#define VIRTIO_NET_F_CTRL_VQ        (1 << 17)
#define VIRTIO_NET_F_CTRL_RX        (1 << 18)
#define VIRTIO_NET_F_CTRL_VLAN      (1 << 19)
#define VIRTIO_NET_F_MQ             (1 << 22)

/* Net header flags */
#define VIRTIO_NET_HDR_F_NEEDS_CSUM 1

/* GSO types */
#define VIRTIO_NET_HDR_GSO_NONE     0

/*
 * Packet header prepended to every RX/TX buffer (spec 5.1.6).
 * With VIRTIO_F_VERSION_1, this is always the v1 layout (12 bytes).
 */
struct virtio_net_hdr_v1 {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
    uint16_t num_buffers;   /* only on RX with MRG_RXBUF, otherwise unused */
} __attribute__((packed));

#define VIRTIO_NET_HDR_SIZE sizeof(struct virtio_net_hdr_v1)

/* Max ethernet frame we handle */
#define NET_MTU         1514
#define NET_RX_BUF_SIZE (VIRTIO_NET_HDR_SIZE + NET_MTU + 2)

/* Number of pre-posted RX buffers */
#define NET_RX_RING_SIZE 16

struct virtio_net {
    struct pci_device     pci;
    struct virtio_pci_dev vpci;
    struct virtqueue      rxq;      /* receiveq  (queue 0) */
    struct virtqueue      txq;      /* transmitq (queue 1) */
    uint8_t               mac[6];
};

int virtio_net_init(struct virtio_net *dev);

/*
 * Transmit a raw ethernet frame (without virtio header — we prepend it).
 * frame points to a complete ethernet frame (dst + src + type + payload).
 * Returns 0 on success.
 */
int virtio_net_tx(struct virtio_net *dev, const void *frame, uint32_t len);

/*
 * Try to receive a packet. Non-blocking.
 * If a packet is available, copies the ethernet frame (without virtio header)
 * into buf and returns the frame length. Returns 0 if no packet available.
 * Re-posts the RX buffer after consuming.
 */
int virtio_net_rx(struct virtio_net *dev, void *buf, uint32_t buf_size);

/*
 * Re-fill any consumed RX buffers back onto the receiveq.
 */
void virtio_net_rx_refill(struct virtio_net *dev);

#endif
