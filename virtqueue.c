/*
 * Split virtqueue implementation for virtio 1.x over PCI.
 * Supports multiple queue instances and multi-descriptor chains.
 */
#include "virtqueue.h"
#include "types.h"
#include "uart.h"

/*
 * Per-queue ring memory. We statically allocate for up to VIRTQ_MAX_QUEUES
 * queues, each with up to VIRTQ_MAX_SIZE entries.
 */
static struct vring_desc vq_descs[VIRTQ_MAX_QUEUES][VIRTQ_MAX_SIZE]
    __attribute__((aligned(16)));

static uint8_t vq_avail_bufs[VIRTQ_MAX_QUEUES][6 + 2 * VIRTQ_MAX_SIZE]
    __attribute__((aligned(2)));

static uint8_t vq_used_bufs[VIRTQ_MAX_QUEUES][6 + 8 * VIRTQ_MAX_SIZE]
    __attribute__((aligned(4)));

static int next_queue_slot = 0;

void virtqueue_init(struct virtqueue *vq, uint16_t num,
                    uint16_t queue_index, uintptr_t notify_addr)
{
    if (next_queue_slot >= VIRTQ_MAX_QUEUES) {
        uart_puts("[VQ] ERROR: too many queues!\n");
        return;
    }
    if (num > VIRTQ_MAX_SIZE)
        num = VIRTQ_MAX_SIZE;

    int slot = next_queue_slot++;

    vq->num = num;
    vq->desc  = vq_descs[slot];
    vq->avail = (struct vring_avail *)vq_avail_bufs[slot];
    vq->used  = (struct vring_used *)vq_used_bufs[slot];
    vq->notify_addr = notify_addr;
    vq->queue_index = queue_index;
    vq->free_head = 0;
    vq->last_used_idx = 0;
    vq->avail_idx = 0;

    /* Build free list */
    for (int i = 0; i < num; i++) {
        vq->desc[i].addr  = 0;
        vq->desc[i].len   = 0;
        vq->desc[i].flags = 0;
        vq->desc[i].next  = (uint16_t)(i + 1);
    }
    vq->desc[num - 1].next = 0xFFFF;

    vq->avail->flags = 0;
    vq->avail->idx   = 0;
    vq->used->flags  = 0;
    vq->used->idx    = 0;

    uart_puts("[VQ] Init q");
    uart_putdec(queue_index);
    uart_puts(": desc=");
    uart_puthex((uintptr_t)vq->desc);
    uart_puts(" avail=");
    uart_puthex((uintptr_t)vq->avail);
    uart_puts(" used=");
    uart_puthex((uintptr_t)vq->used);
    uart_puts(" size=");
    uart_putdec(num);
    uart_puts("\n");
}

uint16_t virtqueue_add_chain(struct virtqueue *vq,
                             struct vq_buf *bufs, int count)
{
    /* Check we have enough free descriptors */
    uint16_t idx = vq->free_head;
    for (int i = 0; i < count; i++) {
        if (idx == 0xFFFF) {
            uart_puts("[VQ] ERROR: not enough free descriptors\n");
            return 0xFFFF;
        }
        if (i < count - 1)
            idx = vq->desc[idx].next;
    }

    /* Fill the chain */
    uint16_t head = vq->free_head;
    idx = head;
    for (int i = 0; i < count; i++) {
        vq->desc[idx].addr  = (uint64_t)(uintptr_t)bufs[i].addr;
        vq->desc[idx].len   = bufs[i].len;
        vq->desc[idx].flags = bufs[i].flags;

        if (i < count - 1) {
            vq->desc[idx].flags |= VRING_DESC_F_NEXT;
            idx = vq->desc[idx].next;
        } else {
            /* Last descriptor: save next free, then terminate chain */
            vq->free_head = vq->desc[idx].next;
            vq->desc[idx].next = 0;
        }
    }

    /* Add head to available ring */
    uint16_t avail_slot = vq->avail_idx % vq->num;
    vq->avail->ring[avail_slot] = head;
    dmb();
    vq->avail_idx++;
    vq->avail->idx = vq->avail_idx;
    dmb();

    return head;
}

uint16_t virtqueue_add_buf_write(struct virtqueue *vq, void *buf, uint32_t len) {
    struct vq_buf b = { .addr = buf, .len = len, .flags = VRING_DESC_F_WRITE };
    return virtqueue_add_chain(vq, &b, 1);
}

void virtqueue_kick(struct virtqueue *vq) {
    dsb();
    mmio_write16(vq->notify_addr, vq->queue_index);
}

int virtqueue_get_used(struct virtqueue *vq, uint16_t *idx, uint32_t *len) {
    dmb();
    if (vq->last_used_idx == vq->used->idx)
        return 0;

    uint16_t slot = vq->last_used_idx % vq->num;
    uint16_t head = (uint16_t)vq->used->ring[slot].id;
    *idx = head;
    *len = vq->used->ring[slot].len;

    /* Walk the chain and return all descriptors to free list */
    uint16_t cur = head;
    while (1) {
        uint16_t next = vq->desc[cur].next;
        int has_next = vq->desc[cur].flags & VRING_DESC_F_NEXT;

        /* Return to free list */
        vq->desc[cur].next = vq->free_head;
        vq->free_head = cur;

        if (!has_next)
            break;
        cur = next;
    }

    vq->last_used_idx++;
    return 1;
}
