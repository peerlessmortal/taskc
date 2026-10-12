CC = gcc
CFLAGS = -Wall -Wextra -O3 -D_GNU_SOURCE
LDFLAGS = -lncurses -lm

PREFIX ?= /usr/local

# Use distinct binary names to avoid colliding with source directories
TASKC_BIN = taskc_bin
SFETCH_BIN = sfetch_bin

all: $(TASKC_BIN) $(SFETCH_BIN)

$(TASKC_BIN): taskc/main.c
	$(CC) $(CFLAGS) taskc/main.c -o $(TASKC_BIN) $(LDFLAGS)

$(SFETCH_BIN): sfetch/main.c
	$(CC) $(CFLAGS) sfetch/main.c -o $(SFETCH_BIN) $(LDFLAGS)

clean:
	rm -f $(TASKC_BIN) $(SFETCH_BIN) *.tmp .tuxedo_config

install: all
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(TASKC_BIN) $(DESTDIR)$(PREFIX)/bin/taskc
	install -m 755 $(SFETCH_BIN) $(DESTDIR)$(PREFIX)/bin/sfetch

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/taskc
	rm -f $(DESTDIR)$(PREFIX)/bin/sfetch

.PHONY: all clean install uninstall
