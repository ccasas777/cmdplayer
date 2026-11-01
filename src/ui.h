#ifndef CMDPLAY_UI_H
#define CMDPLAY_UI_H

#include <stdbool.h>

enum {
    KEY_NONE = 0,
    KEY_ENTER = '\n',
    KEY_BACKSPACE = 0x7F,
    KEY_ESC = 0x1B,
    KEY_UP = 0x100,
    KEY_DOWN,
    KEY_RIGHT,
    KEY_LEFT,
};

extern int ui_verbose;

// Puts the terminal in single key mode when keys is set and stdin is a tty.
void ui_init(bool keys);
void ui_restore(void);
bool ui_keys_enabled(void);

void ui_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void ui_err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void ui_dbg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// One line that is redrawn in place; only shown on a terminal.
void ui_status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void ui_status_clear(void);

// Waits up to timeout_ms for a key.  Returns KEY_NONE on timeout.
int ui_read_key(int timeout_ms);

#endif