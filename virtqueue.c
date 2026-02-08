/*
 * Split virtqueue implementation for virtio 1.x over PCI.
 */
#include "virtqueue.h"
#include "types.h"
#include "uart.h"

/*
 * Static memory for the virtqueue rings.
 * Must be physically contiguous and aligned.
 * We use a simple static allocation since we only need one queue.
 */
static struct vring_desc  vq_desc[VIRTQ_SIZE]  __attribute__((aligned(16)));
static uint8_t            vq_avail_buf[6 + 2 * VIRTQ_SIZE] __attribute__((aligned(2)));
static uint8_t            vq_used_buf[6 + 8 * VIRTQ_SIZE]  __attribute__((aligned(4)));

void virtqueue_init(struct virtqueue *vq, uint16_t num, uintptr_t notify_addr) {
    vq->num = num;
    vq->desc  = vq_desc;
    vq->avail = (struct vring_avail *)vq_avail_buf;
    vq->used  = (struct vring_used *)vq_used_buf;
    vq->notify_addr = notify_addr;
    vq->free_head = 0;
    vq->last_used_idx = 0;
    vq->avail_idx = 0;

    /* Zero out */
    for (int i = 0; i < num; i++) {
        vq->desc[i].addr  = 0;
        vq->desc[i].len   = 0;
        vq->desc[i].flags = 0;
        vq->desc[i].next  = (uint16_t)(i + 1);
    }
    vq->desc[num - 1].next = 0xFFFF; /* end of free list */

    vq->avail->flags = 0;
    vq->avail->idx   = 0;
    vq->used->flags  = 0;
    vq->used->idx    = 0;

    uart_puts("[VQ] Initialized: desc=");
    uart_puthex((uintptr_t)vq->desc);
    uart_puts(" avail=");
    uart_puthex((uintptr_t)vq->avail);
    uart_puts(" used=");
    uart_puthex((uintptr_t)vq->used);
    uart_puts("\n");
}

uint16_t virtqueue_add_buf_write(struct virtqueue *vq, void *buf, uint32_t len) {
    uint16_t idx = vq->free_head;
    if (idx == 0xFFFF) {
        uart_puts("[VQ] ERROR: no free descriptors!\n");
        return 0xFFFF;
    }

    vq->free_head = vq->desc[idx].next;

    vq->desc[idx].addr  = (uint64_t)(uintptr_t)buf;
    vq->desc[idx].len   = len;
    vq->desc[idx].flags = VRING_DESC_F_WRITE; /* device writes to this buffer */
    vq->desc[idx].next  = 0;

    /* Add to available ring */
    uint16_t avail_slot = vq->avail_idx % vq->num;
    vq->avail->ring[avail_slot] = idx;
    dmb();
    vq->avail_idx++;
    vq->avail->idx = vq->avail_idx;
    dmb();

    return idx;
}

void virtqueue_kick(struct virtqueue *vq) {
    dsb();
    /* Write queue index (0) to the notify register */
    mmio_write16(vq->notify_addr, 0);
}

int virtqueue_get_used(struct virtqueue *vq, uint16_t *idx, uint32_t *len) {
    dmb();
    if (vq->last_used_idx == vq->used->idx)
        return 0;

    uint16_t slot = vq->last_used_idx % vq->num;
    *idx = (uint16_t)vq->used->ring[slot].id;
    *len = vq->used->ring[slot].len;

    /* Return descriptor to free list */
    vq->desc[*idx].next = vq->free_head;
    vq->free_head = *idx;

    vq->last_used_idx++;
    return 1;
}
