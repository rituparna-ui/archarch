#ifndef VIRTIO_INPUT_H
#define VIRTIO_INPUT_H

#include "pci.h"
#include "virtio_pci.h"
#include "virtqueue.h"

/*
 * Virtio input device (spec 5.8)
 *
 * Mirrors Linux evdev — sends {type, code, value} events over virtio.
 * QEMU creates separate PCI devices for keyboard and mouse/tablet.
 *
 * Device ID: 18 (transitional 0x1052, modern 0x1052)
 *
 * Virtqueues:
 *   0: eventq  — device-writable, input events from device to driver
 *   1: statusq — device-readable, status feedback (LED updates etc)
 *
 * Device config is queried by writing select+subsel, then reading
 * the returned data. Used to get device name, supported event types, etc.
 */

/* PCI device IDs */
#define VIRTIO_PCI_DEVICE_INPUT_TRANSITIONAL  0x1052
#define VIRTIO_PCI_DEVICE_INPUT_MODERN        0x1052  /* 0x1040 + 18 */

/* Device config select values (spec 5.8.4) */
#define VIRTIO_INPUT_CFG_UNSET      0x00
#define VIRTIO_INPUT_CFG_ID_NAME    0x01
#define VIRTIO_INPUT_CFG_ID_SERIAL  0x02
#define VIRTIO_INPUT_CFG_ID_DEVIDS  0x03
#define VIRTIO_INPUT_CFG_PROP_BITS  0x10
#define VIRTIO_INPUT_CFG_EV_BITS    0x11
#define VIRTIO_INPUT_CFG_ABS_INFO   0x12

/*
 * Device config layout (at device_cfg MMIO region):
 *   offset 0x00: u8  select
 *   offset 0x01: u8  subsel
 *   offset 0x02: u8  size     (read-only, set by device)
 *   offset 0x03: u8  reserved[5]
 *   offset 0x08: u8  data[128]
 *
 * Write select+subsel, read size, then read size bytes from data[].
 */
#define VIRTIO_INPUT_CFG_SELECT_OFF  0x00
#define VIRTIO_INPUT_CFG_SUBSEL_OFF  0x01
#define VIRTIO_INPUT_CFG_SIZE_OFF    0x02
#define VIRTIO_INPUT_CFG_DATA_OFF    0x08

/* Linux evdev event types (subset) */
#define EV_SYN          0x00
#define EV_KEY          0x01
#define EV_REL          0x02
#define EV_ABS          0x03
#define EV_MSC          0x04
#define EV_LED          0x11
#define EV_REP          0x14

/* Key codes (subset — standard US keyboard) */
#define KEY_RESERVED    0
#define KEY_ESC         1
#define KEY_1           2
#define KEY_2           3
#define KEY_3           4
#define KEY_4           5
#define KEY_5           6
#define KEY_6           7
#define KEY_7           8
#define KEY_8           9
#define KEY_9           10
#define KEY_0           11
#define KEY_MINUS       12
#define KEY_EQUAL       13
#define KEY_BACKSPACE   14
#define KEY_TAB         15
#define KEY_Q           16
#define KEY_W           17
#define KEY_E           18
#define KEY_R           19
#define KEY_T           20
#define KEY_Y           21
#define KEY_U           22
#define KEY_I           23
#define KEY_O           24
#define KEY_P           25
#define KEY_LEFTBRACE   26
#define KEY_RIGHTBRACE  27
#define KEY_ENTER       28
#define KEY_LEFTCTRL    29
#define KEY_A           30
#define KEY_S           31
#define KEY_D           32
#define KEY_F           33
#define KEY_G           34
#define KEY_H           35
#define KEY_J           36
#define KEY_K           37
#define KEY_L           38
#define KEY_SEMICOLON   39
#define KEY_APOSTROPHE  40
#define KEY_GRAVE       41
#define KEY_LEFTSHIFT   42
#define KEY_BACKSLASH   43
#define KEY_Z           44
#define KEY_X           45
#define KEY_C           46
#define KEY_V           47
#define KEY_B           48
#define KEY_N           49
#define KEY_M           50
#define KEY_COMMA       51
#define KEY_DOT         52
#define KEY_SLASH       53
#define KEY_RIGHTSHIFT  54
#define KEY_LEFTALT     56
#define KEY_SPACE       57
#define KEY_CAPSLOCK    58
#define KEY_F1          59
#define KEY_F2          60
#define KEY_F3          61
#define KEY_F4          62
#define KEY_F5          63
#define KEY_F6          64
#define KEY_F7          65
#define KEY_F8          66
#define KEY_F9          67
#define KEY_F10         68
#define KEY_F11         87
#define KEY_F12         88
#define KEY_UP          103
#define KEY_LEFT        105
#define KEY_RIGHT       106
#define KEY_DOWN        108

/* Relative axis codes (mouse) */
#define REL_X           0x00
#define REL_Y           0x01
#define REL_WHEEL       0x08

/* Absolute axis codes (tablet) */
#define ABS_X           0x00
#define ABS_Y           0x01

/* Mouse button codes */
#define BTN_LEFT        0x110
#define BTN_RIGHT       0x111
#define BTN_MIDDLE      0x112

/* Event structure — same as Linux input_event (without timestamp) */
struct virtio_input_event {
    uint16_t type;
    uint16_t code;
    uint32_t value;
} __attribute__((packed));

#define INPUT_EVENT_SIZE sizeof(struct virtio_input_event)

/* Number of pre-posted event buffers */
#define INPUT_EVT_RING_SIZE 64

/* Max input devices we scan for */
#define MAX_INPUT_DEVS 4

struct virtio_input {
    struct pci_device     pci;
    struct virtio_pci_dev vpci;
    struct virtqueue      evtq;     /* eventq (queue 0) */
    struct virtqueue      stsq;     /* statusq (queue 1) */
    char                  name[64];
    int                   is_keyboard;
    int                   is_mouse;
};

/*
 * Initialize all virtio-input devices on the PCI bus.
 * Returns the number of devices found (keyboard + mouse).
 */
int virtio_input_init_all(void);

/*
 * Poll for an input event (non-blocking).
 * Checks all initialized input devices.
 * Returns 1 if an event was received, 0 if none available.
 */
int virtio_input_poll(struct virtio_input_event *evt);

/*
 * Get a human-readable name for a key code.
 * Returns a short string like "A", "Enter", "Space", etc.
 */
const char *key_name(uint16_t code);

/*
 * Access individual input devices (for interrupt registration etc).
 */
int virtio_input_get_count(void);
struct virtio_input *virtio_input_get_dev(int index);

#endif
