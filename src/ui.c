#define _GNU_SOURCE
#include "ui.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

int ui_verbose;

static bool keys_on;
static bool status_tty;
static bool status_shown;
static char status_line[256];
static struct termios saved_tio;
static volatile sig_atomic_t raw_active;

static void set_raw(void) {
    struct termios tio = saved_tio;
    // ISIG stays on so Ctrl+C and Ctrl+Z keep working.
    tio.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
    tio.c_cc[VMIN] = 1;
    tio.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &tio);
    raw_active = 1;
}

static void on_tstp(int sig) {
    (void)sig;
    if (raw_active) tcsetattr(STDIN_FILENO, TCSANOW, &saved_tio);
    signal(SIGTSTP, SIG_DFL);
    raise(SIGTSTP);
}

static void on_cont(int sig) {
    (void)sig;
    struct sigaction sa = {0};
    sa.sa_handler = on_tstp;
    sigaction(SIGTSTP, &sa, NULL);
    if (raw_active) {
        struct termios tio = saved_tio;
        tio.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
        tio.c_cc[VMIN] = 1;
        tio.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &tio);
    }
}

void ui_init(bool keys) {
    status_tty = isatty(STDERR_FILENO);
    if (!keys || !isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &saved_tio) != 0)
        return;
    keys_on = true;
    set_raw();
    struct sigaction sa = {0};
    sa.sa_handler = on_tstp;
    sigaction(SIGTSTP, &sa, NULL);
    sa.sa_handler = on_cont;
    sigaction(SIGCONT, &sa, NULL);
    atexit(ui_restore);
}

void ui_restore(void) {
    ui_status_clear();
    if (raw_active) {
        tcsetattr(STDIN_FILENO, TCSANOW, &saved_tio);
        raw_active = 0;
    }
}

bool ui_keys_enabled(void) { return keys_on; }

static void clear_line(void) {
    if (status_shown) {
        fputs("\r\033[K", stderr);
        status_shown = false;
    }
}

static void redraw_status(void) {
    if (status_tty && status_line[0]) {
        fprintf(stderr, "\r\033[K%s", status_line);
        status_shown = true;
    }
    fflush(stderr);
}

static void vline(const char *prefix, const char *fmt, va_list ap) {
    clear_line();
    fputs(prefix, stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    redraw_status();
}

void ui_msg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vline("", fmt, ap);
    va_end(ap);
}

void ui_err(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vline("error: ", fmt, ap);
    va_end(ap);
}

void ui_dbg(const char *fmt, ...) {
    if (!ui_verbose) return;
    va_list ap;
    va_start(ap, fmt);
    vline("debug: ", fmt, ap);
    va_end(ap);
}

void ui_status(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(status_line, sizeof(status_line), fmt, ap);
    va_end(ap);
    if (status_tty) redraw_status();
}

void ui_status_clear(void) {
    clear_line();
    status_line[0] = '\0';
    fflush(stderr);
}

static int read_byte(int timeout_ms) {
    struct pollfd p = {.fd = STDIN_FILENO, .events = POLLIN, .revents = 0};
    int r = poll(&p, 1, timeout_ms);
    if (r <= 0) return -1;
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return -1;
    return c;
}

int ui_read_key(int timeout_ms) {
    if (!keys_on) {
        if (timeout_ms > 0) poll(NULL, 0, timeout_ms);
        return KEY_NONE;
    }
    int c = read_byte(timeout_ms);
    if (c < 0) return KEY_NONE;
    if (c == '\r' || c == '\n') return KEY_ENTER;
    if (c == 0x7F || c == 0x08) return KEY_BACKSPACE;
    if (c != 0x1B) return c;
    // Arrow keys arrive as ESC [ A or ESC O A.
    int c1 = read_byte(30);
    if (c1 != '[' && c1 != 'O') return KEY_ESC;
    int c2 = read_byte(30);
    switch (c2) {
    case 'A': return KEY_UP;
    case 'B': return KEY_DOWN;
    case 'C': return KEY_RIGHT;
    case 'D': return KEY_LEFT;
    default:
        // Skip the rest of other sequences such as ESC [ 5 ~
        while (c2 >= '0' && c2 <= '9') c2 = read_byte(30);
        return KEY_NONE;
    }
}