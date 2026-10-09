/* =============================================================================
 *  kinput.h -- /dev/kinput: raw input events for user-space display servers
 *
 *  An evdev-like stream of fixed-size records. Codes are Linux input codes
 *  (KEY_*, BTN_*, REL_*), so X's evdev keymap applies unchanged (X keycode =
 *  code + 8). Keyboard events come from the PS/2 driver (scancode set 1) and
 *  USB boot keyboards (HID usage page 7); pointer events from USB boot mice.
 *
 *  A program takes the devices with ioctl(KINPUT_GRAB, 1). While grabbed,
 *  keys and pointer motion go only to the queue: TTY input, focus hot-keys
 *  and the kernel's pointer overlay are suspended. Closing the grabbing file
 *  (including at process exit) gives them back.
 * ============================================================================= */
#ifndef KERNEL_KINPUT_H
#define KERNEL_KINPUT_H

#include <stdint.h>
#include <stdbool.h>

struct kinput_event {
    uint64_t time_us;           /* monotonic */
    uint16_t type;              /* EV_*  */
    uint16_t code;              /* KEY_* / BTN_* / REL_* */
    int32_t  value;             /* key: 0 release, 1 press, 2 repeat; rel: delta */
};

#define KINPUT_EV_SYN   0
#define KINPUT_EV_KEY   1
#define KINPUT_EV_REL   2

#define KINPUT_REL_X      0
#define KINPUT_REL_Y      1
#define KINPUT_REL_WHEEL  8

#define KINPUT_BTN_LEFT   0x110
#define KINPUT_BTN_RIGHT  0x111
#define KINPUT_BTN_MIDDLE 0x112

#define KINPUT_GRAB     0x4b10  /* arg (int): 1 grab, 0 release */

void kinput_init(void);                 /* registers /dev/kinput */
bool kinput_grabbed(void);

/* Producers (ISR context). */
void kinput_ps2_byte(uint8_t scancode);                 /* raw set-1 byte */
void kinput_usb_key(uint8_t usage, bool pressed, bool repeat);
void kinput_usb_mouse(int dx, int dy, int wheel, uint8_t buttons);  /* bit0 L, 1 R, 2 M */

#endif
