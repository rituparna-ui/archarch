#ifndef VIRTQUEUE_H
#define VIRTQUEUE_H

#include "types.h"

/*
 * Virtqueue structures per virtio spec 2.7
 * Split virtqueue layout.
 */

/* Descriptor flags */
#define VRING_DESC_F_NEXT       1
#define VRING_DESC_F_WRITE      2
#define VRING_DESC_F_INDIRECT   4

struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct vring_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
    /* followed by uint16_t used_event if VIRTIO_F_EVENT_IDX */
} __attribute__((packed));

struct vring_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct vring_used {
    uint16_t flags;
    uint16_t idx;
    struct vring_used_elem ring[];
    /* followed by uint16_t avail_event if VIRTIO_F_EVENT_IDX */
} __attribute__((packed));

#define VIRTQ_SIZE 16  /* small queue for RNG */

struct virtqueue {
    /* Descriptor table */
    struct vring_desc  *desc;
    /* Available ring */
    struct vring_avail *avail;
    /* Used ring */
    struct vring_used  *used;

    uint16_t num;           /* queue size */
    uint16_t free_head;     /* head of free descriptor list */
    uint16_t last_used_idx; /* last seen used index */
    uint16_t avail_idx;     /* next avail index to write */

    /* Notify address for this queue */
    uintptr_t notify_addr;
};

/*
 * Allocate and initialize a virtqueue.
 * Returns the physical addresses for desc, avail, used tables.
 */
void virtqueue_init(struct virtqueue *vq, uint16_t num, uintptr_t notify_addr);

/*
 * Add a single device-writable buffer to the queue.
 * Returns the descriptor index used.
 */
uint16_t virtqueue_add_buf_write(struct virtqueue *vq, void *buf, uint32_t len);

/*
 * Kick the device (write to notify register).
 */
void virtqueue_kick(struct virtqueue *vq);

/*
 * Check if there's a used buffer. If so, return 1 and fill *len with bytes written.
 */
int virtqueue_get_used(struct virtqueue *vq, uint16_t *idx, uint32_t *len);

#endif
