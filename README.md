<div align="center">

<img src="https://img.shields.io/badge/ReLix-TUI%20APT%20Manager-blue?style=for-the-badge&logo=linux&logoColor=white" alt="ReLix"/>

# ⚡ ReLix

### A production-grade, flicker-free TUI for managing APT repositories on Debian & Ubuntu

[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue?style=flat-square&logo=cplusplus)](https://en.cppreference.com/w/cpp/17)
[![CMake](https://img.shields.io/badge/CMake-3.16%2B-blue?style=flat-square&logo=cmake)](https://cmake.org/)
[![License: MIT](https://img.shields.io/badge/License-MIT-green?style=flat-square)](LICENSE)
[![Platform: Linux](https://img.shields.io/badge/Platform-Linux-orange?style=flat-square&logo=linux)](https://www.kernel.org/)
[![ncurses](https://img.shields.io/badge/TUI-ncurses%2Fncursesw-lightgrey?style=flat-square)]()
[![PRs Welcome](https://img.shields.io/badge/PRs-welcome-brightgreen?style=flat-square)](CONTRIBUTING.md)

</div>

---

**ReLix** is a fast, keyboard-driven terminal UI for managing APT software repositories on Debian and Ubuntu systems. It replaces hand-editing `/etc/apt/sources.list` and scattered `.list` / `.sources` files with a safe, structured interface — complete with atomic writes, automatic backups, undo, live search, async metadata fetch, and four color themes.

> No GUI required. No Python runtime. A single self-contained C++17 binary.

---

## 📸 Preview

```
╔══════════════════════════════════════════════════════════════════════════════════════╗
║  ReLix  |  OS: ubuntu 24.04  |  Theme: Dark  |  Sort: File          [ROOT]     ║
╠══════════════════════════════════════════════════╦═════════════════════════════════╣
║ ● deb http://archive.ubuntu.com/ubuntu noble ... ║ Status:      ENABLED            ║
║ ● deb http://archive.ubuntu.com/ubuntu noble ... ║ Format:      deb822 (.sources)  ║
║ ○ # deb http://dl.google.com/linux/chrome/deb  ║ Type:        deb                ║
║ ● deb https://download.docker.com/linux/ubuntu  ║ URI:         https://download.. ║
║ ● deb https://packages.microsoft.com/repos/code ║ Suite:       noble              ║
║                                                  ║ Comps:       stable             ║
║                                                  ║ File:        /etc/apt/sources.. ║
║                                                  ╠═════════════════════════════════╣
║                                                  ║ Origin:      Docker             ║
║                                                  ║ Codename:    noble              ║
║                                                  ║ Updated:     2025-06-01 14:32  ║
║                                                  ║ Reachable:   Yes                ║
╠══════════════════════════════════════════════════╩═════════════════════════════════╣
║ [5/5]  Repository toggled.                                                         ║
║ F2:Toggle F3:Add F4:Del F5:Update F6:Reload F7:Backup F8:Export m:Meta t:Theme q  ║
╚══════════════════════════════════════════════════════════════════════════════════════╝
```

---

## ✨ Features

### 🖥️ Interface
- **Two-pane layout** — repository list (60%) + live detail view (40%)
- **Flicker-free rendering** — `erase()` + `wnoutrefresh()` + `doupdate()` for single atomic terminal write per frame
- **Mouse support** — click to select, double-click to toggle, scroll wheel navigation
- **4 color themes** — Dark, Light, Solarized, Monokai (press `t` to cycle, persisted to config)
- **Live search** — press `/` to filter repositories in real-time, case-insensitive
- **3 sort modes** — by file, by status (enabled first), or alphabetical (press `s`)
- **Scrollbar indicator** — visual position indicator in list pane
- **apt update output pager** — color-coded `Hit/Get/Err` lines in a scrollable ncurses popup

### 🔒 Safety
- **Atomic writes** — all file edits go through `.tmp` → `rename()` (POSIX atomic, never corrupts on crash)
- **Automatic backup** — every write creates a timestamped `.bak` in `/var/backups/ReLix/` first
- **Undo stack** — up to 20 levels of undo (`Ctrl+Z`), per-file
- **Read-only mode** — runs safely without root, all write actions are blocked with clear messaging
- **Root privilege check** at startup with `[READ-ONLY]` badge in header

### 📦 APT Format Support
- **Legacy one-line format** (`.list`) — full enable/disable/delete/add
- **deb822 format** (`.sources`) — block-aware parsing for Ubuntu 22.04+ and Debian 12+
- **Automatic format detection** based on OS version from `/etc/os-release`
- Handles `# deb`, `#deb`, and `deb` comment styles correctly

### 🌐 Repository Metadata
- **Async fetch with 3 s timeout** — non-blocking DNS + TCP reachability check
- **Local cache parsing** — reads `/var/lib/apt/lists/` Release files for Origin, Codename, Suite, Version, Date, Description
- **Last-updated timestamp** from apt cache file mtime

### ⚙️ Management
- **Export** all repositories to a portable text file
- **Import** from a text file with automatic deduplication
- **Config persistence** — theme, sort mode, backup directory saved to `~/.config/ReLix/config`

---

## 🚀 Quick Start

```bash
# 1. Clone
git clone https://github.com/yourusername/ReLix.git
cd ReLix

# 2. Install dependencies (Debian/Ubuntu)
sudo apt install cmake build-essential libncursesw5-dev

# 3. Build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build/ -j$(nproc)

# 4. Run (root required for editing; read-only without)
sudo ./build/ReLix
```

---

## 📋 Requirements

| Requirement | Minimum | Notes |
|---|---|---|
| **OS** | Debian 10 / Ubuntu 20.04 | Any apt-based Linux |
| **Compiler** | GCC 9+ or Clang 10+ | C++17 required |
| **CMake** | 3.16+ | |
| **ncurses** | libncursesw5-dev | Wide-char preferred |
| **Terminal** | 80×24 minimum | 120×40+ recommended |

---

## ⌨️ Keyboard Reference

| Key | Action |
|---|---|
| `↑` / `↓` | Navigate repository list |
| `PgUp` / `PgDn` | Scroll by 10 entries |
| `Home` / `End` | Jump to first / last |
| `F2` | Toggle repository enabled/disabled |
| `F3` | Add new repository |
| `F4` | Delete selected repository |
| `F5` | Run `sudo apt update` (output captured in pager) |
| `F6` | Reload all repository files from disk |
| `F7` | Manual backup of selected file |
| `F8` | Export / Import repository list |
| `m` | Fetch repository metadata (async, 3 s timeout) |
| `t` | Cycle color theme (Dark → Light → Solarized → Monokai) |
| `s` | Cycle sort mode (File → Status → Alphabetical) |
| `/` | Enter live search/filter mode |
| `Esc` | Clear search filter |
| `Ctrl+Z` | Undo last file change |
| `q` / `F10` | Quit and save config |
| **Mouse** | Click = select, Double-click = toggle, Scroll = navigate |

---

## 🗂️ Repository Structure

```
ReLix/
├── main.cpp          # Full application (~1,580 lines, 21 sections)
├── CMakeLists.txt    # Build system with hardening flags
├── README.md
├── TECHNICAL_GUIDE.md
└── LICENSE
```

---

## 🏗️ Build Options

```bash
# Release (default) — optimised + hardened
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build/ -j$(nproc)

# Debug — AddressSanitizer + UBSan enabled automatically
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build/

# Install to /usr/local/bin
sudo cmake --install build/

# Uninstall
sudo cmake --build build/ --target uninstall
```

---

## 🔧 Configuration

ReLix stores preferences in `~/.config/ReLix/config`:

```ini
theme=0            # 0=Dark 1=Light 2=Solarized 3=Monokai
sort=0             # 0=File 1=Status 2=Alphabetical
backup_dir=/var/backups/ReLix
confirmToggle=0    # 1 = ask before every toggle
```

---

## 🛡️ Safety Model

Every destructive operation follows this sequence:

```
1. Read file → memory
2. Push current state onto undo stack (up to 20 levels)
3. Create timestamped backup in backup_dir
4. Write changes to <file>.ReLix.tmp
5. rename() tmp → original   ← POSIX atomic, crash-safe
6. Reload UI from disk
```

No partial writes ever reach your APT source files.

---

## 🤝 Contributing

Pull requests are welcome. For significant changes please open an issue first.

```bash
# Development build with sanitisers
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build/
sudo ./build/ReLix
```

Please maintain the existing code style: single-file C++17, `static` file-scope functions, section comments, no external dependencies beyond ncurses and pthreads.

---

## 📜 License

MIT © 2025 — see [LICENSE](LICENSE) for details.

---

<div align="center">

**ReLix** — manage APT repositories without fear.

*Keywords: apt repository manager, debian repository tool, ubuntu sources.list editor, ncurses tui, deb822, apt sources manager, terminal ui, linux package management*

</div>