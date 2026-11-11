CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic
BUILD_DIR ?= build
ARCH ?= $(shell uname -m)
PREFIX ?= /usr/local
DESTDIR ?=
OUT ?= $(BUILD_DIR)/$(ARCH)/kitty-litter
.PHONY: all install uninstall clean
all:
	mkdir -p $(dir $(OUT))
	$(CC) $(CFLAGS) -std=c11 $$($(PKG_CONFIG) --cflags gtk+-3.0 sqlite3) ../gtk/kitty-litter.c $$($(PKG_CONFIG) --libs gtk+-3.0 sqlite3) -o $(OUT)
install: all
	mkdir -p $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/share/applications $(DESTDIR)$(PREFIX)/share/icons/hicolor/256x256/apps
	install -m 0755 $(OUT) $(DESTDIR)$(PREFIX)/bin/kitty-litter
	install -m 0644 kitty-litter.desktop $(DESTDIR)$(PREFIX)/share/applications/kitty-litter.desktop
	install -m 0644 kitty-litter.png $(DESTDIR)$(PREFIX)/share/icons/hicolor/256x256/apps/kitty-litter.png
uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/kitty-litter $(DESTDIR)$(PREFIX)/share/applications/kitty-litter.desktop $(DESTDIR)$(PREFIX)/share/icons/hicolor/256x256/apps/kitty-litter.png
clean:
	rm -rf $(BUILD_DIR)
