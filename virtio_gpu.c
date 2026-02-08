/*
 * Virtio GPU 2D driver over PCI (spec 5.7).
 *
 * Implements basic 2D scanout:
 *   - GET_DISPLAY_INFO to query native resolution
 *   - RESOURCE_CREATE_2D (R8G8B8A8, 640x480)
 *   - RESOURCE_ATTACH_BACKING (single contiguous guest buffer)
 *   - SET_SCANOUT (bind resource to scanout 0)
 *   - TRANSFER_TO_HOST_2D + RESOURCE_FLUSH for display updates
 *
 * The controlq uses 2-descriptor chains: request (device-readable)
 * followed by response (device-writable).
 */
#include "virtio_gpu.h"
#include "uart.h"

/* Static framebuffer: 640x480 @ 32bpp = 1,228,800 bytes */
static uint32_t framebuffer[GPU_WIDTH * GPU_HEIGHT]
    __attribute__((aligned(4096)));

/* Resource ID for our scanout framebuffer */
#define FB_RESOURCE_ID 1

/*
 * Command/response buffers — one command in flight at a time.
 * We use a union to hold the largest possible request/response.
 */
static uint8_t cmd_buf[256] __attribute__((aligned(16)));
static uint8_t resp_buf[4096] __attribute__((aligned(16)));

static void gpu_ctrl_hdr_init(struct virtio_gpu_ctrl_hdr *hdr, uint32_t type) {
    hdr->type     = type;
    hdr->flags    = 0;
    hdr->fence_id = 0;
    hdr->ctx_id   = 0;
    hdr->padding  = 0;
}

/*
 * Send a command on the controlq and wait for the response.
 * cmd_len: size of the request in cmd_buf
 * resp_len: size of the response buffer
 * Returns the response type, or 0 on timeout.
 */
static uint32_t gpu_send_cmd(struct virtio_gpu *dev,
                             uint32_t cmd_len, uint32_t resp_len)
{
    /* Clear response */
    for (uint32_t i = 0; i < resp_len && i < sizeof(resp_buf); i++)
        resp_buf[i] = 0;

    struct vq_buf chain[2];

    /* Descriptor 0: command (device-readable) */
    chain[0].addr  = cmd_buf;
    chain[0].len   = cmd_len;
    chain[0].flags = 0;

    /* Descriptor 1: response (device-writable) */
    chain[1].addr  = resp_buf;
    chain[1].len   = resp_len;
    chain[1].flags = VRING_DESC_F_WRITE;

    uint16_t head = virtqueue_add_chain(&dev->ctlq, chain, 2);
    if (head == 0xFFFF) {
        uart_puts("[GPU] Failed to add command to queue\n");
        return 0;
    }

    virtqueue_kick(&dev->ctlq);

    /* Poll for completion */
    uint16_t used_idx;
    uint32_t written;
    uint64_t timeout = 50000000;
    while (!virtqueue_get_used(&dev->ctlq, &used_idx, &written)) {
        if (--timeout == 0) {
            uart_puts("[GPU] Command timeout!\n");
            return 0;
        }
    }

    struct virtio_gpu_ctrl_hdr *resp_hdr = (struct virtio_gpu_ctrl_hdr *)resp_buf;
    return resp_hdr->type;
}

static int gpu_get_display_info(struct virtio_gpu *dev) {
    uart_puts("[GPU] GET_DISPLAY_INFO\n");

    struct virtio_gpu_ctrl_hdr *hdr = (struct virtio_gpu_ctrl_hdr *)cmd_buf;
    gpu_ctrl_hdr_init(hdr, VIRTIO_GPU_CMD_GET_DISPLAY_INFO);

    uint32_t resp_type = gpu_send_cmd(dev,
        sizeof(struct virtio_gpu_ctrl_hdr),
        sizeof(struct virtio_gpu_resp_display_info));

    if (resp_type != VIRTIO_GPU_RESP_OK_DISPLAY_INFO) {
        uart_puts("[GPU] GET_DISPLAY_INFO failed, resp=");
        uart_puthex(resp_type);
        uart_puts("\n");
        return -1;
    }

    struct virtio_gpu_resp_display_info *info =
        (struct virtio_gpu_resp_display_info *)resp_buf;

    for (int i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; i++) {
        if (info->pmodes[i].enabled) {
            uart_puts("  Scanout ");
            uart_putdec((uint64_t)i);
            uart_puts(": ");
            uart_putdec(info->pmodes[i].r.width);
            uart_puts("x");
            uart_putdec(info->pmodes[i].r.height);
            uart_puts(" enabled\n");
        }
    }

    return 0;
}

