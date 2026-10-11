# sfetch

A neofetch-style terminal fetch tool that turns your [tuxedo](https://github.com/webstonehq/tuxedo) / todo.txt task history into a gamified ASCII radar chart — Persona-confidant-style attribute levels, built from the tasks you've actually completed.

`sfetch` doesn't manage tasks itself. Add and complete tasks with `tuxedo`, any other todo.txt-cli compatible tool, or by hand — `sfetch` only *reads* the resulting `todo.txt` / `done.txt` and renders the chart.

---

## Features

- **ASCII radar chart** — a 5-axis spider chart drawn with Unicode glyphs (`✦` center, `◆` vertices, `●` edges, `·` guides), filled with a 5-tier gray gradient (donut.c-style radial brightness falloff) when your terminal supports color.
- **5 tracked attributes**, each earning points from tagged tasks:

  | Attribute   | Marker |
  |-------------|--------|
  | Knowledge   | `!k`   |
  | Vitality    | `!v`   |
  | Diligence   | `!d`   |
  | Charm       | `!c`   |
  | Proficiency | `!p`   |

- **Level system (1–6)**, not raw points — thresholds grow exponentially so each level means more than the last:

  | Level | Points needed |
  |-------|---------------|
  | 1     | 0 (the floor — always at least Level 1) |
  | 2     | 3   |
  | 3     | 9   |
  | 4     | 21  |
  | 5     | 45  |
  | 6     | 93  |

  Points cap at 100 per attribute. The chart's axis length is level-driven (`level / 6`), not points-driven, and six concentric rings mark each level boundary — a proper spider-chart grid, not just an outline.
- **Multiple markers per task** — a single completed task can feed more than one attribute: `tuxedo add "helped a friend cram for finals !k !v"`.
- **Recognizes recurring and one-off completions the same way** — every completed (`x `-prefixed) line counts once, whether it's a single task or the 40th instance of a recurring habit.
- **Two display modes**:
  - `sfetch` (default) — chart beside a neofetch-style system info panel (OS, Kernel, Uptime, Shell, Host).
  - `sfetch -t` — chart beside your task/attribute stats instead (level bars, points, completion counts).
- **Color-aware output** — fires 256-color ANSI only when stdout is a real terminal (`isatty()` check); piped or redirected output (`sfetch > file`, `sfetch | less`) stays plain glyphs, no stray escape codes.

---

## Build

Requires a C compiler and `libm`. No other dependencies.

```sh
make
sudo make install   # optional — installs to /usr/local/bin
```

Or without the Makefile:

```sh
gcc -Wall -Wextra -O2 -std=c11 -o sfetch main.c -lm
```

---

## Usage

Tag a task with `!<code>` to give it one point in the matching attribute:

```sh
tuxedo add "studied for exam +school !k"
tuxedo do 3
sfetch
```

```sh
sfetch                 # chart + system info (default)
sfetch -t               # chart + task/attribute stats
sfetch fetch             # same as sfetch
sfetch fetch -t          # same as sfetch -t
sfetch attrs              # show current attribute names + codes
sfetch attrs Knowledge:k Vitality:v Diligence:d Charm:c Proficiency:p   # rename/recode all 5
```

Rules:

- Only completed (`x `-prefixed) lines are scanned — whether still in `todo.txt` or archived to `done.txt`.
- A marker must be its own whitespace-separated token: `!k` counts; `!knowledge`, `!k2`, and `studied!k` do not.
- Matching is case-insensitive (`!K` works).
- Each marker is worth exactly 1 point — no weighting. Want a task to count for more? Add the marker more than once: `!k !k !k` = 3 Knowledge points from one task.
- A completed task with no recognized marker still counts toward total completions, just not toward any attribute.

---

## File resolution

Same resolution order as `tuxedo`, so one `$TODO_DIR` setup covers both tools:

- todo file: `$TODO_FILE` → `$TODO_DIR/todo.txt` → `./todo.txt`
- done file: `$DONE_FILE` → sibling `done.txt` next to the todo file

`sfetch`'s own state is just the 5 attribute display names/codes:

- `~/.config/sfetch/attributes.conf` — one `Name:code` line per attribute

Renaming via `sfetch attrs ...` only affects matching going forward — it doesn't retag tasks already written with the old code.

---

## Why a Level system instead of raw points

Raw point totals don't feel like progress the same way a level does — and a flat per-point scale means the first few tasks you complete look identical to the hundredth. Thresholds growing exponentially (0 → 3 → 9 → 21 → 45 → 93) means early levels come fast, and the top level actually means something.

---

Built with AI assistance (Claude, Gemini).
