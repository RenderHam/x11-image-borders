PREFIX ?= /usr/local
BIN     = xib

# clang by default unless the user picked a compiler or flags.
ifeq ($(origin CC),default)
CC := clang
endif

CFLAGS  ?= -O3
CFLAGS  += -std=c11 -Wall -Wextra -Wno-unused-parameter
PKGS     = cairo xcb xcb-render xcb-shape
CPPFLAGS += $(shell pkg-config --cflags $(PKGS))
LDLIBS  += $(shell pkg-config --libs $(PKGS))

all: $(BIN)

$(BIN): xib.c config.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ xib.c $(LDLIBS)

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN)

clean:
	rm -f $(BIN)

.PHONY: all install uninstall clean
