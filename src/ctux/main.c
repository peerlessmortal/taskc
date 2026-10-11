/*
 * tuxedo (C rewrite) - a todo.txt-cli-compatible command line, plus a
 * minimal ncurses TUI, reading/writing the standard todo.txt format.
 *
 * Build: gcc main.c -lm -lncurses -o tuxedo && ./tuxedo
 */

#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAX_TASKS 8192
#define MAX_LINE 2048
#define PATH_LEN 1024
#define SIDEBAR_WIDTH 36

#define NUM_ATTRS 5
#define ATTR_LEN 24
#define MAX_POINTS 100
#define NUM_LEVELS 6
#define GRID_W 61
#define GRID_H 27

typedef struct {
  char name[ATTR_LEN];
  char code;
  int total;
  int marker_count;
} Attr;

static const Attr default_attrs[NUM_ATTRS] = {
    {"Knowledge", 'k', 0, 0},   {"Vitality", 'v', 0, 0},
    {"Diligence", 'd', 0, 0},   {"Charm", 'c', 0, 0},
    {"Proficiency", 'p', 0, 0},
};

static const int level_thresholds[NUM_LEVELS] = {0, 3, 9, 21, 45, 93};

static int compute_level(int total) {
  int lvl = 1;
  for (int i = 0; i < NUM_LEVELS; i++)
    if (total >= level_thresholds[i])
      lvl = i + 1;
  return lvl;
}

typedef struct {
  char *text; /* malloc'd, no trailing newline */
} Task;

static Task tasks[MAX_TASKS];
static int task_count = 0;

/* ================= config / state persistence ================= */

static void get_config_path(char *out, size_t out_sz) {
  const char *home = getenv("HOME");
  if (home && home[0]) {
    snprintf(out, out_sz, "%s/.tuxedo_config", home);
  } else {
    snprintf(out, out_sz, "./.tuxedo_config");
  }
}

static int load_sort_preference(void) {
  char path[PATH_LEN];
  get_config_path(path, sizeof(path));
  FILE *f = fopen(path, "r");
  if (!f)
    return 0;
  int mode = 0;
  if (fscanf(f, "%d", &mode) != 1)
    mode = 0;
  fclose(f);
  if (mode < 0 || mode >= 5)
    mode = 0;
  return mode;
}

static void save_sort_preference(int mode) {
  char path[PATH_LEN];
  get_config_path(path, sizeof(path));
  FILE *f = fopen(path, "w");
  if (!f)
    return;
  fprintf(f, "%d\n", mode);
  fclose(f);
}

/* ================= file resolution ================= */

static int file_exists(const char *path) {
  struct stat st;
  return path && stat(path, &st) == 0;
}

static void dir_of(const char *path, char *out, size_t out_sz) {
  const char *slash = strrchr(path, '/');
  if (!slash) {
    snprintf(out, out_sz, ".");
    return;
  }
  size_t len = (size_t)(slash - path);
  if (len == 0)
    len = 1;
  if (len >= out_sz)
    len = out_sz - 1;
  memcpy(out, path, len);
  out[len] = '\0';
}

static void ensure_parent_dir(const char *path) {
  char dir[PATH_LEN];
  dir_of(path, dir, sizeof(dir));
  if (strcmp(dir, ".") == 0)
    return;
  mkdir(dir, 0755);
}

static void resolve_todo_path(char *out, size_t out_sz) {
  const char *f = getenv("TODO_FILE");
  if (f && f[0]) {
    snprintf(out, out_sz, "%s", f);
    return;
  }
  const char *d = getenv("TODO_DIR");
  if (d && d[0]) {
    snprintf(out, out_sz, "%s/todo.txt", d);
    return;
  }
  snprintf(out, out_sz, "./todo.txt");
}

static void resolve_done_path(const char *todo_path, char *out, size_t out_sz) {
  const char *f = getenv("DONE_FILE");
  if (f && f[0]) {
    snprintf(out, out_sz, "%s", f);
    return;
  }
  char dir[PATH_LEN];
  dir_of(todo_path, dir, sizeof(dir));
  snprintf(out, out_sz, "%s/done.txt", dir);
}

/* ================= loading / saving (atomic) ================= */

static void free_tasks(void) {
  for (int i = 0; i < task_count; i++) {
    free(tasks[i].text);
    tasks[i].text = NULL;
  }
  task_count = 0;
}

static void load_tasks(const char *path) {
  free_tasks();
  FILE *f = fopen(path, "r");
  if (!f)
    return;
  char line[MAX_LINE];
  while (task_count < MAX_TASKS && fgets(line, sizeof(line), f)) {
    line[strcspn(line, "\r\n")] = '\0';
    if (line[0] == '\0')
      continue;
    tasks[task_count].text = strdup(line);
    task_count++;
  }
  fclose(f);
}

static int save_tasks(const char *path) {
  ensure_parent_dir(path);
  char tmp[PATH_LEN];
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);

  FILE *f = fopen(tmp, "w");
  if (!f) {
    fprintf(stderr, "tuxedo: %s: %s\n", tmp, strerror(errno));
    return -1;
  }
  for (int i = 0; i < task_count; i++)
    fprintf(f, "%s\n", tasks[i].text);
  fclose(f);

  if (rename(tmp, path) != 0) {
    fprintf(stderr, "tuxedo: rename %s -> %s: %s\n", tmp, path,
            strerror(errno));
    return -1;
  }
  return 0;
}

static int append_lines(const char *path, char **lines, int count) {
  ensure_parent_dir(path);
  FILE *f = fopen(path, "a");
  if (!f) {
    fprintf(stderr, "tuxedo: %s: %s\n", path, strerror(errno));
    return -1;
  }
  for (int i = 0; i < count; i++)
    fprintf(f, "%s\n", lines[i]);
  fclose(f);
  return 0;
}

/* ================= date helpers ================= */

static void today_str(char *out, size_t out_sz) {
  time_t t = time(NULL);
  struct tm tmv;
  localtime_r(&t, &tmv);
  strftime(out, out_sz, "%Y-%m-%d", &tmv);
}

static int looks_like_date(const char *s) {
  if (strlen(s) != 10)
    return 0;
  for (int i = 0; i < 10; i++) {
    if (i == 4 || i == 7) {
      if (s[i] != '-')
        return 0;
    } else if (!isdigit((unsigned char)s[i]))
      return 0;
  }
  return 1;
}

static int parse_date(const char *s, struct tm *out) {
  if (!s)
    return -1;
  int y, m, d;
  if (sscanf(s, "%d-%d-%d", &y, &m, &d) != 3)
    return -1;
  struct tm tmp;
  memset(&tmp, 0, sizeof(tmp));
  tmp.tm_year = y - 1900;
  tmp.tm_mon = m - 1;
  tmp.tm_mday = d;
  tmp.tm_hour = 12;
  if (mktime(&tmp) == (time_t)-1)
    return -1;
  *out = tmp;
  return 0;
}

static void add_interval(struct tm *tmv, int n, char unit) {
  switch (tolower((unsigned char)unit)) {
  case 'd':
    tmv->tm_mday += n;
    mktime(tmv);
    break;
  case 'w':
    tmv->tm_mday += n * 7;
    mktime(tmv);
    break;
  case 'm':
    tmv->tm_mon += n;
    mktime(tmv);
    break;
  case 'y':
    tmv->tm_year += n;
    mktime(tmv);
    break;
  case 'b': {
    int added = 0;
    while (added < n) {
      tmv->tm_mday += 1;
      mktime(tmv);
      if (tmv->tm_wday != 0 && tmv->tm_wday != 6)
        added++;
    }
    break;
  }
  default:
    break;
  }
}

/* ================= todo.txt line helpers ================= */

static int line_is_done(const char *s) { return s[0] == 'x' && s[1] == ' '; }

