/*
 * Virtio input device driver over PCI (spec 5.8).
 *
 * QEMU creates separate virtio-input-pci devices for keyboard and
 * mouse/tablet. We scan the PCI bus for all of them, initialize each,
 * and provide a unified polling API.
 *
 * The eventq carries Linux evdev events: {type, code, value}.
 * We pre-post device-writable buffers and consume them as events arrive.
 */
#include "virtio_input.h"
#include "uart.h"

static struct virtio_input input_devs[MAX_INPUT_DEVS];
static int input_dev_count;

/* Per-device event buffer pools */
static struct virtio_input_event
    evt_buffers[MAX_INPUT_DEVS][INPUT_EVT_RING_SIZE]
    __attribute__((aligned(16)));

/* Maps descriptor index -> evt_buffers slot for each device */
static int desc_to_evt[MAX_INPUT_DEVS][VIRTQ_MAX_SIZE];

static void post_evt_buffer(struct virtio_input *dev, int dev_idx, int slot) {
    struct vq_buf buf = {
        .addr  = &evt_buffers[dev_idx][slot],
        .len   = INPUT_EVENT_SIZE,
        .flags = VRING_DESC_F_WRITE
    };
    uint16_t desc_idx = virtqueue_add_chain(&dev->evtq, &buf, 1);
    if (desc_idx != 0xFFFF)
        desc_to_evt[dev_idx][desc_idx] = slot;
}

static void fill_evt_queue(struct virtio_input *dev, int dev_idx) {
    int count = INPUT_EVT_RING_SIZE;
    if (count > dev->evtq.num)
        count = dev->evtq.num;

    for (int i = 0; i < count; i++)
        post_evt_buffer(dev, dev_idx, i);
    virtqueue_kick(&dev->evtq);

    uart_puts("  Posted ");
    uart_putdec((uint64_t)count);
    uart_puts(" event buffers\n");
}

/*
 * Query device config by writing select+subsel and reading back.
 */
static int input_cfg_query(struct virtio_input *dev, uint8_t select,
                           uint8_t subsel, uint8_t *data, int max_len)
{
    uintptr_t dcfg = dev->vpci.device_cfg;
    if (dcfg == 0)
        return 0;

    mmio_write8(dcfg + VIRTIO_INPUT_CFG_SELECT_OFF, select);
    mmio_write8(dcfg + VIRTIO_INPUT_CFG_SUBSEL_OFF, subsel);
    dsb();

    uint8_t size = mmio_read8(dcfg + VIRTIO_INPUT_CFG_SIZE_OFF);
    if (size == 0)
        return 0;

    int len = (int)size;
    if (len > max_len)
        len = max_len;

    for (int i = 0; i < len; i++)
        data[i] = mmio_read8(dcfg + VIRTIO_INPUT_CFG_DATA_OFF + (uintptr_t)i);

    return len;
}

static void read_device_name(struct virtio_input *dev) {
    int len = input_cfg_query(dev, VIRTIO_INPUT_CFG_ID_NAME, 0,
                              (uint8_t *)dev->name, 63);
    dev->name[len] = '\0';
    if (len == 0) {
        dev->name[0] = '?';
        dev->name[1] = '\0';
    }
}

