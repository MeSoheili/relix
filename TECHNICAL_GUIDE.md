# ReLix — Technical Guide

> Deep-dive reference for contributors, packagers, and developers who want to understand, extend, or port ReLix.

---

## Table of Contents

1. [Architecture Overview](#1-architecture-overview)
2. [Source Code Structure — 21 Sections](#2-source-code-structure--21-sections)
3. [Data Model](#3-data-model)
4. [APT File Format Parsing](#4-apt-file-format-parsing)
5. [File Write Safety Pipeline](#5-file-write-safety-pipeline)
6. [Toggle Logic in Detail](#6-toggle-logic-in-detail)
7. [Async Metadata & Reachability](#7-async-metadata--reachability)
8. [Rendering Pipeline — Flicker-Free TUI](#8-rendering-pipeline--flicker-free-tui)
9. [Two-Pane Layout System](#9-two-pane-layout-system)
10. [Color Theme System](#10-color-theme-system)
11. [Event Loop & Input Handling](#11-event-loop--input-handling)
12. [Config Persistence](#12-config-persistence)
13. [Undo Stack](#13-undo-stack)
14. [Build System Deep Dive](#14-build-system-deep-dive)
15. [Known Limitations & Future Work](#15-known-limitations--future-work)

---

## 1. Architecture Overview

ReLix is a **single-file, single-binary** C++17 TUI application. There are no dynamic plugins, no shared libraries beyond ncurses and pthreads, and no configuration format more complex than a flat key=value file.

```
┌─────────────────────────────────────────────────────────┐
│                     main event loop                     │
│  getch() [100ms timeout] → dispatch → mutate state      │
└──────────────┬──────────────────────────┬───────────────┘
               │                          │
    ┌──────────▼───────────┐   ┌──────────▼───────────────┐
    │    Repo Data Layer   │   │     Render Layer          │
    │  g_repos (master)    │   │  erase()                  │
    │  g_filtered (view)   │   │  draw* functions          │
    │  loadRepos()         │   │  wnoutrefresh(stdscr)     │
    │  rebuildFiltered()   │   │  doupdate()               │
    └──────────┬───────────┘   └───────────────────────────┘
               │
    ┌──────────▼───────────┐
    │   File I/O Layer     │
    │  readAllLines()      │
    │  atomicWriteLines()  │
    │  backupFile()        │
    │  pushUndo()          │
    └──────────┬───────────┘
               │
    ┌──────────▼───────────┐
    │  Background Thread   │
    │  checkReachable()    │  ← detached std::thread
    │  metaFromCache()     │  ← reads /var/lib/apt/lists/
    │  AsyncMeta struct    │  ← atomic<bool> flags + mutex
    └──────────────────────┘
```

The entire application state lives in a small set of `static` global variables. This is intentional for a single-file TUI — it avoids the complexity of passing context through every draw call while remaining trivially auditable.

---

## 2. Source Code Structure — 21 Sections

The file is divided into 21 clearly labelled sections separated by banner comments:

| Section | Lines (approx.) | Responsibility |
|---|---|---|
| 1 — String Utilities | ~25 | `trimStr`, `splitWords`, `toLower`, `containsCI` |
| 2 — Config | ~50 | `Config` struct, `loadConfig`, `saveConfig`, `configPath` |
| 3 — Color Themes | ~110 | `Theme` struct, 4 theme tables, `applyTheme`, `ColorPair` enum |
| 4 — OS Detection | ~25 | `detectOS` — reads `/etc/os-release` |
| 5 — Repo Struct + Globals | ~35 | `RepoEntry`, `UndoEntry`, all global state |
| 6 — Parse Files | ~100 | `parseListFile`, `parseSourcesFile` with block processor lambda |
| 7 — Load + Filter + Sort | ~55 | `loadRepos`, `rebuildFiltered` with 3-mode sort comparator |
| 8 — Backup | ~30 | `backupFile` with timestamp, safe dir creation |
| 9 — Atomic Write + Undo | ~50 | `readAllLines`, `atomicWriteLines`, `pushUndo`, `applyUndo` |
| 10 — Toggle Logic | ~60 | `toggleList`, `toggleDeb822` with block-range detection |
| 11 — Delete Logic | ~50 | `deleteRepoClean` for both formats |
| 12 — Export / Import | ~55 | `exportRepos`, `importRepos` with dedup |
| 13 — Async Metadata | ~130 | `RepoMeta`, `metaFromCache`, `checkReachable`, `AsyncMeta`, `fetchMetaAsync` |
| 14 — UI State | ~35 | Selection, scroll, status, search, meta display flags |
| 15 — Layout Constants | ~10 | `listPaneW`, `detailPaneX`, `detailPaneW`, `listHeight` |
| 16 — Drawing | ~230 | All `draw*` functions + `redraw` |
| 17 — Popup Dialogs | ~100 | `popupCleanup`, `confirmDialog`, `inputDialog`, `pagerDialog` |
| 18 — apt update | ~30 | `runAptUpdate` — suspend ncurses, capture output, show in pager |
| 19 — Mouse Support | ~35 | `handleMouse` — click/double-click/scroll |
| 20 — Search Mode | ~20 | `handleSearchInput` — keystroke handler for `/` filter |
| 21 — Main | ~150 | ncurses init, event loop, all key bindings |

---

## 3. Data Model

### `RepoEntry`

The central data structure. One instance per repository entry found in any APT source file.

```cpp
struct RepoEntry {
    std::string file;       // Absolute path to source file
    std::string display;    // Raw line for .list; formatted "deb URI suite comps" for .sources
    bool        enabled;    // false if commented out or Enabled: no
    bool        isDeb822;   // true for .sources format
    int         blockIndex; // deb822: which stanza (0-based); .list: always -1
    std::string uri;        // e.g. "http://archive.ubuntu.com/ubuntu"
    std::string suite;      // e.g. "noble", "noble-updates"
    std::string components; // e.g. "main restricted universe"
    std::string types;      // "deb", "deb-src", or both
};
```

**Important:** for `.list` files, `display` stores the **raw unmodified line** from disk (including any leading `# ` prefix). This is critical for the toggle logic — it's used as the exact match key when rewriting the file.

### Global State

```cpp
static std::vector<RepoEntry> g_repos;    // Master list, loaded order
static std::vector<int>       g_filtered; // Indices into g_repos after filter+sort
static std::string            g_filterStr;// Current live-search string
static OSInfo                 g_os;       // {id, version} from /etc/os-release
static bool                   g_isRoot;   // geteuid() == 0
static bool                   g_readOnly; // !g_isRoot
```

`g_filtered` is a view — a sorted/filtered list of integer indices into `g_repos`. All UI navigation operates on `g_filtered`. `g_repos` is never reordered.

---

## 4. APT File Format Parsing

### One-Line Format (`.list`)

```
deb http://archive.ubuntu.com/ubuntu noble main restricted
# deb http://archive.ubuntu.com/ubuntu noble-src main
```

Parsing rules in `parseListFile()`:

- Lines starting with `deb` (enabled) or `# deb` / `#deb` (disabled) are accepted
- The entire raw line is stored in `display` unchanged
- Fields are parsed for the detail pane using `splitWords()` on the uncommented form
- `enabled` is set by checking whether `trimmed[0] == '#'`

### deb822 Format (`.sources`)

```ini
Types: deb
URIs: http://archive.ubuntu.com/ubuntu
Suites: noble noble-updates noble-backports
Components: main restricted universe multiverse
Enabled: yes
```

Parsing in `parseSourcesFile()`:

1. File is read line by line; blocks are separated by blank lines
2. Each block is collected into a `std::vector<std::string>` and passed to `processBlock` lambda
3. The lambda extracts `Types`, `URIs`, `Suites`, `Components`, `Enabled` fields
4. Multi-value fields (`URIs`, `Suites`, `Components`) are split with `splitWords()`
5. A `RepoEntry` is created for each URI × Suite combination (Cartesian product)
6. `blockIndex` is a monotonic counter incremented after each complete block — this is the key used by `toggleDeb822` to locate the correct stanza for editing

**Key detail:** if `Enabled:` is absent, the block defaults to enabled (matching apt's own behaviour).

### OS-Driven Format Selection

```cpp
bool useDeb822 = ((g_os.id == "ubuntu" && g_os.version >= 22.04) ||
                  (g_os.id == "debian"  && g_os.version >= 12.0));
```

On qualifying systems, both `.list` and `.sources` files are parsed. On older systems, only `.list` files are processed.

---

## 5. File Write Safety Pipeline

Every file mutation follows this exact sequence with no exceptions:

```
readAllLines(path)
    └── returns std::vector<std::string> (one entry per line, no newlines)

pushUndo(path)
    └── saves current lines to g_undoStack (capped at 20 entries)

backupFile(path)
    └── creates /var/backups/ReLix/<mangled_path>.<timestamp>.bak
    └── non-fatal if it fails (continues with warning in status bar)

[modify the lines vector in memory]

atomicWriteLines(path, lines)
    ├── writes to path + ".ReLix.tmp"
    ├── flushes + checks stream state
    └── rename(tmp, path)   ← POSIX atomic on same filesystem
        └── on failure: removes tmp, returns false with errno message
```

The `rename()` system call is atomic on Linux when source and destination are on the same filesystem (which they always are here, since tmp is written to `path + ".ReLix.tmp"`). The original file is never truncated until the new content is fully written and flushed.

---

## 6. Toggle Logic in Detail

### `.list` Toggle (`toggleList`)

```
Before (enabled):   "deb http://example.com/repo focal main"
After  (disabled):  "# deb http://example.com/repo focal main"

Before (disabled):  "# deb http://example.com/repo focal main"
After  (enabled):   "deb http://example.com/repo focal main"
```

The function scans all lines looking for `line == repo.display` (exact raw string match). When found:
- Enabling: strips `"# "` prefix (handles both `"# deb"` and `"#deb"`)
- Disabling: prepends `"# "`

### deb822 Toggle (`toggleDeb822`)

This is more complex because the `Enabled:` field may be absent, and block boundaries must be precisely tracked.

**Step 1 — Block boundary detection:**
```cpp
struct Range { int s, e, enabledLine; };
// Scan all lines, track when blank lines create block boundaries
// Record index of "Enabled:" line within each block (-1 if absent)
```

**Step 2 — Locate the target block:**
```cpp
Block& b = blocks[repo.blockIndex];
```

**Step 3 — Patch or insert:**
```cpp
if (b.enabledLine >= 0) {
    // Replace existing: "Enabled: yes" → "Enabled: no"
    allLines[b.enabledLine] = newVal;
} else {
    // Insert after the first line of the block (typically Types:)
    allLines.insert(allLines.begin() + b.s + 1, newVal);
}
```

This correctly handles the common case where system-generated `.sources` files omit `Enabled:` entirely (implicit yes).

---

## 7. Async Metadata & Reachability

Fetching metadata is I/O-bound and must never block the UI. ReLix uses a detached `std::thread` with three synchronisation primitives:

```cpp
struct AsyncMeta {
    std::mutex         mtx;          // protects `meta` struct
    RepoMeta           meta;         // result storage
    std::atomic<bool>  ready{false}; // result available flag
    std::atomic<bool>  running{false};// thread in flight flag
    std::string        lastUri;      // identifies which repo was fetched
};
```

### DNS Timeout (the hard part)

`getaddrinfo()` has no built-in timeout. A blocking DNS lookup on an unreachable server can hang for 30+ seconds. ReLix solves this by running the DNS call in a separate thread and waiting with a deadline:

```cpp
std::thread([&]{
    gai_ret = getaddrinfo(host.c_str(), portStr.c_str(), &hints, &gai_res);
    std::lock_guard<std::mutex> lk(mtx);
    done = true;
    cv.notify_one();
}).detach();

std::unique_lock<std::mutex> lk(mtx);
cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&]{ return done; });
if (!done) return false; // DNS timed out
```

### TCP Reachability

After DNS resolves, a non-blocking `connect()` is issued and `select()` waits for the socket to become writable with the remaining timeout:

```cpp
int sock = socket(gai_res->ai_family, SOCK_STREAM | SOCK_NONBLOCK, 0);
::connect(sock, gai_res->ai_addr, gai_res->ai_addrlen); // EINPROGRESS
fd_set wfds; FD_ZERO(&wfds); FD_SET(sock, &wfds);
struct timeval tv{ timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
int sel = select(sock + 1, nullptr, &wfds, nullptr, &tv);
::close(sock);
return sel == 1;
```

### Local Cache Parsing

`metaFromCache()` derives the apt cache filename from the repo URI:

```
http://archive.ubuntu.com/ubuntu
  → strip scheme → archive.ubuntu.com/ubuntu
  → replace / with _ → archive.ubuntu.com_ubuntu
  → /var/lib/apt/lists/archive.ubuntu.com_ubuntu_dists_noble_Release
```

The `Release` file is then parsed line-by-line for `Origin:`, `Codename:`, `Suite:`, `Version:`, `Date:`, `Description:`. File mtime gives the "last updated" timestamp.

### UI Integration

The event loop uses `timeout(100)` so `getch()` returns `ERR` every 100 ms when no key is pressed. `drawDetailPane()` checks `g_asyncMeta.ready` on every frame and atomically copies the result when it arrives — no extra redraw call needed.

---

## 8. Rendering Pipeline — Flicker-Free TUI

The original code used `clear()` + `refresh()`, causing a visible blank frame on every redraw. The fix uses ncurses' double-buffering API correctly:

### The Wrong Way (causes flicker)
```
clear()        → sends ESC[2J to terminal immediately → screen goes blank
draw*()        → writes to ncurses shadow buffer
refresh()      → sends new content to terminal
               [gap between clear and refresh = visible flash]
```

### The Right Way (zero flicker)
```
erase()              → marks shadow buffer as blank (zero terminal I/O)
draw*()              → writes new content to shadow buffer (zero terminal I/O)
wnoutrefresh(stdscr) → copies shadow buffer to ncurses virtual screen
doupdate()           → diffs virtual screen vs terminal state, sends only CHANGES
                     [one atomic write, no blank frame ever visible]
```

`doupdate()` is the key function — it performs a diff of what the terminal currently shows against what we want to show, and writes only the changed characters. For a mostly-static UI where only the selected line changes, this is extremely efficient.

### Popup Windows

Popups (`confirmDialog`, `inputDialog`, `pagerDialog`) use their own `WINDOW*` with the same pattern:

```cpp
wnoutrefresh(win);  // mark popup's area in virtual screen
doupdate();          // flush to terminal
```

On close, `popupCleanup()` calls `wnoutrefresh(win)` after `werase(win)` but does **not** call `doupdate()` — the next `redraw()` in the main loop handles the full repaint cleanly.

---

## 9. Two-Pane Layout System

Layout is computed at runtime from `LINES` and `COLS` (set by ncurses on terminal resize):

```
Row 0         : Header bar (full width, COLOR_PAIR(CP_HEADER), A_BOLD)
Row 1         : Horizontal separator (ACS_HLINE)
Rows 2..H-5  : Content area — list pane left, detail pane right
Row H-4       : Horizontal separator (ACS_HLINE)
Rows H-3..H-3: [reserved — layout simplified in current version]
Row H-2       : Status bar
Row H-1       : Footer / key hint bar
```

Column split:
```cpp
static int listPaneW()   { return std::max(20, COLS * 60 / 100); }
static int detailPaneX() { return listPaneW() + 1; }
static int detailPaneW() { return std::max(0, COLS - detailPaneX()); }
```

The vertical separator uses ncurses line-drawing characters:
- `ACS_VLINE` for the column separator
- `ACS_TTEE` at the top junction (where top hline meets vline)
- `ACS_BTEE` at the bottom junction

The scrollbar is drawn in the rightmost column of the list pane using `ACS_BLOCK` for the thumb and `ACS_VLINE` for the track.

---

## 10. Color Theme System

Themes are defined as compile-time constant tables. Each theme specifies `{fg, bg}` pairs for all 15 named color pairs:

```cpp
enum ColorPair {
    CP_HEADER    = 1,   CP_FOOTER    = 2,   CP_STATUS_OK = 3,
    CP_STATUS_ERR= 4,   CP_ENABLED   = 5,   CP_DISABLED  = 6,
    CP_DETAIL    = 7,   CP_DETAIL_VAL= 8,   CP_SEP       = 9,
    CP_SEARCH    = 10,  CP_READONLY  = 11,  CP_PAGER_HIT = 12,
    CP_PAGER_GET = 13,  CP_PAGER_ERR = 14,  CP_BORDER    = 15,
};
```

`applyTheme(idx)` simply calls `init_pair()` for all 15 pairs from the selected theme table. Since ncurses re-reads color pair definitions on every `refresh()`, theme switching takes effect on the very next frame with no redraw needed.

**Adding a new theme:** add a new `Theme` entry to the `k_themes[]` array. `k_themeCount` is computed automatically at compile time with `sizeof`. No other changes needed.

---

## 11. Event Loop & Input Handling

```cpp
timeout(100);  // getch() returns ERR after 100ms with no input

while (true) {
    redraw();          // always redraw before blocking
    int ch = getch();  // blocks up to 100ms
    if (ch == ERR) continue;  // timeout — loop, redraw (picks up async meta)

    if (g_searchMode) { handleSearchInput(ch); continue; }
    if (ch == KEY_MOUSE) { handleMouse(); continue; }

    switch (ch) { /* all key bindings */ }
}
```

The 100 ms timeout serves two purposes:
1. Allows the async metadata thread to deliver results without requiring a keypress
2. Keeps CPU usage near zero (no busy-wait)

### Mouse Handling

```cpp
mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, nullptr);
mouseinterval(150);  // milliseconds between clicks for double-click detection
```

Mouse events are dispatched in `handleMouse()`:
- `BUTTON1_CLICKED` in list area → update `g_selected`
- `BUTTON1_DOUBLE_CLICKED` in list area → toggle selected repo
- `BUTTON4_PRESSED` → scroll up (wheel)
- `BUTTON5_PRESSED` → scroll down (wheel)

Click coordinates are translated to list indices: `clicked = ev.y - listTop + g_scrollOff`.

### Search Mode

Search is a modal sub-state activated by `/`. While `g_searchMode` is true, all non-special keys are fed to `handleSearchInput()` which builds `g_filterStr` character by character and calls `rebuildFiltered()` after each change. `Esc` clears and exits; `Enter` exits while keeping the filter.

---

## 12. Config Persistence

Config is stored at `~/.config/ReLix/config` (or `/tmp/ReLix.config` if `$HOME` is unset):

```ini
theme=0
sort=0
backup_dir=/var/backups/ReLix
confirmToggle=0
```

`loadConfig()` parses with a simple `find('=')` split — no dependencies on any ini library. Unknown keys are silently ignored. Values are range-clamped after parsing to prevent corruption from manual edits.

`saveConfig()` is called on theme change, sort change, and application exit. It uses `fs::create_directories()` to ensure the config directory exists.

---

## 13. Undo Stack

```cpp
struct UndoEntry {
    std::string              file;   // which file this state belongs to
    std::vector<std::string> lines;  // complete file content at time of capture
};
static std::vector<UndoEntry> g_undoStack;
static constexpr size_t k_maxUndo = 20;
```

`pushUndo(path)` is called before **every** destructive operation (toggle, delete, add). It reads the entire file into memory and appends to the stack, evicting the oldest entry when the cap is reached.

`applyUndo()` pops the most recent entry and calls `atomicWriteLines()` to restore it. After undo, `loadRepos()` is called to refresh the UI from disk.

**Note:** the undo stack is per-session and in-memory only. It does not persist across restarts. For persistent recovery, use the automatic backups in `backup_dir`.

---

## 14. Build System Deep Dive

### Dependency Resolution

```cmake
# ncurses — prefers wide-char variant
set(CURSES_NEED_NCURSES TRUE)
find_package(Curses REQUIRED)
find_library(NCURSESW_LIB NAMES ncursesw)
# Falls back to CURSES_LIBRARIES if ncursesw not found
```

### Warning Target

```cmake
add_library(relix_warnings INTERFACE)
target_compile_options(relix_warnings INTERFACE
    -Wall -Wextra -Wshadow -Wpedantic -Wconversion
    -Wnull-dereference -Wdouble-promotion -Wformat=2
)
```

Using an `INTERFACE` library keeps warning flags cleanly separated from link flags and allows reuse if the project is ever split into multiple targets.

### Release Hardening

Applied automatically when `CMAKE_BUILD_TYPE=Release`:

| Flag | Purpose |
|---|---|
| `-D_FORTIFY_SOURCE=2` | Enables runtime buffer-overflow checks in glibc |
| `-fstack-protector-strong` | Stack canaries on functions with buffers |
| `-fPIE` / `-pie` | Position-independent executable for ASLR |
| `-Wl,-z,relro` | Read-only relocations after startup |
| `-Wl,-z,now` | Resolve all symbols at startup (full RELRO) |

These are standard hardening flags for any Linux binary that may run as root.

### Debug Sanitisers

Applied automatically when `CMAKE_BUILD_TYPE=Debug`:

```cmake
-fsanitize=address,undefined
-fno-omit-frame-pointer
```

ASan detects heap/stack/global buffer overflows and use-after-free. UBSan detects signed integer overflow, null pointer dereference, misaligned access, and other undefined behaviour.

### GCC < 9 Compatibility

```cmake
if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS "9.0")
    set(FILESYSTEM_LIB "stdc++fs")
endif()
```

`std::filesystem` was experimental in GCC 8 and required explicit linkage of `libstdc++fs`. GCC 9+ includes it automatically.

---

## 15. Known Limitations & Future Work

### Current Limitations

| Area | Limitation |
|---|---|
| **deb-src** | Parsed but not separately controllable from `deb` in the same block |
| **Multiple URIs/Suites** | deb822 blocks with multiple URIs and Suites expand to separate entries; toggling one entry only affects the whole block's `Enabled:` field |
| **Import target** | Import always appends to `/etc/apt/sources.list`; cannot target `.list.d/` files |
| **Terminal resize** | No `SIGWINCH` handler — resize requires restart |
| **Pinning / Preferences** | `/etc/apt/preferences.d/` not managed |

### Possible Extensions

- **`SIGWINCH` handler** — call `endwin()` + `refresh()` on terminal resize
- **PPAmanager integration** — parse `add-apt-repository` style PPAs
- **GPG key management** — show and manage `/etc/apt/trusted.gpg.d/` alongside sources
- **Signed-By field** — display and validate `Signed-By:` in deb822 entries
- **Diff view before write** — show a unified diff popup before committing changes
- **Multiple selection** — batch toggle/delete with `Space` to mark entries
- **apt-cache policy view** — show pinning/priority for selected repo inline

---

*ReLix Technical Guide — kept in sync with `main.cpp` section headers.*