CC = gcc
CFLAGS = -Wall -Wextra -O3 -D_GNU_SOURCE
LDFLAGS = -lncurses -lm

PREFIX ?= /usr/local
BUILD_DIR = build

all: $(BUILD_DIR)/taskc $(BUILD_DIR)/sfetch

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/taskc: taskc/main.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) taskc/main.c -o $@ $(LDFLAGS)

$(BUILD_DIR)/sfetch: sfetch/main.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) sfetch/main.c -o $@ $(LDFLAGS)

clean:
	rm -rf $(BUILD_DIR) *.tmp .tuxedo_config

install: all
	@echo "Removing previous installations..."
	rm -f $(DESTDIR)$(PREFIX)/bin/taskc
	rm -f $(DESTDIR)$(PREFIX)/bin/sfetch
	@echo "Installing new binaries..."
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(BUILD_DIR)/taskc $(DESTDIR)$(PREFIX)/bin/taskc
	install -m 755 $(BUILD_DIR)/sfetch $(DESTDIR)$(PREFIX)/bin/sfetch
	@echo "Cleaning up local build directory..."
	rm -rf $(BUILD_DIR)

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/taskc
	rm -f $(DESTDIR)$(PREFIX)/bin/sfetch

update:
	@echo "Pulling latest changes from git..."
	git pull
	$(MAKE) install

.PHONY: all clean install uninstall update