static char line_priority(const char *s) {
  if (!s)
    return 0;
  char buf[MAX_LINE];
  strncpy(buf, s, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  char *saveptr;
  char *tok = strtok_r(buf, " \t", &saveptr);
  while (tok) {
    if (tok[0] == '(' && tok[2] == ')' && isupper((unsigned char)tok[1]) &&
        tok[3] == '\0') {
      return tok[1];
    }
    tok = strtok_r(NULL, " \t", &saveptr);
  }
  return 0;
}

static char *strip_priority(const char *s) {
  if (!s)
    return strdup("");
  char buf[MAX_LINE];
  strncpy(buf, s, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  char out[MAX_LINE] = "";
  char *saveptr;
  char *tok = strtok_r(buf, " \t", &saveptr);
  int first = 1;
  while (tok) {
    int is_pri = (tok[0] == '(' && tok[2] == ')' &&
                  isupper((unsigned char)tok[1]) && tok[3] == '\0');
    if (!is_pri) {
      if (!first)
        strcat(out, " ");
      strcat(out, tok);
      first = 0;
    }
    tok = strtok_r(NULL, " \t", &saveptr);
  }
  return strdup(out);
}

static char *set_priority(const char *s, char p) {
  if (!s)
    return strdup("");
  char buf[MAX_LINE];
  strncpy(buf, s, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  char out[MAX_LINE] = "";
  char *saveptr;
  char *tok = strtok_r(buf, " \t", &saveptr);
  int first = 1;
  int found = 0;
  char pri_str[8];
  snprintf(pri_str, sizeof(pri_str), "(%c)", (char)toupper((unsigned char)p));

  while (tok) {
    int is_pri = (tok[0] == '(' && tok[2] == ')' &&
                  isupper((unsigned char)tok[1]) && tok[3] == '\0');
    if (!first)
      strcat(out, " ");
    first = 0;
    if (is_pri) {
      strcat(out, pri_str);
      found = 1;
    } else {
      strcat(out, tok);
    }
    tok = strtok_r(NULL, " \t", &saveptr);
  }

  if (!found) {
    char temp[MAX_LINE];
    snprintf(temp, sizeof(temp), "%s %s", pri_str, out);
    return strdup(temp);
  }
  return strdup(out);
}

static char *find_tag(const char *s, const char *key) {
  char keybuf[32];
  snprintf(keybuf, sizeof(keybuf), "%s:", key);
  size_t klen = strlen(keybuf);

  const char *p = s;
  while (*p) {
    while (*p == ' ' || *p == '\t')
      p++;
    const char *start = p;
    while (*p && *p != ' ' && *p != '\t')
      p++;
    size_t toklen = (size_t)(p - start);
    if (toklen >= klen && strncmp(start, keybuf, klen) == 0) {
      char *val = malloc(toklen - klen + 1);
      memcpy(val, start + klen, toklen - klen);
      val[toklen - klen] = '\0';
      return val;
    }
  }
  return NULL;
}

static char *set_tag(const char *s, const char *key, const char *newval) {
  char keybuf[32];
  snprintf(keybuf, sizeof(keybuf), "%s:", key);
  size_t klen = strlen(keybuf);

  char buf[MAX_LINE];
  buf[0] = '\0';
  int found = 0;

  char tmp[MAX_LINE];
  strncpy(tmp, s, sizeof(tmp) - 1);
  tmp[sizeof(tmp) - 1] = '\0';

  char *saveptr;
  char *tok = strtok_r(tmp, " \t", &saveptr);
  int first = 1;
  while (tok) {
    if (!first)
      strcat(buf, " ");
    first = 0;
    if (strncmp(tok, keybuf, klen) == 0) {
      strcat(buf, keybuf);
      strcat(buf, newval);
      found = 1;
    } else {
      strcat(buf, tok);
    }
    tok = strtok_r(NULL, " \t", &saveptr);
  }
  if (!found) {
    if (buf[0])
      strcat(buf, " ");
    strcat(buf, keybuf);
    strcat(buf, newval);
  }
  return strdup(buf);
}

static int mark_done(int idx) {
  const char *orig = tasks[idx].text;
  if (line_is_done(orig))
    return -1;

  char today[16];
  today_str(today, sizeof(today));

  char *rec = find_tag(orig, "rec");
  char *due = find_tag(orig, "due");

  if (rec) {
    int strict = (rec[0] == '+');
    const char *num_start = strict ? rec + 1 : rec;
    int n = atoi(num_start);
    char unit = num_start[strlen(num_start) - 1];

    struct tm base;
    int have_base = 0;
    if (strict && due && parse_date(due, &base) == 0) {
      have_base = 1;
    }
    if (!have_base) {
      parse_date(today, &base);
    }
    add_interval(&base, n, unit);

    char new_due[16];
    strftime(new_due, sizeof(new_due), "%Y-%m-%d", &base);

    char *clone = due ? set_tag(orig, "due", new_due) : NULL;
    if (!clone) {
      char withdue[MAX_LINE];
      snprintf(withdue, sizeof(withdue), "%s due:%s", orig, new_due);
      clone = strdup(withdue);
    }
    if (task_count < MAX_TASKS) {
      tasks[task_count].text = clone;
      task_count++;
    } else {
      free(clone);
    }
  }
  free(rec);
  free(due);

  char *stripped = strip_priority(orig);
  char newline[MAX_LINE];
  snprintf(newline, sizeof(newline), "x %s %s", today, stripped);
  free(stripped);

  free(tasks[idx].text);
  tasks[idx].text = strdup(newline);
  return 0;
}

static int mark_undone(int idx) {
  const char *s = tasks[idx].text;
  if (!line_is_done(s))
    return -1;
  const char *p = s + 2;
  while (*p && *p != ' ')
    p++;
  if (*p == ' ')
    p++;
  char *rest = strdup(p);
  free(tasks[idx].text);
  tasks[idx].text = rest;
  return 0;
}

/* ================= matching & sorting ================= */

static int line_matches_term(const char *line, const char *term) {
  if (term[0] == '+' || term[0] == '@' || term[0] == '!') {
    char buf[MAX_LINE];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *saveptr;
    char *tok = strtok_r(buf, " \t", &saveptr);
    while (tok) {
      if (strcmp(tok, term) == 0)
        return 1;
      tok = strtok_r(NULL, " \t", &saveptr);
    }
    return 0;
  }
  if (strncmp(term, "pri:", 4) == 0) {
    char want = (char)toupper((unsigned char)term[4]);
    char p = line_priority(line);
    return p == want;
  }
  size_t tlen = strlen(term);
  if (tlen == 0)
    return 1;
  for (const char *p = line; *p; p++)
    if (strncasecmp(p, term, tlen) == 0)
      return 1;
  return 0;
}

static int line_matches_all(const char *line, char **terms, int nterms) {
  for (int i = 0; i < nterms; i++)
    if (!line_matches_term(line, terms[i]))
      return 0;
  return 1;
}

static int cmp_task_text_ci(const void *a, const void *b) {
  int ia = *(const int *)a, ib = *(const int *)b;
  return strcasecmp(tasks[ia].text, tasks[ib].text);
}

/* ================= CLI commands ================= */

static int g_force = 0;
static int g_json = 0;
static char g_todo_path[PATH_LEN];
static char g_done_path[PATH_LEN];

static int parse_task_num(const char *s) {
  char *end;
  long n = strtol(s, &end, 10);
  if (*end != '\0' || n < 1 || n > task_count) {
    fprintf(stderr, "tuxedo: no task %s\n", s);
    exit(1);
  }
  return (int)n - 1;
}

static char *join_args(int argc, char **argv, int from) {
  size_t len = 1;
  for (int i = from; i < argc; i++)
    len += strlen(argv[i]) + 1;
  char *out = malloc(len);
  out[0] = '\0';
  for (int i = from; i < argc; i++) {
    if (i > from)
      strcat(out, " ");
    strcat(out, argv[i]);
  }
  return out;
}

static void cmd_add(int argc, char **argv) {
  if (argc < 1) {
    fprintf(stderr, "tuxedo: add TEXT...\n");
    exit(1);
  }
  char *text = join_args(argc, argv, 0);
  char today[16];
  today_str(today, sizeof(today));

  char final_line[MAX_LINE];
  char first_tok[16] = {0};
  sscanf(text, "%15s", first_tok);

  if (looks_like_date(first_tok)) {
    snprintf(final_line, sizeof(final_line), "%s", text);
  } else {
    snprintf(final_line, sizeof(final_line), "%s %s", today, text);
  }
  free(text);

  if (task_count >= MAX_TASKS) {
    fprintf(stderr, "tuxedo: task limit reached\n");
    exit(1);
  }
  tasks[task_count].text = strdup(final_line);
  task_count++;
  save_tasks(g_todo_path);
  printf("%d %s\nTODO: 1 added.\n", task_count, tasks[task_count - 1].text);
}

static void cmd_do(int argc, char **argv) {
  if (argc < 1) {
    fprintf(stderr, "tuxedo: do N...\n");
    exit(1);
  }
  for (int i = 0; i < argc; i++) {
    int idx = parse_task_num(argv[i]);
    if (mark_done(idx) == 0) {
      printf("%d %s\nTODO: %d marked as done.\n", idx + 1, tasks[idx].text,
             idx + 1);
    }
  }
  save_tasks(g_todo_path);
}

static void cmd_archive(void) {
  char *done_lines[MAX_TASKS];
  int done_count = 0;
  for (int i = 0; i < task_count; i++)
    if (line_is_done(tasks[i].text))
      done_lines[done_count++] = tasks[i].text;

  if (done_count == 0) {
    printf("TODO: No tasks were archived.\n");
    return;
  }
  append_lines(g_done_path, done_lines, done_count);

  int w = 0;
  for (int i = 0; i < task_count; i++) {
    if (line_is_done(tasks[i].text)) {
      free(tasks[i].text);
      continue;
    }
    tasks[w++] = tasks[i];
  }
  task_count = w;
  save_tasks(g_todo_path);
  printf("TODO: %d tasks archived.\n", done_count);
}

static void collect_unique_tags(char prefix, char out[][64], int *count) {
  *count = 0;
  for (int i = 0; i < task_count; i++) {
    char buf[MAX_LINE];
    strncpy(buf, tasks[i].text, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *saveptr;
    char *tok = strtok_r(buf, " \t", &saveptr);
    while (tok) {
      if (tok[0] == prefix && tok[1]) {
        int dup = 0;
        for (int j = 0; j < *count; j++)
          if (strcmp(out[j], tok) == 0) {
            dup = 1;
            break;
          }
        if (!dup && *count < 512)
          strncpy(out[(*count)++], tok, 63);
      }
      tok = strtok_r(NULL, " \t", &saveptr);
    }
  }
}

static int cmp_str(const void *a, const void *b) {
  return strcasecmp((const char *)a, (const char *)b);
}

/* ================= TUI (ncurses) ================= */

static char tui_filter[256] = "";

#define CP_PROJECT 1
#define CP_CONTEXT 2
#define CP_ATTR 3
#define CP_PRI_A 4
#define CP_PRI_B 5
#define CP_PRI_C 6
#define CP_PRI_D 7
#define CP_PRI_OTHER 8
#define CP_HEADER 9
#define CP_OVERDUE 10
#define CP_MODE 11
#define CP_SIDEBAR 12
#define CP_MAIN_BG 13
#define CP_HELP_KEY 14
#define CP_STATUS_BAR 15

#define CP_SIDEBAR_HEADER 16
#define CP_SIDEBAR_PROJECT 17
#define CP_SIDEBAR_CONTEXT 18
#define CP_SIDEBAR_PRI_A 19
#define CP_SIDEBAR_PRI_B 20
#define CP_SIDEBAR_PRI_C 21
#define CP_SIDEBAR_PRI_OTHER 22
#define CP_DARK_GRAY_LINE 23
#define CP_REC 24
#define CP_VIOLET_ATTR 25

#define COLOR_DARK_BG 26
#define COLOR_SIDEBAR_BG 27
#define COLOR_STATUS_BG 28

#define COLOR_SAGE_GREEN 29
#define COLOR_MUTED_AMBER 30
#define COLOR_ROSE_RED 31
#define COLOR_MINT_TEAL 32
#define COLOR_SKY_BLUE 33
#define COLOR_DARK_GRAY 34
#define COLOR_VIOLET 35

#define CP_RADAR_1 36
#define CP_RADAR_2 37
#define CP_RADAR_3 38
#define CP_RADAR_4 39
#define CP_RADAR_5 40
#define CP_RADAR_6 41

static void tui_init_colors(void) {
  start_color();
  use_default_colors();

  if (can_change_color()) {
    init_color(COLOR_DARK_BG, (int)(21 * 1000 / 255), (int)(23 * 1000 / 255),
               (int)(29 * 1000 / 255));
    init_color(COLOR_SIDEBAR_BG, (int)(27 * 1000 / 255), (int)(31 * 1000 / 255),
               (int)(38 * 1000 / 255));
    init_color(COLOR_STATUS_BG, (int)(24 * 1000 / 255), (int)(30 * 1000 / 255),
               (int)(40 * 1000 / 255));

    init_color(COLOR_SAGE_GREEN, (int)(114 * 1000 / 255),
               (int)(163 * 1000 / 255), (int)(140 * 1000 / 255));
    init_color(COLOR_MUTED_AMBER, (int)(217 * 1000 / 255),
               (int)(155 * 1000 / 255), (int)(101 * 1000 / 255));
    init_color(COLOR_ROSE_RED, (int)(217 * 1000 / 255), (int)(107 * 1000 / 255),
               (int)(110 * 1000 / 255));
    init_color(COLOR_MINT_TEAL, (int)(114 * 1000 / 255),
               (int)(163 * 1000 / 255), (int)(140 * 1000 / 255));
    init_color(COLOR_SKY_BLUE, (int)(101 * 1000 / 255), (int)(141 * 1000 / 255),
               (int)(185 * 1000 / 255));
    init_color(COLOR_DARK_GRAY, (int)(100 * 1000 / 255),
               (int)(100 * 1000 / 255), (int)(100 * 1000 / 255));
    init_color(COLOR_VIOLET, (int)(175 * 1000 / 255), (int)(130 * 1000 / 255),
               (int)(220 * 1000 / 255));

    init_pair(CP_MAIN_BG, COLOR_WHITE, COLOR_DARK_BG);
    init_pair(CP_SIDEBAR, COLOR_WHITE, COLOR_SIDEBAR_BG);
    init_pair(CP_STATUS_BAR, COLOR_WHITE, COLOR_STATUS_BG);
    init_pair(CP_MODE, COLOR_BLACK, COLOR_SKY_BLUE);

    init_pair(CP_PROJECT, COLOR_SAGE_GREEN, -1);
    init_pair(CP_CONTEXT, COLOR_MUTED_AMBER, -1);
    init_pair(CP_ATTR, COLOR_ROSE_RED, -1);
    init_pair(CP_REC, COLOR_DARK_GRAY, -1);
    init_pair(CP_VIOLET_ATTR, COLOR_VIOLET, -1);

    init_pair(CP_PRI_A, COLOR_ROSE_RED, -1);
    init_pair(CP_PRI_B, COLOR_MUTED_AMBER, -1);
    init_pair(CP_PRI_C, COLOR_MINT_TEAL, -1);
    init_pair(CP_PRI_D, COLOR_SKY_BLUE, -1);
    init_pair(CP_PRI_OTHER, COLOR_WHITE, -1);

    init_pair(CP_HEADER, COLOR_WHITE, COLOR_SIDEBAR_BG);
    init_pair(CP_DARK_GRAY_LINE, COLOR_DARK_GRAY, -1);
    init_pair(CP_OVERDUE, COLOR_ROSE_RED, -1);
    init_pair(CP_HELP_KEY, COLOR_MUTED_AMBER, -1);

    init_pair(CP_SIDEBAR_HEADER, COLOR_SKY_BLUE, COLOR_SIDEBAR_BG);
    init_pair(CP_SIDEBAR_PROJECT, COLOR_SAGE_GREEN, COLOR_SIDEBAR_BG);
    init_pair(CP_SIDEBAR_CONTEXT, COLOR_MUTED_AMBER, COLOR_SIDEBAR_BG);
    init_pair(CP_SIDEBAR_PRI_A, COLOR_ROSE_RED, COLOR_SIDEBAR_BG);
    init_pair(CP_SIDEBAR_PRI_B, COLOR_MUTED_AMBER, COLOR_SIDEBAR_BG);
    init_pair(CP_SIDEBAR_PRI_C, COLOR_MINT_TEAL, COLOR_SIDEBAR_BG);
    init_pair(CP_SIDEBAR_PRI_OTHER, COLOR_WHITE, COLOR_SIDEBAR_BG);

    init_pair(CP_RADAR_1, COLOR_WHITE, 236);
    init_pair(CP_RADAR_2, COLOR_WHITE, 239);
    init_pair(CP_RADAR_3, COLOR_WHITE, 243);
    init_pair(CP_RADAR_4, COLOR_WHITE, 247);
    init_pair(CP_RADAR_5, COLOR_WHITE, 250);
    init_pair(CP_RADAR_6, COLOR_WHITE, 254);
  } else {
    init_pair(CP_MAIN_BG, COLOR_WHITE, COLOR_BLACK);
    init_pair(CP_SIDEBAR, COLOR_WHITE, COLOR_BLUE);
    init_pair(CP_STATUS_BAR, COLOR_WHITE, COLOR_BLACK);
    init_pair(CP_MODE, COLOR_BLACK, COLOR_CYAN);

    init_pair(CP_PROJECT, COLOR_GREEN, -1);
    init_pair(CP_CONTEXT, COLOR_YELLOW, -1);
    init_pair(CP_ATTR, COLOR_MAGENTA, -1);
    init_pair(CP_REC, COLOR_WHITE, -1);
    init_pair(CP_VIOLET_ATTR, COLOR_MAGENTA, -1);

    init_pair(CP_PRI_A, COLOR_RED, -1);
    init_pair(CP_PRI_B, COLOR_YELLOW, -1);
    init_pair(CP_PRI_C, COLOR_GREEN, -1);
    init_pair(CP_PRI_D, COLOR_CYAN, -1);
    init_pair(CP_PRI_OTHER, COLOR_WHITE, -1);

    init_pair(CP_HEADER, COLOR_WHITE, COLOR_BLUE);
    init_pair(CP_DARK_GRAY_LINE, COLOR_BLACK, -1);
    init_pair(CP_OVERDUE, COLOR_RED, -1);
    init_pair(CP_HELP_KEY, COLOR_YELLOW, -1);

    init_pair(CP_SIDEBAR_HEADER, COLOR_CYAN, COLOR_BLUE);
    init_pair(CP_SIDEBAR_PROJECT, COLOR_GREEN, COLOR_BLUE);
    init_pair(CP_SIDEBAR_CONTEXT, COLOR_YELLOW, COLOR_BLUE);
    init_pair(CP_SIDEBAR_PRI_A, COLOR_RED, COLOR_BLUE);
    init_pair(CP_SIDEBAR_PRI_B, COLOR_YELLOW, COLOR_BLUE);
    init_pair(CP_SIDEBAR_PRI_C, COLOR_GREEN, COLOR_BLUE);
    init_pair(CP_SIDEBAR_PRI_OTHER, COLOR_WHITE, COLOR_BLUE);

    init_pair(CP_RADAR_1, COLOR_WHITE, COLOR_BLUE);
    init_pair(CP_RADAR_2, COLOR_WHITE, COLOR_BLUE);
    init_pair(CP_RADAR_3, COLOR_WHITE, COLOR_CYAN);
    init_pair(CP_RADAR_4, COLOR_WHITE, COLOR_CYAN);
    init_pair(CP_RADAR_5, COLOR_WHITE, COLOR_WHITE);
    init_pair(CP_RADAR_6, COLOR_WHITE, COLOR_WHITE);
  }
}

static int priority_color_pair(char p) {
  switch (p) {
  case 'A':
    return CP_PRI_A;
  case 'B':
    return CP_PRI_B;
  case 'C':
    return CP_PRI_C;
  case 'D':
    return CP_PRI_D;
  default:
    return CP_PRI_OTHER;
  }
}

static int sidebar_priority_color_pair(char p) {
  switch (p) {
  case 'A':
    return CP_SIDEBAR_PRI_A;
  case 'B':
    return CP_SIDEBAR_PRI_B;
  case 'C':
    return CP_SIDEBAR_PRI_C;
  default:
    return CP_SIDEBAR_PRI_OTHER;
  }
}

static int token_is_kv_tag(const char *tok) {
  const char *colon = strchr(tok, ':');
  if (!colon || colon == tok || colon[1] == '\0')
    return 0;
  for (const char *p = tok; p < colon; p++)
    if (!isalpha((unsigned char)*p))
      return 0;
  return 1;
}

static int is_overdue(int idx) {
  if (line_is_done(tasks[idx].text))
    return 0;
  char *due = find_tag(tasks[idx].text, "due");
  if (!due)
    return 0;
  char today[16];
  today_str(today, sizeof(today));
  int overdue = strcmp(due, today) < 0;
  free(due);
  return overdue;
}

static void render_colored_text_line(int y, int xoff, int width,
                                     const char *text, int is_cursor,
                                     int overdue, int done) {
  char tmp[MAX_LINE];
  strncpy(tmp, text, sizeof(tmp) - 1);
  tmp[sizeof(tmp) - 1] = '\0';

  int col = xoff;
  int budget = width;
  if (done && !is_cursor)
    attron(A_DIM);

  if (is_cursor) {
    attron(A_REVERSE);
  }

  char *saveptr;
  char *tok = strtok_r(tmp, " ", &saveptr);
  int first = 1;
  while (tok && budget > 0) {
    if (!first) {
      if (budget > 0) {
        mvaddch(y, col, ' ');
        col++;
        budget--;
      }
    }
    first = 0;

    int pair = 0;
    if (!is_cursor) {
      if (tok[0] == '(' && strlen(tok) == 3 && isupper((unsigned char)tok[1]) &&
          tok[2] == ')')
        pair = priority_color_pair(tok[1]);
      else if (tok[0] == '+' && tok[1] != '\0')
        pair = CP_PROJECT;
      else if (tok[0] == '@' && tok[1] != '\0')
        pair = CP_CONTEXT;
      else if (tok[0] == '!' && tok[1] != '\0' && tok[2] == '\0')
        pair = CP_VIOLET_ATTR;
      else if (strncmp(tok, "rec:", 4) == 0 || strncmp(tok, "+rec:", 5) == 0)
        pair = CP_REC;
      else if (token_is_kv_tag(tok))
        pair = CP_ATTR;
    }

    int tlen = (int)strlen(tok);
    int show = tlen < budget ? tlen : budget;

    if (!is_cursor && overdue) {
      attron(COLOR_PAIR(CP_OVERDUE));
    } else if (!is_cursor && pair && !done) {
      attron(COLOR_PAIR(pair));
    }

    mvprintw(y, col, "%.*s", show, tok);

    if (!is_cursor && overdue) {
      attroff(COLOR_PAIR(CP_OVERDUE));
    } else if (!is_cursor && pair && !done) {
      attroff(COLOR_PAIR(pair));
    }

    col += show;
    budget -= show;

    tok = strtok_r(NULL, " ", &saveptr);
  }

  while (budget > 0) {
    mvaddch(y, col, ' ');
    col++;
    budget--;
  }

  if (is_cursor) {
    attroff(A_REVERSE);
  }
  if (done && !is_cursor)
    attroff(A_DIM);
}

static void render_task_row(int y, int xoff, int width, int task_idx,
                            int is_cursor, int line_num_display) {
  const char *text = tasks[task_idx].text;
  char prefix_buf[16];
  snprintf(prefix_buf, sizeof(prefix_buf), "%3d ", line_num_display);

  int done = line_is_done(text);
  int overdue = is_overdue(task_idx);

  if (is_cursor) {
    attron(A_REVERSE);
  }

  if (overdue && !is_cursor)
    attron(COLOR_PAIR(CP_OVERDUE));
  mvprintw(y, xoff, "%s", prefix_buf);
  if (overdue && !is_cursor)
    attroff(COLOR_PAIR(CP_OVERDUE));

  render_colored_text_line(y, xoff + 4, width - 4, text, is_cursor, overdue,
                           done);

  if (is_cursor) {
    attroff(A_REVERSE);
  }
}

static int tui_show_done = 0;

#define UNDO_STACK_SIZE 50
typedef struct {
  char *texts[MAX_TASKS];
  int count;
} UndoSnapshot;
static UndoSnapshot undo_stack[UNDO_STACK_SIZE];
static int undo_top = 0;

static void push_undo(void) {
  if (undo_top >= UNDO_STACK_SIZE) {
    for (int i = 0; i < undo_stack[0].count; i++)
      free(undo_stack[0].texts[i]);
    memmove(&undo_stack[0], &undo_stack[1],
            sizeof(UndoSnapshot) * (UNDO_STACK_SIZE - 1));
    undo_top--;
  }
  UndoSnapshot *s = &undo_stack[undo_top];
  s->count = task_count;
  for (int i = 0; i < task_count; i++)
    s->texts[i] = strdup(tasks[i].text);
  undo_top++;
}

static int pop_undo(void) {
  if (undo_top == 0)
    return 0;
  undo_top--;
  UndoSnapshot *s = &undo_stack[undo_top];
  for (int i = 0; i < task_count; i++)
    free(tasks[i].text);
  task_count = s->count;
  for (int i = 0; i < task_count; i++)
    tasks[i].text = s->texts[i];
  s->count = 0;
  return 1;
}

static void tui_visible_indices(int indices[MAX_TASKS], int *n) {
  *n = 0;
  for (int i = 0; i < task_count; i++) {
    if (!tui_show_done && line_is_done(tasks[i].text))
      continue;
    if (tui_filter[0] == '\0' || line_matches_term(tasks[i].text, tui_filter))
      indices[(*n)++] = i;
  }
}

enum {
  SORT_FILE = 0,
  SORT_PRIORITY,
  SORT_DUE,
  SORT_RECUR,
  SORT_ATTR,
  SORT_MODE_COUNT
};
static const char *sort_mode_name[SORT_MODE_COUNT] = {
    "File Order", "Priority", "Due Date", "Recurring", "Attribute"};

typedef struct {
  int is_header;
  int task_idx;
  char header[32];
  int header_color;
} DisplayRow;

static int due_bucket(const char *due_str, struct tm *out_date) {
  if (!due_str)
    return 4;
  if (parse_date(due_str, out_date) != 0)
    return 4;
  char today[16];
  today_str(today, sizeof(today));
  struct tm tnow;
  parse_date(today, &tnow);
  time_t t_due = mktime(out_date);
  time_t t_now = mktime(&tnow);
  double diff_days = difftime(t_due, t_now) / 86400.0;
  if (diff_days < -0.5)
    return 0;
  if (diff_days < 0.5)
    return 1;
  if (diff_days < 6.5)
    return 2;
  return 3;
}

static int cmp_by_due(const void *a, const void *b) {
  int ia = *(const int *)a, ib = *(const int *)b;
  char *da = find_tag(tasks[ia].text, "due");
  char *db = find_tag(tasks[ib].text, "due");
  int r;
  if (da && db)
    r = strcmp(da, db);
  else if (da)
    r = -1;
  else if (db)
    r = 1;
  else
    r = ia - ib;
  free(da);
  free(db);
  return r;
}

static void append_sorted_rows(DisplayRow rows[], int *nrows, int *in_indices,
                               int n, int sort_mode) {
  if (sort_mode == SORT_FILE) {
    for (int i = 0; i < n && *nrows < MAX_TASKS; i++) {
      rows[*nrows].is_header = 0;
      rows[*nrows].task_idx = in_indices[i];
      (*nrows)++;
    }
    return;
  }
  if (sort_mode == SORT_PRIORITY) {
    for (int p = 0; p < 27; p++) {
      char want = p < 26 ? (char)('A' + p) : 0;
      int any = 0;
      for (int i = 0; i < n; i++)
        if (line_priority(tasks[in_indices[i]].text) == want)
          any = 1;
      if (!any)
        continue;
      if (*nrows < MAX_TASKS) {
        rows[*nrows].is_header = 1;
        rows[*nrows].header_color = CP_HEADER;
        snprintf(rows[*nrows].header, sizeof(rows[0].header),
                 want ? "PRIORITY %c" : "NO PRIORITY", want);
        (*nrows)++;
      }
      for (int i = 0; i < n; i++) {
        if (line_priority(tasks[in_indices[i]].text) != want)
          continue;
        if (*nrows < MAX_TASKS) {
          rows[*nrows].is_header = 0;
          rows[*nrows].task_idx = in_indices[i];
          (*nrows)++;
        }
      }
    }
    return;
  }
  if (sort_mode == SORT_DUE) {
    static const char *bucket_name[5] = {"OVERDUE", "TODAY", "THIS WEEK",
                                         "LATER", "NO DUE DATE"};
    int sorted[MAX_TASKS];
    memcpy(sorted, in_indices, sizeof(int) * (size_t)n);
    qsort(sorted, n, sizeof(int), cmp_by_due);

    for (int b = 0; b < 5; b++) {
      int any = 0;
      for (int i = 0; i < n; i++) {
        char *due = find_tag(tasks[sorted[i]].text, "due");
        struct tm tmp;
        int bk = due_bucket(due, &tmp);
        free(due);
        if (bk == b)
          any = 1;
      }
      if (!any)
        continue;
      if (*nrows < MAX_TASKS) {
        rows[*nrows].is_header = 1;
        rows[*nrows].header_color = (b == 0) ? CP_OVERDUE : CP_HEADER;
        snprintf(rows[*nrows].header, sizeof(rows[0].header), "%s",
                 bucket_name[b]);
        (*nrows)++;
      }
      for (int i = 0; i < n; i++) {
        char *due = find_tag(tasks[sorted[i]].text, "due");
        struct tm tmp;
        int bk = due_bucket(due, &tmp);
        free(due);
        if (bk != b)
          continue;
        if (*nrows < MAX_TASKS) {
          rows[*nrows].is_header = 0;
          rows[*nrows].task_idx = sorted[i];
          (*nrows)++;
        }
      }
    }
    return;
  }
  if (sort_mode == SORT_RECUR) {
    int rec_idx[MAX_TASKS], rec_n = 0;
    int other_idx[MAX_TASKS], other_n = 0;
    for (int i = 0; i < n; i++) {
      char *rec = find_tag(tasks[in_indices[i]].text, "rec");
      if (rec) {
        rec_idx[rec_n++] = in_indices[i];
        free(rec);
      } else
        other_idx[other_n++] = in_indices[i];
    }
    qsort(rec_idx, rec_n, sizeof(int), cmp_by_due);

    if (rec_n > 0 && *nrows < MAX_TASKS) {
      rows[*nrows].is_header = 1;
      rows[*nrows].header_color = CP_HEADER;
      snprintf(rows[*nrows].header, sizeof(rows[0].header), "RECURRING");
      (*nrows)++;
      for (int i = 0; i < rec_n && *nrows < MAX_TASKS; i++) {
        rows[*nrows].is_header = 0;
        rows[*nrows].task_idx = rec_idx[i];
        (*nrows)++;
      }
    }
    if (other_n > 0 && *nrows < MAX_TASKS) {
      rows[*nrows].is_header = 1;
      rows[*nrows].header_color = CP_HEADER;
      snprintf(rows[*nrows].header, sizeof(rows[0].header), "ONE-OFF");
      (*nrows)++;
      for (int i = 0; i < other_n && *nrows < MAX_TASKS; i++) {
        rows[*nrows].is_header = 0;
        rows[*nrows].task_idx = other_idx[i];
        (*nrows)++;
      }
    }
    return;
  }
  if (sort_mode == SORT_ATTR) {
    for (int a = 0; a < NUM_ATTRS; a++) {
      char attr_tag[8];
      snprintf(attr_tag, sizeof(attr_tag), "!%c", default_attrs[a].code);
      int any = 0;
      for (int i = 0; i < n; i++) {
        if (line_matches_term(tasks[in_indices[i]].text, attr_tag))
          any = 1;
      }
      if (!any)
        continue;
      if (*nrows < MAX_TASKS) {
        rows[*nrows].is_header = 1;
        rows[*nrows].header_color = CP_HEADER;
        snprintf(rows[*nrows].header, sizeof(rows[0].header), "ATTRIBUTE !%c",
                 default_attrs[a].code);
        (*nrows)++;
      }
      for (int i = 0; i < n; i++) {
        if (!line_matches_term(tasks[in_indices[i]].text, attr_tag))
          continue;
        if (*nrows < MAX_TASKS) {
          rows[*nrows].is_header = 0;
          rows[*nrows].task_idx = in_indices[i];
          (*nrows)++;
        }
      }
    }
    int any_none = 0;
    for (int i = 0; i < n; i++) {
      int has_any_attr = 0;
      for (int a = 0; a < NUM_ATTRS; a++) {
        char attr_tag[8];
        snprintf(attr_tag, sizeof(attr_tag), "!%c", default_attrs[a].code);
        if (line_matches_term(tasks[in_indices[i]].text, attr_tag))
          has_any_attr = 1;
      }
      if (!has_any_attr)
        any_none = 1;
    }
    if (any_none && *nrows < MAX_TASKS) {
      rows[*nrows].is_header = 1;
      rows[*nrows].header_color = CP_HEADER;
      snprintf(rows[*nrows].header, sizeof(rows[0].header), "NO ATTRIBUTE");
      (*nrows)++;
      for (int i = 0; i < n; i++) {
        int has_any_attr = 0;
        for (int a = 0; a < NUM_ATTRS; a++) {
          char attr_tag[8];
          snprintf(attr_tag, sizeof(attr_tag), "!%c", default_attrs[a].code);
          if (line_matches_term(tasks[in_indices[i]].text, attr_tag))
            has_any_attr = 1;
        }
        if (!has_any_attr && *nrows < MAX_TASKS) {
          rows[*nrows].is_header = 0;
          rows[*nrows].task_idx = in_indices[i];
          (*nrows)++;
        }
      }
    }
    return;
  }
}

static void build_display_rows(DisplayRow rows[], int *nrows, int sort_mode) {
  int indices[MAX_TASKS], n;
  tui_visible_indices(indices, &n);
  *nrows = 0;

  int overdue_idx[MAX_TASKS], od_n = 0;
  int rest_idx[MAX_TASKS], rest_n = 0;
  for (int i = 0; i < n; i++) {
    if (is_overdue(indices[i]))
      overdue_idx[od_n++] = indices[i];
    else
      rest_idx[rest_n++] = indices[i];
  }

  if (od_n > 0) {
    qsort(overdue_idx, od_n, sizeof(int), cmp_by_due);
    rows[*nrows].is_header = 1;
    rows[*nrows].header_color = CP_OVERDUE;
    snprintf(rows[*nrows].header, sizeof(rows[0].header), "OVERDUE");
    (*nrows)++;
    for (int i = 0; i < od_n && *nrows < MAX_TASKS; i++) {
      rows[*nrows].is_header = 0;
      rows[*nrows].task_idx = overdue_idx[i];
      (*nrows)++;
    }
  }
  append_sorted_rows(rows, nrows, rest_idx, rest_n, sort_mode);
}

static int display_row_for_task_cursor(DisplayRow rows[], int nrows,
                                       int task_cursor) {
  int seen = -1;
  for (int r = 0; r < nrows; r++) {
    if (!rows[r].is_header) {
      seen++;
      if (seen == task_cursor)
        return r;
    }
  }
  return -1;
}

static int count_selectable(DisplayRow rows[], int nrows) {
  int c = 0;
  for (int r = 0; r < nrows; r++)
    if (!rows[r].is_header)
      c++;
  return c;
}

typedef struct {
  char tag[64];
  int type; /* 0: project, 1: context, 2: priority, 3: attribute */
  int count;
} SidebarItem;

static int build_sidebar_items(SidebarItem items[], int max_items) {
  static char projs[512][64], ctxs[512][64];
  int np, nc;
  collect_unique_tags('+', projs, &np);
  collect_unique_tags('@', ctxs, &nc);
  qsort(projs, np, 64, cmp_str);
  qsort(ctxs, nc, 64, cmp_str);

  int n = 0;
  for (int p = 0; p < 26 && n < max_items; p++) {
    char want = (char)('A' + p);
    int cnt = 0;
    for (int t = 0; t < task_count; t++) {
      if (line_priority(tasks[t].text) == want)
        cnt++;
    }
    if (cnt > 0) {
      snprintf(items[n].tag, sizeof(items[n].tag), "pri:%c", want);
      items[n].type = 2;
      items[n].count = cnt;
      n++;
    }
  }

  for (int a = 0; a < NUM_ATTRS && n < max_items; a++) {
    snprintf(items[n].tag, sizeof(items[n].tag), "!%c", default_attrs[a].code);
    items[n].type = 3;
    items[n].count = 0;
    for (int t = 0; t < task_count; t++) {
      if (line_matches_term(tasks[t].text, items[n].tag))
        items[n].count++;
    }
    n++;
  }

  for (int i = 0; i < np && n < max_items; i++) {
    strncpy(items[n].tag, projs[i], sizeof(items[n].tag) - 1);
    items[n].type = 0;
    items[n].count = 0;
    for (int t = 0; t < task_count; t++) {
      if (line_matches_term(tasks[t].text, items[n].tag))
        items[n].count++;
    }
    n++;
  }
  for (int i = 0; i < nc && n < max_items; i++) {
    strncpy(items[n].tag, ctxs[i], sizeof(items[n].tag) - 1);
    items[n].type = 1;
    items[n].count = 0;
    for (int t = 0; t < task_count; t++) {
      if (line_matches_term(tasks[t].text, items[n].tag))
        items[n].count++;
    }
    n++;
  }
  return n;
}

static void render_sidebar(int h, SidebarItem items[], int item_count,
                           int sidebar_focus, int sidebar_cursor,
                           int sidebar_x) {
  attron(COLOR_PAIR(CP_SIDEBAR));
  for (int y = 1; y < h - 3; y++) {
    move(y, sidebar_x);
    for (int x = sidebar_x; x < sidebar_x + SIDEBAR_WIDTH; x++)
      addch(' ');
  }

  int row = 1;
  attron(COLOR_PAIR(CP_SIDEBAR_HEADER) | A_BOLD);
  mvprintw(row++, sidebar_x + 1, "FILTERS");
  row++;
  mvprintw(row++, sidebar_x + 1, "PRIORITIES");
  attroff(COLOR_PAIR(CP_SIDEBAR_HEADER) | A_BOLD);

  int idx = 0;
  for (; idx < item_count && items[idx].type == 2; idx++) {
    if (row >= h - 3)
      break;
    int is_sel = sidebar_focus && sidebar_cursor == idx;
    char count_str[16];
    snprintf(count_str, sizeof(count_str), "%d", items[idx].count);
    int tag_w = SIDEBAR_WIDTH - 2 - (int)strlen(count_str) - 1;
    if (tag_w < 1)
      tag_w = 1;

    char pri_char = items[idx].tag[4];
    int p_pair = sidebar_priority_color_pair(pri_char);

    attron(COLOR_PAIR(p_pair));
    if (is_sel)
      attron(A_REVERSE);
    mvprintw(row, sidebar_x + 2, "%-*.*s", tag_w, tag_w, items[idx].tag);
    if (is_sel)
      attroff(A_REVERSE);
    attroff(COLOR_PAIR(p_pair));

    attron(COLOR_PAIR(CP_SIDEBAR));
    if (is_sel)
      attron(A_REVERSE);
    mvprintw(row, sidebar_x + SIDEBAR_WIDTH - 1 - (int)strlen(count_str), "%s",
             count_str);
    if (is_sel)
      attroff(A_REVERSE);
    attroff(COLOR_PAIR(CP_SIDEBAR));

    row++;
  }

  row++;
  if (row < h - 3) {
    attron(COLOR_PAIR(CP_SIDEBAR_HEADER) | A_BOLD);
    mvprintw(row++, sidebar_x + 1, "ATTRIBUTES");
    attroff(COLOR_PAIR(CP_SIDEBAR_HEADER) | A_BOLD);
  }
  for (; idx < item_count && items[idx].type == 3; idx++) {
    if (row >= h - 3)
      break;
    int is_sel = sidebar_focus && sidebar_cursor == idx;
    char count_str[16];
    snprintf(count_str, sizeof(count_str), "%d", items[idx].count);
    int tag_w = SIDEBAR_WIDTH - 2 - (int)strlen(count_str) - 1;
    if (tag_w < 1)
      tag_w = 1;

    attron(COLOR_PAIR(CP_VIOLET_ATTR));
    if (is_sel)
      attron(A_REVERSE);
    mvprintw(row, sidebar_x + 2, "%-*.*s", tag_w, tag_w, items[idx].tag);
    if (is_sel)
      attroff(A_REVERSE);
    attroff(COLOR_PAIR(CP_VIOLET_ATTR));

    attron(COLOR_PAIR(CP_SIDEBAR));
    if (is_sel)
      attron(A_REVERSE);
    mvprintw(row, sidebar_x + SIDEBAR_WIDTH - 1 - (int)strlen(count_str), "%s",
             count_str);
    if (is_sel)
      attroff(A_REVERSE);
    attroff(COLOR_PAIR(CP_SIDEBAR));

    row++;
  }

  row++;
  if (row < h - 3) {
    attron(COLOR_PAIR(CP_SIDEBAR_HEADER) | A_BOLD);
    mvprintw(row++, sidebar_x + 1, "PROJECTS");
    attroff(COLOR_PAIR(CP_SIDEBAR_HEADER) | A_BOLD);
  }
  for (; idx < item_count && items[idx].type == 0; idx++) {
    if (row >= h - 3)
      break;
    int is_sel = sidebar_focus && sidebar_cursor == idx;
    char count_str[16];
    snprintf(count_str, sizeof(count_str), "%d", items[idx].count);
    int tag_w = SIDEBAR_WIDTH - 2 - (int)strlen(count_str) - 1;
    if (tag_w < 1)
      tag_w = 1;

    attron(COLOR_PAIR(CP_SIDEBAR_PROJECT));
    if (is_sel)
      attron(A_REVERSE);
    mvprintw(row, sidebar_x + 2, "%-*.*s", tag_w, tag_w, items[idx].tag);
    if (is_sel)
      attroff(A_REVERSE);
    attroff(COLOR_PAIR(CP_SIDEBAR_PROJECT));

    attron(COLOR_PAIR(CP_SIDEBAR));
    if (is_sel)
      attron(A_REVERSE);
    mvprintw(row, sidebar_x + SIDEBAR_WIDTH - 1 - (int)strlen(count_str), "%s",
             count_str);
    if (is_sel)
      attroff(A_REVERSE);
    attroff(COLOR_PAIR(CP_SIDEBAR));

    row++;
  }

  row++;
  if (row < h - 3) {
    attron(COLOR_PAIR(CP_SIDEBAR_HEADER) | A_BOLD);
    mvprintw(row++, sidebar_x + 1, "CONTEXTS");
    attroff(COLOR_PAIR(CP_SIDEBAR_HEADER) | A_BOLD);
  }
  for (; idx < item_count; idx++) {
    if (row >= h - 3)
      break;
    int is_sel = sidebar_focus && sidebar_cursor == idx;
    char count_str[16];
    snprintf(count_str, sizeof(count_str), "%d", items[idx].count);
    int tag_w = SIDEBAR_WIDTH - 2 - (int)strlen(count_str) - 1;
    if (tag_w < 1)
      tag_w = 1;

    attron(COLOR_PAIR(CP_SIDEBAR_CONTEXT));
    if (is_sel)
      attron(A_REVERSE);
    mvprintw(row, sidebar_x + 2, "%-*.*s", tag_w, tag_w, items[idx].tag);
    if (is_sel)
      attroff(A_REVERSE);
    attroff(COLOR_PAIR(CP_SIDEBAR_CONTEXT));

    attron(COLOR_PAIR(CP_SIDEBAR));
    if (is_sel)
      attron(A_REVERSE);
    mvprintw(row, sidebar_x + SIDEBAR_WIDTH - 1 - (int)strlen(count_str), "%s",
             count_str);
    if (is_sel)
      attroff(A_REVERSE);
    attroff(COLOR_PAIR(CP_SIDEBAR));

    row++;
  }

  for (int y = 1; y < h - 3; y++) {
    attron(COLOR_PAIR(CP_SIDEBAR));
    mvaddch(y, sidebar_x - 1, ACS_VLINE);
    attroff(COLOR_PAIR(CP_SIDEBAR));
  }
}

/* ================= help screen ================= */

static void tui_show_help(void) {
  int h, w;
  getmaxyx(stdscr, h, w);
  attron(COLOR_PAIR(CP_MAIN_BG));
  for (int y = 0; y < h; y++) {
    move(y, 0);
    for (int x = 0; x < w; x++)
      addch(' ');
  }

  int col1 = 4;
  int col2 = w / 2 + 2;
  int row = 2;

  attron(COLOR_PAIR(CP_HEADER) | A_BOLD);
  mvprintw(row, col1, "NAVIGATION");
  mvprintw(row, col2, "VIEW");
  attroff(COLOR_PAIR(CP_HEADER) | A_BOLD);
  row++;

  struct {
    const char *k;
    const char *d;
  } nav[] = {
      {"j / \\", "next task"},
      {"k / ^", "previous task"},
      {"gg", "first task"},
      {"G", "last task"},
  };
  struct {
    const char *k;
    const char *d;
  } view[] = {
      {"/", "fuzzy search"},      {"S", "cycle sort"},
      {"h", "show done in list"}, {"A", "radar stats chart"},
      {"q", "quit help"},
  };

  int max_pairs = 5;
  for (int i = 0; i < max_pairs; i++) {
    int r = row + i;
    if (i < 4) {
      attron(COLOR_PAIR(CP_HELP_KEY));
      mvprintw(r, col1, "%-14s", nav[i].k);
      attroff(COLOR_PAIR(CP_HELP_KEY));
      mvprintw(r, col1 + 14, "%s", nav[i].d);
    }
    attron(COLOR_PAIR(CP_HELP_KEY));
    mvprintw(r, col2, "%-14s", view[i].k);
    attroff(COLOR_PAIR(CP_HELP_KEY));
    mvprintw(r, col2 + 14, "%s", view[i].d);
  }

  row += max_pairs + 1;
  attron(COLOR_PAIR(CP_HEADER) | A_BOLD);
  mvprintw(row, col1, "EDITING");
  mvprintw(row, col2, "SYSTEM");
  attroff(COLOR_PAIR(CP_HEADER) | A_BOLD);
  row++;

  struct {
    const char *k;
    const char *d;
  } edit[] = {
      {"n", "new task"},
      {"e", "edit task"},
      {"x", "toggle complete"},
      {"dd", "delete task"},
      {"p", "cycle priority A->B->C->."},
      {"u", "undo"},
  };
  struct {
    const char *k;
    const char *d;
  } sys[] = {
      {"?", "help"},
      {"q", "quit"},
  };

  int edit_count = 6;
  for (int i = 0; i < edit_count; i++) {
    int r = row + i;
    attron(COLOR_PAIR(CP_HELP_KEY));
    mvprintw(r, col1, "%-14s", edit[i].k);
    attroff(COLOR_PAIR(CP_HELP_KEY));
    mvprintw(r, col1 + 14, "%s", edit[i].d);

    if (i < 2) {
      attron(COLOR_PAIR(CP_HELP_KEY));
      mvprintw(r, col2, "%-14s", sys[i].k);
      attroff(COLOR_PAIR(CP_HELP_KEY));
      mvprintw(r, col2 + 14, "%s", sys[i].d);
    }
  }

  mvprintw(h - 2, col1, "Press any key to return...");
  refresh();
  getch();
  attroff(COLOR_PAIR(CP_MAIN_BG));
}

/* ================= sfetch radar chart & stats view ('A') ================= */

#define NUM_GRAY_LEVELS 6
static const int radar_color_pairs[NUM_GRAY_LEVELS] = {
    CP_RADAR_1, CP_RADAR_2, CP_RADAR_3, CP_RADAR_4, CP_RADAR_5, CP_RADAR_6};
/* Borderlines (+) and (*) replaced with spaces to remove edges/vertices */
static const char *chart_glyph[] = {" ", "\u00b7", " ", " ", "#"};

static int point_in_poly(double px, double py, const int *vx, const int *vy,
                         int n) {
  int inside = 0;
  int j = n - 1;
  for (int i = 0; i < n; i++) {
    if (((vy[i] > py) != (vy[j] > py)) &&
        (px < (double)(vx[j] - vx[i]) * (py - vy[i]) / (double)(vy[j] - vy[i]) +
                  vx[i]))
      inside = !inside;
    j = i;
  }
  return inside;
}

static void plot(int g[GRID_H][GRID_W], int x, int y, int type) {
  if (x >= 0 && x < GRID_W && y >= 0 && y < GRID_H)
    if (g[y][x] == 0 || type == 2 || type == 3 || type == 4)
      g[y][x] = type;
}

static void draw_line(int g[GRID_H][GRID_W], int x0, int y0, int x1, int y1,
                      int type) {
  int dx = x1 - x0, dy = y1 - y0;
  int steps = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
  if (steps == 0) {
    plot(g, x0, y0, type);
    return;
  }
  for (int i = 0; i <= steps; i++) {
    double t = (double)i / steps;
    int x = x0 + (int)lround(dx * t);
    int y = y0 + (int)lround(dy * t);
    plot(g, x, y, type);
  }
}

static void tui_show_stats(void) {
  Attr attrs[NUM_ATTRS];
  memcpy(attrs, default_attrs, sizeof(default_attrs));

  int completed_total = 0;
  int completed_unmatched = 0;

  for (int i = 0; i < task_count; i++) {
    if (line_is_done(tasks[i].text)) {
      completed_total++;
      char buf[MAX_LINE];
      strncpy(buf, tasks[i].text + 2, sizeof(buf) - 1);
      buf[sizeof(buf) - 1] = '\0';
      char *saveptr;
      char *tok = strtok_r(buf, " \t", &saveptr);
      int has_any = 0;
      while (tok) {
        if (tok[0] == '!' && tok[1] != '\0' && tok[2] == '\0') {
          char lc = (char)tolower((unsigned char)tok[1]);
          for (int a = 0; a < NUM_ATTRS; a++) {
            if (attrs[a].code == lc) {
              if (attrs[a].total < MAX_POINTS)
                attrs[a].total++;
              attrs[a].marker_count++;
              has_any = 1;
            }
          }
        }
        tok = strtok_r(NULL, " \t", &saveptr);
      }
      if (!has_any)
        completed_unmatched++;
    }
  }

  FILE *f = fopen(g_done_path, "r");
  if (f) {
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f)) {
      line[strcspn(line, "\r\n")] = '\0';
      if (line[0] == 'x' && line[1] == ' ') {
        completed_total++;
        char buf[MAX_LINE];
        strncpy(buf, line + 2, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        char *saveptr;
        char *tok = strtok_r(buf, " \t", &saveptr);
        int has_any = 0;
        while (tok) {
          if (tok[0] == '!' && tok[1] != '\0' && tok[2] == '\0') {
            char lc = (char)tolower((unsigned char)tok[1]);
            for (int a = 0; a < NUM_ATTRS; a++) {
              if (attrs[a].code == lc) {
                if (attrs[a].total < MAX_POINTS)
                  attrs[a].total++;
                attrs[a].marker_count++;
                has_any = 1;
              }
            }
          }
          tok = strtok_r(NULL, " \t", &saveptr);
        }
        if (!has_any)
          completed_unmatched++;
      }
    }
    fclose(f);
  }

  int h, w;
  getmaxyx(stdscr, h, w);

  int stats_lines = 2 + NUM_ATTRS + 1;
  int margin = 3;
  int use_h = h - margin - stats_lines;
  if (use_h > GRID_H)
    use_h = GRID_H;
  if (use_h < 9)
    use_h = 9;
  if (use_h % 2 == 0)
    use_h -= 1;

  int use_w = (int)lround(use_h * ((double)GRID_W / (double)GRID_H));
  if (use_w > GRID_W)
    use_w = GRID_W;
  if (use_w > w - 6)
    use_w = w - 6;
  if (use_w < 19)
    use_w = 19;

  int grid[GRID_H][GRID_W];
  for (int y = 0; y < use_h; y++)
    for (int x = 0; x < use_w; x++)
      grid[y][x] = 0;

  int cx = use_w / 2;
  int cy = use_h / 2;
  double max_r_y = (use_h / 2) - 1;
  double max_r_x = max_r_y * 2.0;
  if (max_r_x > use_w / 2 - 1)
    max_r_x = use_w / 2 - 1;

  double angle_deg[NUM_ATTRS];
  int axis_x[NUM_ATTRS], axis_y[NUM_ATTRS];
  int data_x[NUM_ATTRS], data_y[NUM_ATTRS];

  for (int i = 0; i < NUM_ATTRS; i++) {
    angle_deg[i] = -90.0 + i * (360.0 / NUM_ATTRS);
    double rad = angle_deg[i] * M_PI / 180.0;
    axis_x[i] = cx + (int)lround(max_r_x * cos(rad));
    axis_y[i] = cy + (int)lround(max_r_y * sin(rad));

    int lvl = compute_level(attrs[i].total);
    double ratio = (double)lvl / (double)NUM_LEVELS;
    data_x[i] = cx + (int)lround(max_r_x * ratio * cos(rad));
    data_y[i] = cy + (int)lround(max_r_y * ratio * sin(rad));
  }

  for (int i = 0; i < NUM_ATTRS; i++)
    draw_line(grid, cx, cy, axis_x[i], axis_y[i], 1);

  for (int r = 1; r <= NUM_LEVELS; r++) {
    double rr = (double)r / NUM_LEVELS;
    int ring_x[NUM_ATTRS], ring_y[NUM_ATTRS];
    for (int i = 0; i < NUM_ATTRS; i++) {
      double rad = angle_deg[i] * M_PI / 180.0;
      ring_x[i] = cx + (int)lround(max_r_x * rr * cos(rad));
      ring_y[i] = cy + (int)lround(max_r_y * rr * sin(rad));
    }
    for (int i = 0; i < NUM_ATTRS; i++) {
      int j = (i + 1) % NUM_ATTRS;
      draw_line(grid, ring_x[i], ring_y[i], ring_x[j], ring_y[j], 1);
    }
  }

  for (int i = 0; i < NUM_ATTRS; i++) {
    int j = (i + 1) % NUM_ATTRS;
    draw_line(grid, data_x[i], data_y[i], data_x[j], data_y[j], 2);
  }
  for (int i = 0; i < NUM_ATTRS; i++)
    plot(grid, data_x[i], data_y[i], 3);
  plot(grid, cx, cy, 4);

  attron(COLOR_PAIR(CP_MAIN_BG));
  for (int y = 0; y < h; y++) {
    move(y, 0);
    for (int x = 0; x < w; x++)
      addch(' ');
  }

  int start_y = 1;
  for (int y = 0; y < use_h && start_y + y < h - 1; y++) {
    for (int x = 0; x < use_w; x++) {
      int is_inside =
          point_in_poly((double)x, (double)y, data_x, data_y, NUM_ATTRS);
      if (is_inside && has_colors()) {
        double ddx = (x - cx) / 2.0;
        double ddy = (y - cy);
        double dist = sqrt(ddx * ddx + ddy * ddy);
        double ratio = max_r_y > 0 ? dist / max_r_y : 0.0;
        if (ratio > 1.0)
          ratio = 1.0;
        int bucket = (int)(ratio * NUM_GRAY_LEVELS);
        if (bucket >= NUM_GRAY_LEVELS)
          bucket = NUM_GRAY_LEVELS - 1;
        int tier = (NUM_GRAY_LEVELS - 1) - bucket;

        attron(COLOR_PAIR(radar_color_pairs[tier]));
        mvaddstr(start_y + y, 2 + x, chart_glyph[grid[y][x]]);
        attroff(COLOR_PAIR(radar_color_pairs[tier]));
      } else {
        chtype ch = ' ';
        if (grid[y][x] == 1)
          ch = '.';
        else if (grid[y][x] == 4)
          ch = '#';
        mvaddch(start_y + y, 2 + x, ch);
      }
    }
  }

  int stat_x = 4;
  int stat_y = start_y + use_h + 1;
  char *user = getenv("USER");

#define STAT_LINE(...)                                                         \
  do {                                                                         \
    if (stat_y < h - 1)                                                        \
      mvprintw(stat_y, stat_x, __VA_ARGS__);                                   \
    stat_y++;                                                                  \
  } while (0)

  attron(COLOR_PAIR(CP_ATTR) | A_BOLD);
  STAT_LINE("%s@tuxedo  -  %s", user ? user : "you", g_todo_path);
  attroff(COLOR_PAIR(CP_ATTR) | A_BOLD);
  stat_y++;

  for (int i = 0; i < NUM_ATTRS; i++) {
    int lvl = compute_level(attrs[i].total);
    char bar[32] = "";
    for (int j = 0; j < NUM_LEVELS; j++)
      strcat(bar, j < lvl ? "#" : "-");
    STAT_LINE("%-12s !%c  %s  Lv.%d  (%d pts)", attrs[i].name, attrs[i].code,
              bar, lvl, attrs[i].total);
  }

  if (completed_unmatched > 0)
    STAT_LINE("Tasks: %d done, %d unlabeled", completed_total,
              completed_unmatched);
  else
    STAT_LINE("Tasks: %d done", completed_total);

#undef STAT_LINE

  mvprintw(h - 2, 4, "Press any key to return...");
  refresh();
  getch();
  attroff(COLOR_PAIR(CP_MAIN_BG));
}

/* ================= calendar & repeat helpers ================= */

static int days_in_month(int year4, int mon1to12) {
  struct tm t;
  memset(&t, 0, sizeof(t));
  t.tm_year = year4 - 1900;
  t.tm_mon = mon1to12;
  t.tm_mday = 0;
  t.tm_hour = 12;
  mktime(&t);
  return t.tm_mday;
}

static void normalize_tm_noon(struct tm *t) {
  t->tm_hour = 12;
  mktime(t);
}

/* ================= task prompt with embedded DUE & REPEAT ================= */

static void render_embedded_calendar(int top, int left, struct tm *cur) {
  int box_h = 12, box_w = 32;
  static const char *month_names[12] = {
      "January", "February", "March",     "April",   "May",      "June",
      "July",    "August",   "September", "October", "November", "December"};

  for (int y = 0; y < box_h; y++) {
    move(top + y, left);
    for (int x = 0; x < box_w; x++)
      addch(' ');
  }

  mvaddch(top, left, ACS_ULCORNER);
  mvaddch(top, left + box_w - 1, ACS_URCORNER);
  mvaddch(top + box_h - 1, left, ACS_LLCORNER);
  mvaddch(top + box_h - 1, left + box_w - 1, ACS_LRCORNER);
  for (int x = 1; x < box_w - 1; x++) {
    mvaddch(top, left + x, ACS_HLINE);
    mvaddch(top + box_h - 1, left + x, ACS_HLINE);
  }
  for (int y = 1; y < box_h - 1; y++) {
    mvaddch(top + y, left, ACS_VLINE);
    mvaddch(top + y, left + box_w - 1, ACS_VLINE);
  }

  attron(A_BOLD);
  mvprintw(top, left + 2, " DUE ");
  attroff(A_BOLD);

  mvprintw(top + 1, left + 3, "<  %s %d  >", month_names[cur->tm_mon],
           cur->tm_year + 1900);
  mvprintw(top + 3, left + 3, "S  M  T  W  T  F  S");

  struct tm first = *cur;
  first.tm_mday = 1;
  normalize_tm_noon(&first);
  int start_wday = first.tm_wday;
  int dim = days_in_month(cur->tm_year + 1900, cur->tm_mon + 1);

  int day = 1;
  for (int row = 0; row < 6 && day <= dim; row++) {
    for (int col = 0; col < 7; col++) {
      if (row == 0 && col < start_wday)
        continue;
      if (day > dim)
        break;
      int is_sel = (day == cur->tm_mday);
      if (is_sel) {
        attron(COLOR_PAIR(CP_OVERDUE) | A_REVERSE | A_BOLD);
      }
      mvprintw(top + 4 + row, left + 3 + col * 3, "%2d", day);
      if (is_sel) {
        attroff(COLOR_PAIR(CP_OVERDUE) | A_REVERSE | A_BOLD);
      }
      day++;
    }
  }

  char wkday_str[16];
  strftime(wkday_str, sizeof(wkday_str), "%a %b %d", cur);

  attron(COLOR_PAIR(CP_CONTEXT) | A_BOLD);
  mvprintw(top + 10, left + 3, "%s", wkday_str);
  attroff(COLOR_PAIR(CP_CONTEXT) | A_BOLD);
}

static void render_embedded_repeat(int top, int left, int num, int unit_idx,
                                   int repeat_field) {
  int box_h = 8, box_w = 56;
  static const char *units[5] = {"day", "business", "week", "month", "year"};
  char unit_chars[5] = {'d', 'b', 'w', 'm', 'y'};

  for (int y = 0; y < box_h; y++) {
    move(top + y, left);
    for (int x = 0; x < box_w; x++)
      addch(' ');
  }

  mvaddch(top, left, ACS_ULCORNER);
  mvaddch(top, left + box_w - 1, ACS_URCORNER);
  mvaddch(top + box_h - 1, left, ACS_LLCORNER);
  mvaddch(top + box_h - 1, left + box_w - 1, ACS_LRCORNER);
  for (int x = 1; x < box_w - 1; x++) {
    mvaddch(top, left + x, ACS_HLINE);
    mvaddch(top + box_h - 1, left + x, ACS_HLINE);
  }
  for (int y = 1; y < box_h - 1; y++) {
    mvaddch(top + y, left, ACS_VLINE);
    mvaddch(top + y, left + box_w - 1, ACS_VLINE);
  }

  attron(A_BOLD);
  mvprintw(top, left + 2, " REPEAT ");
  attroff(A_BOLD);

  attron(COLOR_PAIR(CP_CONTEXT) | A_BOLD);
  mvprintw(top + 2, left + 3, "Every");
  attroff(COLOR_PAIR(CP_CONTEXT) | A_BOLD);

  mvprintw(top + 2, left + 8, " ");
  if (repeat_field == 0)
    attron(A_REVERSE);
  printw(" %d ", num);
  if (repeat_field == 0)
    attroff(A_REVERSE);

  for (int i = 0; i < 5; i++) {
    if (repeat_field == 1 && unit_idx == i)
      attron(A_REVERSE);
    printw(" %s", units[i]);
    if (repeat_field == 1 && unit_idx == i)
      attroff(A_REVERSE);
  }

  time_t t = time(NULL);
  struct tm tmv;
  localtime_r(&t, &tmv);
  add_interval(&tmv, num, unit_chars[unit_idx]);
  char next_buf[32];
  strftime(next_buf, sizeof(next_buf), "%a %b %d", &tmv);

  attron(A_DIM);
  mvprintw(top + 4, left + 3, "next: %s", next_buf);
  attroff(A_DIM);

  char generated[32];
  snprintf(generated, sizeof(generated), "rec:%d%c", num, unit_chars[unit_idx]);

  attron(COLOR_PAIR(CP_CONTEXT) | A_BOLD);
  mvprintw(top + 6, left + 3, "-> writes %s", generated);
  attroff(COLOR_PAIR(CP_CONTEXT) | A_BOLD);
}

static int tui_task_prompt(const char *title, char *buf, size_t buf_sz) {
  char work[MAX_LINE];
  memset(work, 0, sizeof(work));
  strncpy(work, buf, sizeof(work) - 1);
  int len = (int)strlen(work);
  int cpos = len;
  int sug_sel = 0;

  time_t t_now = time(NULL);
  struct tm cal_cur;
  localtime_r(&t_now, &cal_cur);
  normalize_tm_noon(&cal_cur);

  int rep_num = 1;
  int rep_unit = 2;
  int rep_field = 0;

  while (1) {
    curs_set(1); /* Ensure cursor is visible during prompt loop */
    int h, w;
    getmaxyx(stdscr, h, w);

    int box_w = 72;
    int box_h = 7;
    int top = h / 2 - box_h / 2 - 4;
    if (top < 1)
      top = 1;
    int left = w / 2 - box_w / 2;

    for (int y = 0; y < box_h + 16; y++) {
      move(top + y, left - 2);
      for (int x = 0; x < box_w + 4; x++)
        addch(' ');
    }

    mvaddch(top, left, ACS_ULCORNER);
    mvaddch(top, left + box_w - 1, ACS_URCORNER);
    mvaddch(top + box_h - 1, left, ACS_LLCORNER);
    mvaddch(top + box_h - 1, left + box_w - 1, ACS_LRCORNER);
    for (int x = 1; x < box_w - 1; x++) {
      mvaddch(top, left + x, ACS_HLINE);
      mvaddch(top + box_h - 1, left + x, ACS_HLINE);
    }
    for (int y = 1; y < box_h - 1; y++) {
      mvaddch(top + y, left, ACS_VLINE);
      mvaddch(top + y, left + box_w - 1, ACS_VLINE);
    }

    attron(A_BOLD);
    mvprintw(top, left + 3, " %s ", title);
    attroff(A_BOLD);

    mvprintw(top + 2, left + 2, "> ");
    mvprintw(top + 2, left + 4, "%s", work);

    attron(A_DIM);
    mvprintw(top + 4, left + 2,
             "format: (A) Buy milk +shop @home due:2026-05-12 rec:2d");
    attroff(A_DIM);

    int is_due_mode = 0;
    int is_rec_mode = 0;

    if (cpos >= 4 && strncasecmp(&work[cpos - 4], "due:", 4) == 0) {
      is_due_mode = 1;
    } else if (cpos >= 4 && strncasecmp(&work[cpos - 4], "rec:", 4) == 0) {
      is_rec_mode = 1;
    }

    int start = cpos;
    while (start > 0 && work[start - 1] != ' ' && work[start - 1] != '\t')
      start--;

    char sug_prefix[64] = "";
    int has_tag_sug = 0;
    SidebarItem matches[64];
    int mcount = 0;

    if (!is_due_mode && !is_rec_mode && start < cpos &&
        (work[start] == '+' || work[start] == '@' || work[start] == '!')) {
      int plen = cpos - start;
      if (plen < (int)sizeof(sug_prefix)) {
        memcpy(sug_prefix, &work[start], (size_t)plen);
        sug_prefix[plen] = '\0';

        SidebarItem all_items[512];
        int icount = build_sidebar_items(all_items, 512);
        for (int i = 0; i < icount; i++) {
          if (strncasecmp(all_items[i].tag, sug_prefix, (size_t)plen) == 0) {
            if (mcount < 64)
              matches[mcount++] = all_items[i];
          }
        }
        if (mcount > 0)
          has_tag_sug = 1;
      }
    }

    if (is_due_mode) {
      render_embedded_calendar(top + box_h, left, &cal_cur);
    } else if (is_rec_mode) {
      render_embedded_repeat(top + box_h, left, rep_num, rep_unit, rep_field);
    } else if (has_tag_sug) {
      int sug_top = top + box_h;
      int sug_left = left + 4;
      int sug_w = 30;
      int sug_h = mcount < 5 ? mcount : 5;

      for (int sy = 0; sy < sug_h; sy++) {
        move(sug_top + sy, sug_left);
        for (int sx = 0; sx < sug_w; sx++)
          addch(' ');
      }

      for (int sy = 0; sy < sug_h; sy++) {
        int is_sel = (sy == sug_sel);
        if (is_sel)
          attron(A_REVERSE);
        attron(COLOR_PAIR(CP_ATTR));
        mvprintw(sug_top + sy, sug_left, " %-*s ", sug_w - 2, matches[sy].tag);
        attroff(COLOR_PAIR(CP_ATTR));
        if (is_sel)
          attroff(A_REVERSE);
      }
    }

    int cursor_x = left + 4 + cpos;
    if (cursor_x >= left + box_w - 1)
      cursor_x = left + box_w - 2;
    move(top + 2, cursor_x);

    int sidebar_x = w - SIDEBAR_WIDTH;
    move(h - 2, 0);
    clrtoeol();
    attron(COLOR_PAIR(CP_STATUS_BAR));
    for (int x = 0; x < sidebar_x; x++)
      addch(' ');
    mvprintw(h - 2, 0, " Enter Save  .  Esc Normal");
    attroff(COLOR_PAIR(CP_STATUS_BAR));

    mvprintw(h - 2, sidebar_x - 1, "");
    attron(COLOR_PAIR(CP_MODE) | A_BOLD);
    printw("%-*s", SIDEBAR_WIDTH + 1, " EDITING");
    attroff(COLOR_PAIR(CP_MODE) | A_BOLD);

    /* Explicitly place and refresh the physical hardware cursor position */
    move(top + 2, cursor_x);
    refresh();

    int ch = getch();

    if (is_due_mode) {
      if (ch == 'h' || ch == KEY_LEFT) {
        cal_cur.tm_mday -= 1;
        normalize_tm_noon(&cal_cur);
      } else if (ch == 'l' || ch == KEY_RIGHT) {
        cal_cur.tm_mday += 1;
        normalize_tm_noon(&cal_cur);
      } else if (ch == 'j' || ch == KEY_DOWN) {
        cal_cur.tm_mday += 7;
        normalize_tm_noon(&cal_cur);
      } else if (ch == 'k' || ch == KEY_UP) {
        cal_cur.tm_mday -= 7;
        normalize_tm_noon(&cal_cur);
      } else if (ch == 't') {
        time_t tnow = time(NULL);
        localtime_r(&tnow, &cal_cur);
        normalize_tm_noon(&cal_cur);
      } else if (ch == 'T') {
        time_t tnow = time(NULL);
        localtime_r(&tnow, &cal_cur);
        cal_cur.tm_mday += 1;
        normalize_tm_noon(&cal_cur);
      } else if (ch == 'w') {
        cal_cur.tm_mday += 7;
        normalize_tm_noon(&cal_cur);
      } else if (ch == 'm') {
        cal_cur.tm_mon += 1;
        normalize_tm_noon(&cal_cur);
      } else if (ch == '\n' || ch == KEY_ENTER) {
        char dstr[16];
        strftime(dstr, sizeof(dstr), "%Y-%m-%d", &cal_cur);
        if (len + 11 < (int)sizeof(work)) {
          strcat(work, dstr);
          len += 10;
          cpos += 10;
        }
      } else if (ch == 27 || ch == 'x') {
      } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
        if (cpos > 0) {
          memmove(&work[cpos - 1], &work[cpos], (size_t)(len - cpos + 1));
          cpos--;
          len--;
          work[len] = '\0';
        }
      }
      continue;
    }

    if (is_rec_mode) {
      char unit_chars[5] = {'d', 'b', 'w', 'm', 'y'};
      if (ch == 'h' || ch == KEY_LEFT) {
        if (rep_field == 1 && rep_unit > 0)
          rep_unit--;
        else if (rep_field == 0 && rep_num > 1)
          rep_num--;
      } else if (ch == 'l' || ch == KEY_RIGHT) {
        if (rep_field == 1 && rep_unit < 4)
          rep_unit++;
        else if (rep_field == 0)
          rep_num++;
      } else if (ch == 'j' || ch == KEY_DOWN) {
        if (rep_field < 1)
          rep_field++;
      } else if (ch == 'k' || ch == KEY_UP) {
        if (rep_field > 0)
          rep_field--;
      } else if (ch == '\n' || ch == KEY_ENTER) {
        char rstr[32];
        snprintf(rstr, sizeof(rstr), "%d%c", rep_num, unit_chars[rep_unit]);
        if (len + strlen(rstr) < sizeof(work)) {
          strcat(work, rstr);
          len += (int)strlen(rstr);
          cpos += (int)strlen(rstr);
        }
      } else if (ch == 27) {
      } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
        if (cpos > 0) {
          memmove(&work[cpos - 1], &work[cpos], (size_t)(len - cpos + 1));
          cpos--;
          len--;
          work[len] = '\0';
        }
      }
      continue;
    }

    if (ch == '\n' || ch == KEY_ENTER) {
      if (has_tag_sug && mcount > 0) {
        int plen = cpos - start;
        size_t tlen = strlen(matches[sug_sel].tag);
        if (len - plen + tlen < sizeof(work) - 1) {
          memmove(&work[start + tlen], &work[cpos], (size_t)(len - cpos + 1));
          memcpy(&work[start], matches[sug_sel].tag, tlen);
          len = len - plen + (int)tlen;
          cpos = start + (int)tlen;
          work[len] = '\0';
        }
      } else {
        strncpy(buf, work, buf_sz - 1);
        buf[buf_sz - 1] = '\0';
        curs_set(0);
        return 1;
      }
    } else if (ch == 27) {
      curs_set(0);
      return 0;
    } else if (has_tag_sug && (ch == KEY_DOWN || ch == 'j')) {
      if (sug_sel < mcount - 1)
        sug_sel++;
    } else if (has_tag_sug && (ch == KEY_UP || ch == 'k')) {
      if (sug_sel > 0)
        sug_sel--;
    } else if (ch == KEY_LEFT) {
      if (cpos > 0)
        cpos--;
    } else if (ch == KEY_RIGHT) {
      if (cpos < len)
        cpos++;
    } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
      if (cpos > 0) {
        memmove(&work[cpos - 1], &work[cpos], (size_t)(len - cpos + 1));
        cpos--;
        len--;
        work[len] = '\0';
        sug_sel = 0;
      }
    } else if (ch >= 32 && ch < 127 && len < (int)sizeof(work) - 1) {
      memmove(&work[cpos + 1], &work[cpos], (size_t)(len - cpos + 1));
      work[cpos] = (char)ch;
      cpos++;
      len++;
      work[len] = '\0';
      sug_sel = 0;
    }
  }
}