static int gpu_create_resource(struct virtio_gpu *dev) {
    uart_puts("[GPU] RESOURCE_CREATE_2D: ");
    uart_putdec(GPU_WIDTH);
    uart_puts("x");
    uart_putdec(GPU_HEIGHT);
    uart_puts(" R8G8B8A8\n");

    struct virtio_gpu_resource_create_2d *cmd =
        (struct virtio_gpu_resource_create_2d *)cmd_buf;
    gpu_ctrl_hdr_init(&cmd->hdr, VIRTIO_GPU_CMD_RESOURCE_CREATE_2D);
    cmd->resource_id = FB_RESOURCE_ID;
    cmd->format      = VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM;
    cmd->width       = GPU_WIDTH;
    cmd->height      = GPU_HEIGHT;

    uint32_t resp_type = gpu_send_cmd(dev,
        sizeof(struct virtio_gpu_resource_create_2d),
        sizeof(struct virtio_gpu_ctrl_hdr));

    if (resp_type != VIRTIO_GPU_RESP_OK_NODATA) {
        uart_puts("[GPU] RESOURCE_CREATE_2D failed, resp=");
        uart_puthex(resp_type);
        uart_puts("\n");
        return -1;
    }
    uart_puts("  OK\n");
    return 0;
}

static int gpu_attach_backing(struct virtio_gpu *dev) {
    uart_puts("[GPU] RESOURCE_ATTACH_BACKING\n");

    /*
     * We need to send the attach_backing header followed by
     * one mem_entry, all in a single device-readable descriptor.
     * Pack them contiguously in cmd_buf.
     */
    struct virtio_gpu_resource_attach_backing *cmd =
        (struct virtio_gpu_resource_attach_backing *)cmd_buf;
    gpu_ctrl_hdr_init(&cmd->hdr, VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING);
    cmd->resource_id = FB_RESOURCE_ID;
    cmd->nr_entries  = 1;

    /* mem_entry follows immediately after the header */
    struct virtio_gpu_mem_entry *entry =
        (struct virtio_gpu_mem_entry *)(cmd_buf +
            sizeof(struct virtio_gpu_resource_attach_backing));
    entry->addr    = (uint64_t)(uintptr_t)framebuffer;
    entry->length  = GPU_WIDTH * GPU_HEIGHT * 4;
    entry->padding = 0;

    uint32_t cmd_len = sizeof(struct virtio_gpu_resource_attach_backing) +
                       sizeof(struct virtio_gpu_mem_entry);

    uint32_t resp_type = gpu_send_cmd(dev, cmd_len,
        sizeof(struct virtio_gpu_ctrl_hdr));

    if (resp_type != VIRTIO_GPU_RESP_OK_NODATA) {
        uart_puts("[GPU] ATTACH_BACKING failed, resp=");
        uart_puthex(resp_type);
        uart_puts("\n");
        return -1;
    }
    uart_puts("  OK (fb=");
    uart_puthex((uintptr_t)framebuffer);
    uart_puts(" size=");
    uart_putdec((uint64_t)(GPU_WIDTH * GPU_HEIGHT * 4));
    uart_puts(")\n");
    return 0;
}

static int gpu_set_scanout(struct virtio_gpu *dev) {
    uart_puts("[GPU] SET_SCANOUT\n");

    struct virtio_gpu_set_scanout *cmd =
        (struct virtio_gpu_set_scanout *)cmd_buf;
    gpu_ctrl_hdr_init(&cmd->hdr, VIRTIO_GPU_CMD_SET_SCANOUT);
    cmd->r.x         = 0;
    cmd->r.y         = 0;
    cmd->r.width     = GPU_WIDTH;
    cmd->r.height    = GPU_HEIGHT;
    cmd->scanout_id  = 0;
    cmd->resource_id = FB_RESOURCE_ID;

    uint32_t resp_type = gpu_send_cmd(dev,
        sizeof(struct virtio_gpu_set_scanout),
        sizeof(struct virtio_gpu_ctrl_hdr));

    if (resp_type != VIRTIO_GPU_RESP_OK_NODATA) {
        uart_puts("[GPU] SET_SCANOUT failed, resp=");
        uart_puthex(resp_type);
        uart_puts("\n");
        return -1;
    }
    uart_puts("  OK\n");
    return 0;
}

