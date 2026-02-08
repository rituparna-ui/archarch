#ifndef VIRTIO_GPU_H
#define VIRTIO_GPU_H

#include "pci.h"
#include "virtio_pci.h"
#include "virtqueue.h"

/*
 * Virtio GPU device (spec 5.7)
 *
 * Virtqueues:
 *   0: controlq  — 2D rendering commands (request/response pairs)
 *   1: cursorq   — cursor updates (we don't use it)
 *
 * Device ID: transitional 0x1050, modern 0x1040+16=0x1050
 *
 * Basic 2D rendering flow:
 *   1. RESOURCE_CREATE_2D   — create a host resource (R8G8B8A8)
 *   2. RESOURCE_ATTACH_BACKING — attach guest memory pages
 *   3. SET_SCANOUT           — bind resource to display scanout
 *   4. (draw into guest buffer)
 *   5. TRANSFER_TO_HOST_2D   — push dirty region to host
 *   6. RESOURCE_FLUSH         — tell host to display it
 */

/* PCI device IDs */
#define VIRTIO_PCI_DEVICE_GPU_TRANSITIONAL  0x1050
#define VIRTIO_PCI_DEVICE_GPU_MODERN        0x1050  /* 0x1040 + 16 */

/* Feature bits (spec 5.7.3) */
#define VIRTIO_GPU_F_VIRGL          (1 << 0)
#define VIRTIO_GPU_F_EDID           (1 << 1)

/* Control command types (spec 5.7.6.7) */
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO     0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D   0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF       0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT          0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH       0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D  0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106
#define VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING 0x0107
#define VIRTIO_GPU_CMD_GET_CAPSET_INFO      0x0108
#define VIRTIO_GPU_CMD_GET_CAPSET           0x0109
#define VIRTIO_GPU_CMD_GET_EDID             0x010A

/* Response types */
#define VIRTIO_GPU_RESP_OK_NODATA           0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO     0x1101
#define VIRTIO_GPU_RESP_OK_CAPSET_INFO      0x1102
#define VIRTIO_GPU_RESP_OK_CAPSET           0x1103
#define VIRTIO_GPU_RESP_OK_EDID             0x1104
#define VIRTIO_GPU_RESP_ERR_UNSPEC          0x1200
#define VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY   0x1201
#define VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID 0x1202
#define VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID 0x1203
#define VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID  0x1204
#define VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER   0x1205

/* Pixel formats */
#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM    1
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM    2
#define VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM    3
#define VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM    4
#define VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM    67
#define VIRTIO_GPU_FORMAT_X8B8G8R8_UNORM    68
#define VIRTIO_GPU_FORMAT_A8B8G8R8_UNORM    121
#define VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM    134

/* Max scanouts */
#define VIRTIO_GPU_MAX_SCANOUTS 16

/* Control header — every command starts with this */
struct virtio_gpu_ctrl_hdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t padding;
} __attribute__((packed));

/* Rectangle */
struct virtio_gpu_rect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} __attribute__((packed));

/* GET_DISPLAY_INFO response */
struct virtio_gpu_display_one {
    struct virtio_gpu_rect r;
    uint32_t enabled;
    uint32_t flags;
} __attribute__((packed));

struct virtio_gpu_resp_display_info {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_display_one pmodes[VIRTIO_GPU_MAX_SCANOUTS];
} __attribute__((packed));

/* RESOURCE_CREATE_2D */
struct virtio_gpu_resource_create_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} __attribute__((packed));

/* RESOURCE_ATTACH_BACKING */
struct virtio_gpu_mem_entry {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_resource_attach_backing {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
    /* followed by nr_entries virtio_gpu_mem_entry */
} __attribute__((packed));

/* SET_SCANOUT */
struct virtio_gpu_set_scanout {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t scanout_id;
    uint32_t resource_id;
} __attribute__((packed));

/* TRANSFER_TO_HOST_2D */
struct virtio_gpu_transfer_to_host_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

/* RESOURCE_FLUSH */
struct virtio_gpu_resource_flush {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

/* Display dimensions */
#define GPU_WIDTH   640
#define GPU_HEIGHT  480

struct virtio_gpu {
    struct pci_device     pci;
    struct virtio_pci_dev vpci;
    struct virtqueue      ctlq;     /* controlq (queue 0) */
    uint32_t              width;
    uint32_t              height;
};

int virtio_gpu_init(struct virtio_gpu *dev);

/*
 * Get the framebuffer pointer. The caller draws into this buffer
 * (GPU_WIDTH * GPU_HEIGHT * 4 bytes, R8G8B8A8).
 */
uint32_t *virtio_gpu_get_framebuffer(void);

/*
 * Flush a rectangular region to the display.
 * Performs TRANSFER_TO_HOST_2D + RESOURCE_FLUSH.
 * Returns 0 on success.
 */
int virtio_gpu_flush(struct virtio_gpu *dev,
                     uint32_t x, uint32_t y,
                     uint32_t w, uint32_t h);

#endif
