/* include/kernel/keyboard.h -- PS/2 keyboard (IRQ1, scancode set 1) */
#ifndef KESTREL_KEYBOARD_H
#define KESTREL_KEYBOARD_H

#include <stdint.h>
#include <stdbool.h>

enum keycode {
    KEY_NONE = 0,
    /* printable keys report their ASCII value in key_event.ascii and use
       KEY_CHAR as the key code */
    KEY_CHAR = 1,
    KEY_ENTER = 0x100, KEY_TAB, KEY_ESC, KEY_BACKSPACE,
    KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
    KEY_HOME, KEY_END, KEY_PGUP, KEY_PGDN, KEY_INSERT, KEY_DELETE,
    KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
    KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
};

#define MOD_SHIFT 0x01
#define MOD_CTRL  0x02
#define MOD_ALT   0x04
#define MOD_CAPS  0x08

struct key_event {
    uint16_t key;       /* enum keycode                         */
    char     ascii;     /* translated character, 0 if none       */
    uint8_t  mods;      /* MOD_* state at the time of the press */
};

void keyboard_init(void);
bool keyboard_poll(struct key_event *ev);     /* non-blocking               */
void keyboard_wait(struct key_event *ev);     /* blocks the calling thread  */
bool keyboard_has_input(void);
void keyboard_flush(void);
/* Queue a key press from another input driver (USB HID). Safe in IRQ context. */
void keyboard_inject(uint16_t key, char ascii, uint8_t mods);
const char *keyboard_key_name(uint16_t key);

/* Wait channel woken on every key press (see task.h sleep_on/wakeup) */
extern int keyboard_waitq;

#endif
