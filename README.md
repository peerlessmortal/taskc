# TaskC & Sfetch

> A lightning-fast, `todo.txt`-compatible command-line and **Terminal User Interface (TUI)** featuring **Sfetch**—a retro-style statistics and radar chart productivity viewer. 

This project was heavily inspired by the architecture and concept of **tuxedo** by [webstonehq](https://github.com/webstonehq/tuxedo). AI coding assistance was utilized during the development process of this repository.

---

## Features

* **Minimalist TUI**: A slick, responsive terminal interface powered by `ncurses` for managing tasks efficiently with Vim-like bindings (`j`/`k`, `gg`, `G`, etc.).
* **Sfetch Integration (`A`)**: View a stunning stats chart and radar breakdown of your completed attributes and productivity metrics (Knowledge, Vitality, Diligence, Charm, Proficiency).
* **`todo.txt` Compatibility**: Fully interoperable with standard `todo.txt` formats (`due:YYYY-MM-DD`, `rec:2d`, priorities, contexts, and projects).
* **Smart Autocomplete**: Real-time suggestion dropdowns for tags (`+`, `@`, `!`) with robust handling and partial word matching.
* **Persistent Preferences & Undo**: Keeps track of sorting modes and features a safety stack for quick undo actions (`u`).

---

## Repository Structure

```text
taskc/
├── Makefile
├── sfetch/
│   └── main.c
└── taskc/
    └── main.c
```

---

## Makefile

Here is the `Makefile` used to compile both binaries into a temporary `build/` directory, install them system-wide, and automatically clean up local build artifacts:

```makefile
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
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(BUILD_DIR)/taskc $(DESTDIR)$(PREFIX)/bin/taskc
	install -m 755 $(BUILD_DIR)/sfetch $(DESTDIR)$(PREFIX)/bin/sfetch
	rm -rf $(BUILD_DIR)

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/taskc
	rm -f $(DESTDIR)$(PREFIX)/bin/sfetch

.PHONY: all clean install uninstall
```

---

## Installation Guide

### Prerequisites
Make sure you have a C compiler (`gcc`), `make`, and the `ncurses` development libraries installed on your system.

* **Debian / Ubuntu / Mint**:
  ```bash
  sudo apt update && sudo apt install build-essential libncurses-dev
  ```
* **Arch Linux / Manjaro**:
  ```bash
  sudo pacman -S base-devel ncurses
  ```
* **Fedora / RHEL**:
  ```bash
  sudo dnf install gcc make ncurses-devel
  ```

### Building & Installing from Source

1. Clone or download the repository:
   ```bash
   git clone https://github.com/peerlessmortal/taskc.git
   cd taskc
   ```

2. Compile the binaries locally:
   ```bash
   make
   ```

3. Install system-wide (this will place `taskc` and `sfetch` into `/usr/local/bin` and neatly clean up local build files):
   ```bash
   sudo make install
   ```

---

## Usage & Quick Start

* Run the task manager TUI:
  ```bash
  taskc
  ```
* Run the statistics radar view:
  ```bash
  sfetch
  ```

### Key Bindings inside the TaskC TUI

| Key | Action |
| :--- | :--- |
| `n` | Add a new task (with calendar/repeat helpers) |
| `e` | Edit the currently selected task |
| `x` | Toggle task completion |
| `dd` | Delete task |
| `yy` | Copy/duplicate task |
| `p` | Cycle priority (`A` through `D`) |
| `A` | Open **Sfetch** stats radar view |
| `S` | Cycle sort filters (Priority, Due Date, etc.) |
| `u` | Undo last change |
| `?` | Toggle help overlay |
| `q` | Quit application |

---

## Acknowledgments

* Strongly inspired by [tuxedo](https://github.com/webstonehq/tuxedo) and Persona 4.
* Developed with the assistance of AI tooling.
