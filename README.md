# taskc

Two small C tools that work off the same plain-text task file:

- **`taskc`** — a keyboard-driven terminal task manager. A full interactive TUI plus a minimal CLI for quick add/complete/archive.
- **`sfetch`** — a companion neofetch-style fetch tool that turns your completed tasks into a gamified ASCII radar chart.

Both read and write the same `todo.txt` file, so they're meant to be used together — but each works fine on its own.

---

## Install

Both are single C files with no external dependencies beyond their libraries.

**taskc** (needs `libncurses-dev`):

```sh
sudo apt install libncurses-dev     # Debian/Ubuntu
sudo pacman -S ncurses              # Arch

cc -Wall -Wextra -O2 -std=c11 -o taskc main.c -lm -lncurses
```

**sfetch** (needs only `libm`, included with any standard C toolchain):

```sh
gcc -Wall -Wextra -O2 -std=c11 -o sfetch main.c -lm
```

Put either binary wherever's on your `$PATH` (e.g. `~/.local/bin`, or install system-wide to `/usr/local/bin`).

---

## How taskc works

### Storage

Plain `todo.txt`, resolved in this order:

- `$TODO_FILE` → `$TODO_DIR/todo.txt` → `./todo.txt`
- Completed/archived tasks go to `$DONE_FILE` → a sibling `done.txt` next to the todo file.

### Launching

```sh
taskc              # opens the interactive TUI
taskc add TEXT...  # one-shot: add a task and exit
taskc do N         # one-shot: mark task N complete and exit
taskc archive      # one-shot: move completed tasks into the done file
```

Any other input (including no recognized command) opens the TUI.

### The TUI

Keybinds, straight from the in-app help screen (`?`):

**Navigation**

| Key | Action |
|-----|--------|
| `j` / `↓` | next task |
| `k` / `↑` | previous task |
| `gg` | first task |
| `G` | last task |

**Editing**

| Key | Action |
|-----|--------|
| `n` | new task |
| `e` | edit task |
| `x` | toggle complete |
| `dd` | delete task |
| `p` | cycle priority `A → B → C → none` |
| `u` | undo |

**View**

| Key | Action |
|-----|--------|
| `/` | fuzzy search |
| `S` | cycle sort (file order / priority / due date / recurring) |
| `h` | show completed tasks in the list (hidden by default) |
| `A` | radar stats chart (see below) |

**System**: `?` help, `q` quit.

A left sidebar is always on screen, listing `+projects` (green) and `@contexts` (yellow) for the current task set, along with priority and attribute breakdowns — `Tab` moves focus into it to select an entry as a filter.

### Due dates and recurrence

Tasks support the standard todo.txt extensions: `due:YYYY-MM-DD` and `rec:[+]N{d,w,m,y,b}`. Completing a task that carries `rec:` automatically inserts a fresh copy with `due:` advanced by the interval:

- No `+` prefix — the new due date is computed from **today** (the completion date).
- `+` prefix — *strict*, anchored to the task's existing `due:` instead of today (so rent due the 15th stays on the 15th even if you complete it a few days late).
- `d`/`w`/`m`/`y` are calendar days/weeks/months/years; `b` is business days (Mon–Fri only).

Overdue, incomplete tasks are always pinned at the top of the list in red, regardless of whatever sort mode is active.

### Color coding

Every task line is colored by token: `+projects` green, `@contexts` yellow, `key:value` tags (`due:`, `rec:`, etc.) blue, and the priority prefix by letter (A red, B/C stepping down in intensity). Overdue tasks render as a solid red line instead, for visibility. All of it degrades gracefully to plain text on a terminal with no color support.

### Radar stats chart (`A`)

A full-screen view: the same 6-level attribute radar chart `sfetch` draws (see below), plus your level bars, shown directly inside taskc. It sizes itself to whatever terminal space is actually available, rather than assuming a fixed size.

---

## How sfetch works

`sfetch` doesn't manage tasks — it only *reads* `todo.txt`/`done.txt` (same file, same resolution order as taskc) and renders a radar chart from what you've completed.

### Marking tasks for sfetch

Tag a task with `!<code>` to give it one point toward an attribute:

```sh
taskc add "studied for exam +school !k"
taskc do 3
sfetch
```

| Attribute   | Marker |
|-------------|--------|
| Knowledge   | `!k`   |
| Vitality    | `!v`   |
| Diligence   | `!d`   |
| Charm       | `!c`   |
| Proficiency | `!p`   |

Rules:
- Only completed (`x `-prefixed) lines count, whether still in `todo.txt` or archived to `done.txt`.
- A marker must be its own whitespace-separated token (`!k` counts; `!knowledge` or `studied!k` don't).
- Case-insensitive. Each marker is worth exactly 1 point — stack them (`!k !k !k`) for a task worth more.
- A completed task with no recognized marker still counts toward total completions, just not toward any attribute.

### Levels, not raw points

Points cap at 100 per attribute, but the chart and stat bars read off a derived **Level (1–6)** with exponentially growing thresholds, so progress actually means something at each step:

| Level | Points needed |
|-------|---------------|
| 1 | 0 (the floor — always at least Level 1) |
| 2 | 3 |
| 3 | 9 |
| 4 | 21 |
| 5 | 45 |
| 6 | 93 |

The chart's axis length is level-driven (`level / 6`), with six concentric rings marking each level boundary.

### Display modes

```sh
sfetch                 # chart + system info (OS, Kernel, Uptime, Shell, Host)
sfetch -t               # chart + task/attribute stats instead
```

Interior fill uses a 5-tier gray gradient (radial brightness falloff from center) when stdout is a real terminal; piped or redirected output stays plain glyphs, no stray ANSI codes.

### sfetch's own config

```sh
sfetch attrs                                                        # show current names/codes
sfetch attrs Knowledge:k Vitality:v Diligence:d Charm:c Proficiency:p   # rename/recode all 5
```

Stored in `~/.config/sfetch/attributes.conf`. Renaming only affects matching going forward — it won't retag tasks already written with an old code.

---

## Credits

The idea for a fast, keyboard-driven todo.txt tool came from [tuxedo](https://github.com/webstonehq/tuxedo) by webstonehq. `taskc` is an independent C rewrite built from that idea, not a fork — it shares no code with the original Rust project.

Built with AI assistance (Claude, Gemini).