static void detect_device_type(struct virtio_input *dev) {
    uint8_t bits[32];

    dev->is_keyboard = 0;
    dev->is_mouse = 0;

    /* Check if device supports EV_KEY events */
    int len = input_cfg_query(dev, VIRTIO_INPUT_CFG_EV_BITS, EV_KEY,
                              bits, sizeof(bits));
    if (len > 0) {
        /* If it has key events and supports KEY_A (byte 3, bit 6),
         * it's likely a keyboard */
        if (len > 3 && (bits[3] & (1 << 6)))  /* KEY_A = 30 -> byte 3, bit 6 */
            dev->is_keyboard = 1;
    }

    /* Check if device supports EV_REL events (relative mouse) */
    len = input_cfg_query(dev, VIRTIO_INPUT_CFG_EV_BITS, EV_REL,
                          bits, sizeof(bits));
    if (len > 0 && (bits[0] & 0x03))  /* REL_X=0, REL_Y=1 */
        dev->is_mouse = 1;

    /* Check if device supports EV_ABS events (tablet/touchscreen) */
    if (!dev->is_mouse) {
        len = input_cfg_query(dev, VIRTIO_INPUT_CFG_EV_BITS, EV_ABS,
                              bits, sizeof(bits));
        if (len > 0 && (bits[0] & 0x03))  /* ABS_X=0, ABS_Y=1 */
            dev->is_mouse = 1;  /* treat tablet as mouse-like */
    }
}

static int find_and_init_input(int start_slot) {
    for (uint8_t dev = (uint8_t)start_slot; dev < 32; dev++) {
        uint16_t vendor = pci_config_read16(0, dev, 0, PCI_VENDOR_ID);
        if (vendor != VIRTIO_PCI_VENDOR)
            continue;

        uint16_t device = pci_config_read16(0, dev, 0, PCI_DEVICE_ID);
        if (device != VIRTIO_PCI_DEVICE_INPUT_TRANSITIONAL &&
            device != VIRTIO_PCI_DEVICE_INPUT_MODERN)
            continue;

        /* Check we haven't already claimed this slot */
        int already = 0;
        for (int i = 0; i < input_dev_count; i++) {
            if (input_devs[i].pci.dev == dev) {
                already = 1;
                break;
            }
        }
        if (already)
            continue;

        struct virtio_input *inp = &input_devs[input_dev_count];
        inp->pci.bus = 0;
        inp->pci.dev = dev;
        inp->pci.func = 0;
        inp->pci.vendor_id = vendor;
        inp->pci.device_id = device;

        uart_puts("[PCI] Found virtio-input at slot ");
        uart_putdec(dev);
        uart_puts("\n");

        pci_assign_bars(&inp->pci);
        pci_enable_device(&inp->pci);

        if (virtio_pci_parse_caps(&inp->pci, &inp->vpci) < 0)
            continue;

        /* No device-specific features */
        if (virtio_pci_init_device(&inp->pci, &inp->vpci, 0, 0) < 0)
            continue;

        /* Setup eventq (queue 0) */
        if (virtio_pci_setup_queue(&inp->vpci, &inp->evtq, 0, 64) < 0)
            continue;

        /* Setup statusq (queue 1) — we don't use it but must init */
        if (virtio_pci_setup_queue(&inp->vpci, &inp->stsq, 1, 8) < 0)
            continue;

        virtio_pci_set_driver_ok(&inp->vpci);

        /* Query device identity */
        read_device_name(inp);
        detect_device_type(inp);

        uart_puts("  Name: \"");
        uart_puts(inp->name);
        uart_puts("\" [");
        if (inp->is_keyboard) uart_puts("keyboard");
        if (inp->is_keyboard && inp->is_mouse) uart_puts("+");
        if (inp->is_mouse) uart_puts("mouse");
        if (!inp->is_keyboard && !inp->is_mouse) uart_puts("unknown");
        uart_puts("]\n");

        /* Pre-fill event queue */
        fill_evt_queue(inp, input_dev_count);

        input_dev_count++;
        return dev + 1; /* return next slot to scan from */
    }
    return -1; /* no more found */
}

/* ---- Public API ---- */

int virtio_input_init_all(void) {
    uart_puts("\n[VIRTIO-INPUT] === Scanning for input devices ===\n");
    input_dev_count = 0;

    int next = 0;
    while (input_dev_count < MAX_INPUT_DEVS) {
        next = find_and_init_input(next);
        if (next < 0)
            break;
    }

    uart_puts("[VIRTIO-INPUT] Found ");
    uart_putdec((uint64_t)input_dev_count);
    uart_puts(" input device(s)\n\n");

    return input_dev_count;
}