/* ---- Public API ---- */

int virtio_gpu_init(struct virtio_gpu *dev) {
    uart_puts("\n[VIRTIO-GPU] === Init ===\n");

    if (!pci_find_virtio_gpu(&dev->pci)) {
        uart_puts("[VIRTIO-GPU] Device not found\n");
        return -1;
    }

    if (virtio_pci_parse_caps(&dev->pci, &dev->vpci) < 0)
        return -1;

    /* No device-specific features needed for basic 2D */
    if (virtio_pci_init_device(&dev->pci, &dev->vpci, 0, 0) < 0)
        return -1;

    /* Setup controlq (queue 0) */
    if (virtio_pci_setup_queue(&dev->vpci, &dev->ctlq, 0, 64) < 0)
        return -1;

    virtio_pci_set_driver_ok(&dev->vpci);

    dev->width  = GPU_WIDTH;
    dev->height = GPU_HEIGHT;

    /* Clear framebuffer to black */
    for (uint32_t i = 0; i < GPU_WIDTH * GPU_HEIGHT; i++)
        framebuffer[i] = 0xFF000000; /* opaque black (RGBA) */

    /* Query display info */
    if (gpu_get_display_info(dev) < 0)
        return -1;

    /* Create 2D resource */
    if (gpu_create_resource(dev) < 0)
        return -1;

    /* Attach guest framebuffer memory */
    if (gpu_attach_backing(dev) < 0)
        return -1;

    /* Bind to scanout 0 */
    if (gpu_set_scanout(dev) < 0)
        return -1;

    uart_puts("[VIRTIO-GPU] === Ready ===\n\n");
    return 0;
}

uint32_t *virtio_gpu_get_framebuffer(void) {
    return framebuffer;
}

int virtio_gpu_flush(struct virtio_gpu *dev,
                     uint32_t x, uint32_t y,
                     uint32_t w, uint32_t h)
{
    /* Step 1: TRANSFER_TO_HOST_2D — push pixels from guest to host */
    struct virtio_gpu_transfer_to_host_2d *xfer =
        (struct virtio_gpu_transfer_to_host_2d *)cmd_buf;
    gpu_ctrl_hdr_init(&xfer->hdr, VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D);
    xfer->r.x         = x;
    xfer->r.y         = y;
    xfer->r.width     = w;
    xfer->r.height    = h;
    xfer->offset       = (uint64_t)(y * GPU_WIDTH + x) * 4;
    xfer->resource_id  = FB_RESOURCE_ID;
    xfer->padding      = 0;

    uint32_t resp_type = gpu_send_cmd(dev,
        sizeof(struct virtio_gpu_transfer_to_host_2d),
        sizeof(struct virtio_gpu_ctrl_hdr));

    if (resp_type != VIRTIO_GPU_RESP_OK_NODATA) {
        uart_puts("[GPU] TRANSFER_TO_HOST_2D failed, resp=");
        uart_puthex(resp_type);
        uart_puts("\n");
        return -1;
    }

    /* Step 2: RESOURCE_FLUSH — tell host to display it */
    struct virtio_gpu_resource_flush *flush =
        (struct virtio_gpu_resource_flush *)cmd_buf;
    gpu_ctrl_hdr_init(&flush->hdr, VIRTIO_GPU_CMD_RESOURCE_FLUSH);
    flush->r.x         = x;
    flush->r.y         = y;
    flush->r.width     = w;
    flush->r.height    = h;
    flush->resource_id = FB_RESOURCE_ID;
    flush->padding     = 0;

    resp_type = gpu_send_cmd(dev,
        sizeof(struct virtio_gpu_resource_flush),
        sizeof(struct virtio_gpu_ctrl_hdr));

    if (resp_type != VIRTIO_GPU_RESP_OK_NODATA) {
        uart_puts("[GPU] RESOURCE_FLUSH failed, resp=");
        uart_puthex(resp_type);
        uart_puts("\n");
        return -1;
    }

    return 0;
}
