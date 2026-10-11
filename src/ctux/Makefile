CC      = cc
CFLAGS  = -Wall -Wextra -O2 -std=c11
LDFLAGS = -lm -lncurses
PREFIX  = /usr/local

sfetch: main.c
	$(CC) $(CFLAGS) -o taskc main.c $(LDFLAGS)

install: taskc
	install -Dm755 taskc $(DESTDIR)$(PREFIX)/bin/taskc

clean:
	rm -f taskc

.PHONY: install clean