static int tui_confirm(const char *label) {
  int h, w;
  getmaxyx(stdscr, h, w);
  move(h - 1, 0);
  clrtoeol();
  mvprintw(h - 1, 0, "%s (y/n) ", label);
  refresh();
  int c = getch();
  return c == 'y' || c == 'Y';
}

/* ================= main TUI Loop ================= */

static void run_tui(void) {
  initscr();
  cbreak();
  noecho();
  keypad(stdscr, TRUE);
  curs_set(0);
  if (has_colors())
    tui_init_colors();

  int task_cursor = 0;
  int pending_g = 0, pending_d = 0;
  int sort_mode = load_sort_preference();
  int sidebar_focus = 0, sidebar_cursor = 0;

  while (1) {
    DisplayRow rows[MAX_TASKS];
    int nrows;
    build_display_rows(rows, &nrows, sort_mode);
    int n = count_selectable(rows, nrows);
    if (task_cursor >= n)
      task_cursor = n > 0 ? n - 1 : 0;
    if (task_cursor < 0)
      task_cursor = 0;

    SidebarItem items[1024];
    int item_count = build_sidebar_items(items, 1024);
    if (sidebar_cursor >= item_count)
      sidebar_cursor = item_count > 0 ? item_count - 1 : 0;

    int h, w;
    getmaxyx(stdscr, h, w);

    bkgd(COLOR_PAIR(CP_MAIN_BG));
    erase();

    int sidebar_x = w - SIDEBAR_WIDTH;
    int task_w = sidebar_x - 1;

    /* Top Title Bar (Blue background matching sidebar) */
    char top_buf[MAX_LINE];
    snprintf(top_buf, sizeof(top_buf), " %d Task%s  Sort: %s%s%s%s", task_count,
             task_count == 1 ? "" : "s", sort_mode_name[sort_mode],
             tui_show_done ? "  +Done" : "", tui_filter[0] ? "  Filter: " : "",
             tui_filter);
    attron(COLOR_PAIR(CP_HEADER) | A_BOLD);
    mvprintw(0, 0, "%-*s", task_w, top_buf);
    attroff(COLOR_PAIR(CP_HEADER) | A_BOLD);

    int list_h = h - 3;
    int cur_display_row = display_row_for_task_cursor(rows, nrows, task_cursor);
    int top = 0;
    if (cur_display_row >= list_h)
      top = cur_display_row - list_h + 1;

    int selectable_seen = 0;
    for (int row = 0; row < list_h && top + row < nrows; row++) {
      DisplayRow *dr = &rows[top + row];
      int y = row + 1;
      if (dr->is_header) {
        int hc = dr->header_color ? dr->header_color : CP_HEADER;
        attron(COLOR_PAIR(hc) | A_BOLD);
        mvprintw(y, 0, "%s", dr->header);
        attroff(COLOR_PAIR(hc) | A_BOLD);

        int len_h = (int)strlen(dr->header);
        attron(COLOR_PAIR(CP_DARK_GRAY_LINE));
        mvaddch(y, len_h, ' ');
        for (int lx = len_h + 1; lx < task_w; lx++)
          mvaddch(y, lx, ACS_HLINE);
        attroff(COLOR_PAIR(CP_DARK_GRAY_LINE));
      } else {
        int is_cursor = !sidebar_focus && (top + row == cur_display_row);
        int line_num_display;
        if (is_cursor) {
          line_num_display = selectable_seen + 1;
        } else {
          line_num_display = abs(selectable_seen - task_cursor);
          if (line_num_display == 0)
            line_num_display = n;
        }
        render_task_row(y, 0, task_w, dr->task_idx, is_cursor,
                        line_num_display);
        selectable_seen++;
      }
    }

    render_sidebar(h, items, item_count, sidebar_focus, sidebar_cursor,
                   sidebar_x);

    /* Bottom Bar Row 1 (h - 2): Controls/Status */
    move(h - 2, 0);
    attron(COLOR_PAIR(CP_STATUS_BAR));
    for (int x = 0; x < sidebar_x; x++)
      addch(' ');
    mvprintw(h - 2, 0, " Enter Save  .  Esc Normal");
    attroff(COLOR_PAIR(CP_STATUS_BAR));

    mvprintw(h - 2, sidebar_x - 1, "");
    attron(COLOR_PAIR(CP_MODE) | A_BOLD);
    printw("%-*s", SIDEBAR_WIDTH + 1, " CHECKING");
    attroff(COLOR_PAIR(CP_MODE) | A_BOLD);

    /* Bottom-most Line (h - 1): Task label + color-coded full details */
    move(h - 1, 0);
    attron(COLOR_PAIR(CP_STATUS_BAR));
    for (int x = 0; x < w; x++)
      addch(' ');
    attroff(COLOR_PAIR(CP_STATUS_BAR));

    mvprintw(h - 1, 1, "Task: ");
    if (n > 0) {
      int dr = display_row_for_task_cursor(rows, nrows, task_cursor);
      int idx = rows[dr].task_idx;
      int done = line_is_done(tasks[idx].text);
      int overdue = is_overdue(idx);
      render_colored_text_line(h - 1, 7, w - 8, tasks[idx].text, 0, overdue,
                               done);
    } else {
      mvprintw(h - 1, 7, "No tasks");
    }

    refresh();

    int ch = getch();

    if (sidebar_focus) {
      switch (ch) {
      case 'j':
      case KEY_DOWN:
        if (sidebar_cursor < item_count - 1)
          sidebar_cursor++;
        break;
      case 'k':
      case KEY_UP:
        if (sidebar_cursor > 0)
          sidebar_cursor--;
        break;
      case '\n':
      case KEY_ENTER:
        if (item_count > 0) {
          strncpy(tui_filter, items[sidebar_cursor].tag,
                  sizeof(tui_filter) - 1);
          task_cursor = 0;
        }
        sidebar_focus = 0;
        break;
      case '\t':
      case 27:
        sidebar_focus = 0;
        break;
      case 'q':
        save_sort_preference(sort_mode);
        endwin();
        return;
      default:
        break;
      }
      continue;
    }

    if (pending_g) {
      pending_g = 0;
      if (ch == 'g')
        task_cursor = 0;
      continue;
    }
    if (pending_d) {
      pending_d = 0;
      if (ch == 'd' && n > 0) {
        int dr = display_row_for_task_cursor(rows, nrows, task_cursor);
        int idx = rows[dr].task_idx;
        if (tui_confirm("Delete this task?")) {
          push_undo();
          free(tasks[idx].text);
          for (int i = idx; i < task_count - 1; i++)
            tasks[i] = tasks[i + 1];
          task_count--;
          save_tasks(g_todo_path);
        }
      }
      continue;
    }

    switch (ch) {
    case 'j':
    case KEY_DOWN:
      if (task_cursor < n - 1)
        task_cursor++;
      break;
    case 'k':
    case KEY_UP:
      if (task_cursor > 0)
        task_cursor--;
      break;
    case 'g':
      pending_g = 1;
      break;
    case 'G':
      if (n > 0)
        task_cursor = n - 1;
      break;
    case 'd':
      pending_d = 1;
      break;
    case 'h':
      tui_show_done = !tui_show_done;
      task_cursor = 0;
      break;
    case 'A':
      tui_show_stats();
      break;
    case '\t':
      if (item_count > 0)
        sidebar_focus = 1;
      break;
    case 'S':
      sort_mode = (sort_mode + 1) % SORT_MODE_COUNT;
      save_sort_preference(sort_mode);
      break;
    case 'u':
      if (pop_undo())
        save_tasks(g_todo_path);
      break;
    case '?':
      tui_show_help();
      break;
    case 'x':
      if (n > 0) {
        int dr = display_row_for_task_cursor(rows, nrows, task_cursor);
        int idx = rows[dr].task_idx;
        push_undo();
        if (line_is_done(tasks[idx].text))
          mark_undone(idx);
        else
          mark_done(idx);
        save_tasks(g_todo_path);
      }
      break;
    case 'e':
      if (n > 0) {
        int dr = display_row_for_task_cursor(rows, nrows, task_cursor);
        int idx = rows[dr].task_idx;
        char buf[MAX_LINE];
        strncpy(buf, tasks[idx].text, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        if (tui_task_prompt("EDIT TASK", buf, sizeof(buf)) && buf[0]) {
          push_undo();
          free(tasks[idx].text);
          tasks[idx].text = strdup(buf);
          save_tasks(g_todo_path);
        }
      }
      break;
    case 'p':
      if (n > 0) {
        int dr = display_row_for_task_cursor(rows, nrows, task_cursor);
        int idx = rows[dr].task_idx;
        char curp = line_priority(tasks[idx].text);
        char next = curp == 0 ? 'A' : (curp >= 'D' ? 0 : (char)(curp + 1));
        push_undo();
        char *updated = next ? set_priority(tasks[idx].text, next)
                             : strip_priority(tasks[idx].text);
        free(tasks[idx].text);
        tasks[idx].text = updated;
        save_tasks(g_todo_path);
      }
      break;
    case 'n': {
      char buf[MAX_LINE] = "";
      if (tui_task_prompt("ADD TASK", buf, sizeof(buf)) && buf[0] &&
          task_count < MAX_TASKS) {
        char today[16];
        today_str(today, sizeof(today));
        char final_line[MAX_LINE];
        char first_tok[16] = {0};
        sscanf(buf, "%15s", first_tok);
        if (looks_like_date(first_tok))
          snprintf(final_line, sizeof(final_line), "%s", buf);
        else
          snprintf(final_line, sizeof(final_line), "%s %s", today, buf);
        push_undo();
        tasks[task_count].text = strdup(final_line);
        task_count++;
        save_tasks(g_todo_path);
        task_cursor = n;
      }
      break;
    }
    case 'q':
      save_sort_preference(sort_mode);
      endwin();
      return;
    default:
      break;
    }
  }
}

/* ================= main / dispatch ================= */

int main(int argc, char **argv) {
  char *rest[64];
  int rest_n = 0;
  for (int i = 1; i < argc && rest_n < 64; i++) {
    if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--force") == 0)
      g_force = 1;
    else if (strcmp(argv[i], "--json") == 0)
      g_json = 1;
    else
      rest[rest_n++] = argv[i];
  }

  resolve_todo_path(g_todo_path, sizeof(g_todo_path));
  resolve_done_path(g_todo_path, g_done_path, sizeof(g_done_path));
  load_tasks(g_todo_path);

  (void)g_force;
  (void)g_json;

  if (rest_n == 0) {
    run_tui();
    return 0;
  }

  const char *cmd = rest[0];
  char **cargv = rest + 1;
  int cargc = rest_n - 1;

  if (!strcmp(cmd, "add") || !strcmp(cmd, "a"))
    cmd_add(cargc, cargv);
  else if (!strcmp(cmd, "do") || !strcmp(cmd, "done"))
    cmd_do(cargc, cargv);
  else if (!strcmp(cmd, "archive"))
    cmd_archive();
  else {
    run_tui();
  }

  return 0;
}
