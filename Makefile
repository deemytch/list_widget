PREFIX     ?= /usr
DESTDIR    ?=
PKGS        = libxfce4panel-2.0 gtk+-3.0
PLUGIN_DIR ?= $(shell pkg-config --variable=libdir libxfce4panel-2.0)/xfce4/panel/plugins
DESKTOP_DIR = $(PREFIX)/share/xfce4/panel/plugins

CC       ?= gcc
CFLAGS   ?= -O2 -g
CFLAGS   += -std=gnu11 -Wall -Wextra -fPIC $(shell pkg-config --cflags $(PKGS))
LDFLAGS  += -shared
LDLIBS    = $(shell pkg-config --libs $(PKGS))

PLUGIN  = libovpn-widget.so
SOURCES = src/panel.c src/reader.c src/config.c src/log.c
OBJECTS = $(SOURCES:.c=.o)

all: $(PLUGIN) ovpn-send

$(PLUGIN): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

src/%.o: src/%.c src/ovpn-widget.h
	$(CC) $(CFLAGS) -c -o $@ $<

ovpn-send: tools/ovpn-send.c
	$(CC) -O2 -Wall -Wextra -o $@ $<

install: all
	install -d $(DESTDIR)$(PLUGIN_DIR) $(DESTDIR)$(DESKTOP_DIR)
	install -m 0644 $(PLUGIN) $(DESTDIR)$(PLUGIN_DIR)/$(PLUGIN)
	install -m 0644 ovpn-widget.desktop $(DESTDIR)$(DESKTOP_DIR)/ovpn-widget.desktop

uninstall:
	rm -f $(DESTDIR)$(PLUGIN_DIR)/$(PLUGIN)
	rm -f $(DESTDIR)$(DESKTOP_DIR)/ovpn-widget.desktop

clean:
	rm -f $(OBJECTS) $(PLUGIN) ovpn-send

.PHONY: all install uninstall clean