int virtio_input_poll(struct virtio_input_event *evt) {
    for (int i = 0; i < input_dev_count; i++) {
        struct virtio_input *dev = &input_devs[i];
        uint16_t desc_idx;
        uint32_t len;

        if (!virtqueue_get_used(&dev->evtq, &desc_idx, &len))
            continue;

        if (len >= INPUT_EVENT_SIZE) {
            int slot = desc_to_evt[i][desc_idx];
            struct virtio_input_event *src = &evt_buffers[i][slot];
            evt->type  = src->type;
            evt->code  = src->code;
            evt->value = src->value;

            /* Re-post buffer */
            post_evt_buffer(dev, i, slot);
            virtqueue_kick(&dev->evtq);
            return 1;
        }

        /* Short read — re-post anyway */
        int slot = desc_to_evt[i][desc_idx];
        post_evt_buffer(dev, i, slot);
        virtqueue_kick(&dev->evtq);
    }
    return 0;
}

int virtio_input_get_count(void) {
    return input_dev_count;
}

struct virtio_input *virtio_input_get_dev(int index) {
    if (index < 0 || index >= input_dev_count)
        return NULL;
    return &input_devs[index];
}

const char *key_name(uint16_t code) {
    switch (code) {
    case KEY_ESC:        return "Esc";
    case KEY_1:          return "1";
    case KEY_2:          return "2";
    case KEY_3:          return "3";
    case KEY_4:          return "4";
    case KEY_5:          return "5";
    case KEY_6:          return "6";
    case KEY_7:          return "7";
    case KEY_8:          return "8";
    case KEY_9:          return "9";
    case KEY_0:          return "0";
    case KEY_MINUS:      return "-";
    case KEY_EQUAL:      return "=";
    case KEY_BACKSPACE:  return "Bksp";
    case KEY_TAB:        return "Tab";
    case KEY_Q:          return "Q";
    case KEY_W:          return "W";
    case KEY_E:          return "E";
    case KEY_R:          return "R";
    case KEY_T:          return "T";
    case KEY_Y:          return "Y";
    case KEY_U:          return "U";
    case KEY_I:          return "I";
    case KEY_O:          return "O";
    case KEY_P:          return "P";
    case KEY_LEFTBRACE:  return "[";
    case KEY_RIGHTBRACE: return "]";
    case KEY_ENTER:      return "Enter";
    case KEY_LEFTCTRL:   return "LCtrl";
    case KEY_A:          return "A";
    case KEY_S:          return "S";
    case KEY_D:          return "D";
    case KEY_F:          return "F";
    case KEY_G:          return "G";
    case KEY_H:          return "H";
    case KEY_J:          return "J";
    case KEY_K:          return "K";
    case KEY_L:          return "L";
    case KEY_SEMICOLON:  return ";";
    case KEY_APOSTROPHE: return "'";
    case KEY_GRAVE:      return "`";
    case KEY_LEFTSHIFT:  return "LShift";
    case KEY_BACKSLASH:  return "\\";
    case KEY_Z:          return "Z";
    case KEY_X:          return "X";
    case KEY_C:          return "C";
    case KEY_V:          return "V";
    case KEY_B:          return "B";
    case KEY_N:          return "N";
    case KEY_M:          return "M";
    case KEY_COMMA:      return ",";
    case KEY_DOT:        return ".";
    case KEY_SLASH:      return "/";
    case KEY_RIGHTSHIFT: return "RShift";
    case KEY_LEFTALT:    return "LAlt";
    case KEY_SPACE:      return "Space";
    case KEY_CAPSLOCK:   return "Caps";
    case KEY_UP:         return "Up";
    case KEY_DOWN:       return "Down";
    case KEY_LEFT:       return "Left";
    case KEY_RIGHT:      return "Right";
    default:             return "?";
    }
}
