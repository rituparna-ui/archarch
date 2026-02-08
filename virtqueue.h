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
} __attribute__((packed));

struct vring_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct vring_used {
    uint16_t flags;
    uint16_t idx;
    struct vring_used_elem ring[];
} __attribute__((packed));

/*
 * Max queue size we support. Each virtqueue instance gets its own
 * statically-allocated ring memory via virtqueue_alloc().
 */
#define VIRTQ_MAX_SIZE 32

struct virtqueue {
    struct vring_desc  *desc;
    struct vring_avail *avail;
    struct vring_used  *used;

    uint16_t num;
    uint16_t free_head;
    uint16_t last_used_idx;
    uint16_t avail_idx;
    uint16_t queue_index;     /* which queue on the device (for kick) */
    uintptr_t notify_addr;
};

/*
 * Allocate ring memory for a virtqueue and initialize it.
 * Supports up to VIRTQ_MAX_QUEUES concurrent queues.
 */
#define VIRTQ_MAX_QUEUES 4
void virtqueue_init(struct virtqueue *vq, uint16_t num,
                    uint16_t queue_index, uintptr_t notify_addr);

/*
 * Descriptor for building chains.
 */
struct vq_buf {
    void     *addr;
    uint32_t  len;
    uint16_t  flags;  /* 0 = device-readable, VRING_DESC_F_WRITE = device-writable */
};

/*
 * Add a chain of buffers to the queue.
 * Returns the head descriptor index, or 0xFFFF on failure.
 */
uint16_t virtqueue_add_chain(struct virtqueue *vq,
                             struct vq_buf *bufs, int count);

/*
 * Convenience: add a single device-writable buffer (for RNG etc).
 */
uint16_t virtqueue_add_buf_write(struct virtqueue *vq, void *buf, uint32_t len);

/*
 * Kick the device.
 */
void virtqueue_kick(struct virtqueue *vq);

/*
 * Poll for a used buffer. Returns 1 if found, fills idx and len.
 * Frees the entire descriptor chain back to the free list.
 */
int virtqueue_get_used(struct virtqueue *vq, uint16_t *idx, uint32_t *len);

#endif
