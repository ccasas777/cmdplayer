CC ?= gcc
PKG_CONFIG ?= pkg-config
PREFIX ?= /usr/local
PKGS = alsa libavformat libavcodec libavutil

CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -pthread $(shell $(PKG_CONFIG) --cflags $(PKGS))
LDLIBS += $(shell $(PKG_CONFIG) --libs $(PKGS)) -lm -pthread

SRCS = $(wildcard src/*.c)
OBJS = $(SRCS:.c=.o)

all: cmdplay

cmdplay: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

src/%.o: src/%.c $(wildcard src/*.h)
	$(CC) $(CFLAGS) -c -o $@ $<

install: cmdplay
	install -Dm755 cmdplay $(DESTDIR)$(PREFIX)/bin/cmdplay

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/cmdplay

clean:
	rm -f cmdplay $(OBJS)

.PHONY: all install uninstall clean
```[cite: 43]