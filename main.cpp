/*
 * relix — APT Repository Manager (TUI)
 *
 * Features:
 *   - Two-pane layout (list | detail)
 *   - Mouse support
 *   - Color theme switcher (4 themes)
 *   - Live /filter search
 *   - Sort by name/status/file
 *   - Backup before every write
 *   - Atomic writes (tmp → rename)
 *   - deb822 (.sources) full support
 *   - Repo metadata from apt cache (non-blocking, timeout)
 *   - apt update output pager
 *   - Root check / read-only mode
 *   - Undo stack (Ctrl+Z)
 *   - Export / Import repo list
 *   - Config file persistence
 *
 * Build:
 *   g++ -std=c++17 -O2 -Wall -Wextra -o relix main.cpp \
 *       -lncursesw -lpthread
 *
 * CMake:
 *   find_package(Curses REQUIRED)
 *   find_package(Threads REQUIRED)
 *   add_executable(relix main.cpp)
 *   target_compile_features(relix PRIVATE cxx_std_17)
 *   target_link_libraries(relix ${CURSES_LIBRARIES} Threads::Threads)
 */

/* ─── system headers ──────────────────────────────────────────────────────── */
#include <ncurses.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

/* POSIX / Linux */
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace fs = std::filesystem;

/* ═══════════════════════════════════════════════════════════════════════════
 *  SECTION 1 — STRING UTILITIES
 * ═══════════════════════════════════════════════════════════════════════════ */

static std::string trimStr(const std::string& s) {
    auto st = s.find_first_not_of(" \t\r\n");
    if (st == std::string::npos) return {};
    auto en = s.find_last_not_of(" \t\r\n");
    return s.substr(st, en - st + 1);
}

static std::vector<std::string> splitWords(const std::string& s) {
    std::vector<std::string> r;
    std::istringstream iss(s);
    std::string w;
    while (iss >> w) r.push_back(w);
    return r;
}

static std::string toLower(std::string s) {
    for (auto& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

static bool containsCI(const std::string& haystack, const std::string& needle) {
    return toLower(haystack).find(toLower(needle)) != std::string::npos;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SECTION 2 — CONFIG  (~/.config/relix/config)
 * ═══════════════════════════════════════════════════════════════════════════ */

struct Config {
    int         themeIndex   = 0;  // 0=dark 1=light 2=solarized 3=monokai
    int         sortMode     = 0;  // 0=file 1=status 2=alpha
    std::string backupDir    = "/var/backups/relix";
    bool        confirmToggle = false;
};

static Config g_cfg;

static std::string configPath() {
    const char* home = getenv("HOME");
    return home ? std::string(home) + "/.config/relix/config"
                : "/tmp/relix.config";
}

static void loadConfig() {
    std::ifstream f(configPath());
    if (!f.is_open()) return;
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trimStr(line.substr(0, eq));
        std::string val = trimStr(line.substr(eq + 1));
        if      (key == "theme")         { try { g_cfg.themeIndex   = std::stoi(val); } catch (...) {} }
        else if (key == "sort")          { try { g_cfg.sortMode     = std::stoi(val); } catch (...) {} }
        else if (key == "backup_dir")    { g_cfg.backupDir    = val; }
        else if (key == "confirmToggle") { g_cfg.confirmToggle = (val == "1"); }
    }
    g_cfg.themeIndex = std::max(0, std::min(3, g_cfg.themeIndex));
    g_cfg.sortMode   = std::max(0, std::min(2, g_cfg.sortMode));
}

static void saveConfig() {
    std::string path = configPath();
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open()) return;
    f << "theme="         << g_cfg.themeIndex   << "\n"
      << "sort="          << g_cfg.sortMode      << "\n"
      << "backup_dir="    << g_cfg.backupDir     << "\n"
      << "confirmToggle=" << (g_cfg.confirmToggle ? 1 : 0) << "\n";
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SECTION 3 — COLOR THEMES
 * ═══════════════════════════════════════════════════════════════════════════ */

// Pair IDs (1-based, pair 0 = terminal default)
enum ColorPair {
    CP_HEADER    = 1,  // header bar
    CP_FOOTER    = 2,  // footer / key hints
    CP_STATUS_OK = 3,  // status: success
    CP_STATUS_ERR= 4,  // status: error
    CP_ENABLED   = 5,  // repo enabled
    CP_DISABLED  = 6,  // repo disabled
    CP_DETAIL    = 7,  // detail pane label
    CP_DETAIL_VAL= 8,  // detail pane value
    CP_SEP       = 9,  // separator lines
    CP_SEARCH    = 10, // search bar
    CP_READONLY  = 11, // read-only badge
    CP_PAGER_HIT = 12, // apt pager: HIT
    CP_PAGER_GET = 13, // apt pager: GET
    CP_PAGER_ERR = 14, // apt pager: ERR
    CP_BORDER    = 15, // window borders
};

struct Theme {
    const char* name;
    // {fg, bg} for each pair in order: HEADER,FOOTER,STATUS_OK,STATUS_ERR,
    //  ENABLED,DISABLED,DETAIL,DETAIL_VAL,SEP,SEARCH,READONLY,
    //  PAGER_HIT,PAGER_GET,PAGER_ERR,BORDER
    short pairs[15][2];
};

static const Theme k_themes[] = {
    /* 0 — Dark (default) */
    { "Dark",
      { {COLOR_BLACK,   COLOR_CYAN  },  // HEADER
        {COLOR_YELLOW,  COLOR_BLACK },   // FOOTER
        {COLOR_GREEN,   COLOR_BLACK },   // STATUS_OK
        {COLOR_RED,     COLOR_BLACK },   // STATUS_ERR
        {COLOR_GREEN,   COLOR_BLACK },   // ENABLED
        {COLOR_RED,     COLOR_BLACK },   // DISABLED
        {COLOR_CYAN,    COLOR_BLACK },   // DETAIL label
        {COLOR_WHITE,   COLOR_BLACK },   // DETAIL value
        {COLOR_BLUE,    COLOR_BLACK },   // SEP
        {COLOR_BLACK,   COLOR_YELLOW},   // SEARCH
        {COLOR_BLACK,   COLOR_RED   },   // READONLY
        {COLOR_GREEN,   COLOR_BLACK },   // PAGER_HIT
        {COLOR_CYAN,    COLOR_BLACK },   // PAGER_GET
        {COLOR_RED,     COLOR_BLACK },   // PAGER_ERR
        {COLOR_CYAN,    COLOR_BLACK },   // BORDER
      }
    },
    /* 1 — Light */
    { "Light",
      { {COLOR_WHITE,  COLOR_BLUE  },
        {COLOR_BLUE,   COLOR_WHITE },
        {COLOR_GREEN,  COLOR_WHITE },
        {COLOR_RED,    COLOR_WHITE },
        {COLOR_GREEN,  COLOR_WHITE },
        {COLOR_RED,    COLOR_WHITE },
        {COLOR_BLUE,   COLOR_WHITE },
        {COLOR_BLACK,  COLOR_WHITE },
        {COLOR_BLUE,   COLOR_WHITE },
        {COLOR_WHITE,  COLOR_BLUE  },
        {COLOR_WHITE,  COLOR_RED   },
        {COLOR_GREEN,  COLOR_WHITE },
        {COLOR_BLUE,   COLOR_WHITE },
        {COLOR_RED,    COLOR_WHITE },
        {COLOR_BLUE,   COLOR_WHITE },
      }
    },
    /* 2 — Solarized Dark */
    { "Solarized",
      { {COLOR_BLACK,   COLOR_YELLOW},
        {COLOR_YELLOW,  COLOR_BLACK },
        {COLOR_GREEN,   COLOR_BLACK },
        {COLOR_RED,     COLOR_BLACK },
        {COLOR_GREEN,   COLOR_BLACK },
        {COLOR_RED,     COLOR_BLACK },
        {COLOR_YELLOW,  COLOR_BLACK },
        {COLOR_WHITE,   COLOR_BLACK },
        {COLOR_YELLOW,  COLOR_BLACK },
        {COLOR_BLACK,   COLOR_CYAN  },
        {COLOR_BLACK,   COLOR_RED   },
        {COLOR_GREEN,   COLOR_BLACK },
        {COLOR_CYAN,    COLOR_BLACK },
        {COLOR_RED,     COLOR_BLACK },
        {COLOR_YELLOW,  COLOR_BLACK },
      }
    },
    /* 3 — Monokai */
    { "Monokai",
      { {COLOR_WHITE,   COLOR_MAGENTA},
        {COLOR_MAGENTA, COLOR_BLACK  },
        {COLOR_GREEN,   COLOR_BLACK  },
        {COLOR_RED,     COLOR_BLACK  },
        {COLOR_GREEN,   COLOR_BLACK  },
        {COLOR_RED,     COLOR_BLACK  },
        {COLOR_MAGENTA, COLOR_BLACK  },
        {COLOR_WHITE,   COLOR_BLACK  },
        {COLOR_MAGENTA, COLOR_BLACK  },
        {COLOR_BLACK,   COLOR_WHITE  },
        {COLOR_BLACK,   COLOR_RED    },
        {COLOR_GREEN,   COLOR_BLACK  },
        {COLOR_CYAN,    COLOR_BLACK  },
        {COLOR_RED,     COLOR_BLACK  },
        {COLOR_MAGENTA, COLOR_BLACK  },
      }
    },
};
static constexpr int k_themeCount = static_cast<int>(sizeof(k_themes)/sizeof(k_themes[0]));

static void applyTheme(int idx) {
    idx = std::max(0, std::min(k_themeCount - 1, idx));
    const auto& t = k_themes[idx];
    for (int i = 0; i < 15; i++)
        init_pair(static_cast<short>(i + 1), t.pairs[i][0], t.pairs[i][1]);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SECTION 4 — OS DETECTION
 * ═══════════════════════════════════════════════════════════════════════════ */

struct OSInfo { std::string id; double version; };

static OSInfo detectOS() {
    OSInfo info{"unknown", 0.0};
    std::ifstream f("/etc/os-release");
    if (!f.is_open()) return info;
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("ID=", 0) == 0) {
            info.id = trimStr(line.substr(3));
            info.id.erase(std::remove(info.id.begin(), info.id.end(), '"'), info.id.end());
        } else if (line.rfind("VERSION_ID=", 0) == 0) {
            std::string vs = trimStr(line.substr(11));
            vs.erase(std::remove(vs.begin(), vs.end(), '"'), vs.end());
            try { info.version = std::stod(vs); } catch (...) {}
        }
    }
    return info;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SECTION 5 — REPO STRUCT + GLOBALS
 * ═══════════════════════════════════════════════════════════════════════════ */

struct RepoEntry {
    std::string file;       // source file path
    std::string display;    // raw line (.list) or formatted string (.sources)
    bool        enabled;
    bool        isDeb822;
    int         blockIndex; // deb822 block (-1 for .list)
    /* parsed fields (always populated for detail pane) */
    std::string uri;
    std::string suite;
    std::string components;
    std::string types;
};

static std::vector<RepoEntry> g_repos;      // master list
static std::vector<int>       g_filtered;   // indices into g_repos after filter/sort
static OSInfo                 g_os;
static bool                   g_isRoot   = false;
static bool                   g_readOnly = false;

/* ─── undo stack ─────────────────────────────────────────────────────────── */
struct UndoEntry {
    std::string file;
    std::vector<std::string> lines;
};
static std::vector<UndoEntry> g_undoStack;
static constexpr size_t k_maxUndo = 20;

/* ═══════════════════════════════════════════════════════════════════════════
 *  SECTION 6 — PARSE FILES
 * ═══════════════════════════════════════════════════════════════════════════ */

static void parseListFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return;
    std::string line;
    while (std::getline(file, line)) {
        std::string t = trimStr(line);
        if (t.empty()) continue;
        bool isDeb  = (t.rfind("deb", 0) == 0);
        bool isHDeb = (t[0] == '#' && trimStr(t.substr(1)).rfind("deb", 0) == 0);
        if (!isDeb && !isHDeb) continue;

        bool enabled = (t[0] != '#');
        // Parse fields for detail pane
        std::string parseable = enabled ? t : trimStr(t.substr(t[1] == ' ' ? 2 : 1));
        auto words = splitWords(parseable);
        RepoEntry e;
        e.file       = path;
        e.display    = line;
        e.enabled    = enabled;
        e.isDeb822   = false;
        e.blockIndex = -1;
        e.types      = "deb";
        if (words.size() > 1) e.uri       = words[1];
        if (words.size() > 2) e.suite     = words[2];
        if (words.size() > 3) {
            for (size_t i = 3; i < words.size(); i++) {
                if (!e.components.empty()) e.components += " ";
                e.components += words[i];
            }
        }
        g_repos.push_back(std::move(e));
    }
}

static void parseSourcesFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return;

    std::vector<std::string> block;
    std::string line;
    int blockIndex = 0;

    auto processBlock = [&](const std::vector<std::string>& blines) {
        std::string              types, uri_raw, suites_raw, comp_raw;
        std::vector<std::string> uris, suites, comps;
        bool                     enabled = true;

        for (auto l : blines) {
            l = trimStr(l);
            if (l.empty() || l[0] == '#') continue;
            if      (l.rfind("Types:",      0) == 0) types     = trimStr(l.substr(6));
            else if (l.rfind("URIs:",       0) == 0) { uri_raw   = trimStr(l.substr(5)); uris   = splitWords(uri_raw); }
            else if (l.rfind("Suites:",     0) == 0) { suites_raw= trimStr(l.substr(7)); suites = splitWords(suites_raw); }
            else if (l.rfind("Components:", 0) == 0) { comp_raw  = trimStr(l.substr(11)); comps  = splitWords(comp_raw); }
            else if (l.rfind("Enabled:",    0) == 0) {
                std::string v = trimStr(l.substr(8));
                enabled = (v == "yes" || v == "Yes" || v == "YES");
            }
        }

        if (types.find("deb") == std::string::npos) return;
        if (uris.empty() || suites.empty()) return;

        for (const auto& u : uris) {
            for (const auto& s : suites) {
                std::string display = types + " " + u + " " + s;
                if (!comps.empty()) {
                    display += " ";
                    for (const auto& c : comps) display += c + " ";
                    display.pop_back();
                }
                RepoEntry e;
                e.file       = path;
                e.display    = display;
                e.enabled    = enabled;
                e.isDeb822   = true;
                e.blockIndex = blockIndex;
                e.types      = types;
                e.uri        = u;
                e.suite      = s;
                e.components = comp_raw;
                g_repos.push_back(std::move(e));
            }
        }
        blockIndex++;
    };

    while (std::getline(file, line)) {
        if (trimStr(line).empty()) {
            if (!block.empty()) { processBlock(block); block.clear(); }
        } else {
            block.push_back(line);
        }
    }
    if (!block.empty()) processBlock(block);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SECTION 7 — LOAD + FILTER + SORT
 * ═══════════════════════════════════════════════════════════════════════════ */

static std::string g_filterStr;

static void rebuildFiltered() {
    g_filtered.clear();
    for (int i = 0; i < (int)g_repos.size(); i++) {
        if (g_filterStr.empty() || containsCI(g_repos[i].display, g_filterStr))
            g_filtered.push_back(i);
    }
    // Sort
    auto cmp = [&](int a, int b) -> bool {
        const auto& ra = g_repos[a];
        const auto& rb = g_repos[b];
        switch (g_cfg.sortMode) {
            case 1: // status first (enabled first), then alpha
                if (ra.enabled != rb.enabled) return ra.enabled > rb.enabled;
                return ra.display < rb.display;
            case 2: // pure alpha
                return toLower(ra.display) < toLower(rb.display);
            default: // by file then display
                if (ra.file != rb.file) return ra.file < rb.file;
                return ra.display < rb.display;
        }
    };
    std::stable_sort(g_filtered.begin(), g_filtered.end(), cmp);
}

static void loadRepos() {
    g_repos.clear();
    bool useDeb822 = ((g_os.id == "ubuntu" && g_os.version >= 22.04) ||
                      (g_os.id == "debian"  && g_os.version >= 12.0));

    const std::string mainList = "/etc/apt/sources.list";
    const std::string dir      = "/etc/apt/sources.list.d/";

    if (fs::exists(mainList)) parseListFile(mainList);
    if (fs::exists(dir)) {
        // Sort directory entries for deterministic order
        std::vector<fs::directory_entry> entries(fs::directory_iterator(dir),
                                                 fs::directory_iterator{});
        std::sort(entries.begin(), entries.end());
        for (const auto& e : entries) {
            auto ext = e.path().extension();
            if (ext == ".list")
                parseListFile(e.path().string());
            else if (useDeb822 && ext == ".sources")
                parseSourcesFile(e.path().string());
        }
    }
    rebuildFiltered();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SECTION 8 — BACKUP
 * ═══════════════════════════════════════════════════════════════════════════ */

static bool backupFile(const std::string& src, std::string& errMsg) {
    std::error_code ec;
    fs::create_directories(g_cfg.backupDir, ec);
    if (ec) { errMsg = "Cannot create backup dir: " + ec.message(); return false; }

    // Timestamp
    auto now = std::chrono::system_clock::now();
    auto t   = std::chrono::system_clock::to_time_t(now);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", std::localtime(&t));

    // Derive backup filename: replace '/' with '_'
    std::string base = src;
    std::replace(base.begin(), base.end(), '/', '_');
    std::string dest = g_cfg.backupDir + "/" + base + "." + ts + ".bak";

    fs::copy_file(src, dest, fs::copy_options::overwrite_existing, ec);
    if (ec) { errMsg = "Backup copy failed: " + ec.message(); return false; }
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SECTION 9 — ATOMIC WRITE + UNDO STACK
 * ═══════════════════════════════════════════════════════════════════════════ */

static std::vector<std::string> readAllLines(const std::string& path) {
    std::ifstream f(path);
    std::vector<std::string> lines;
    std::string l;
    while (std::getline(f, l)) lines.push_back(l);
    return lines;
}

static bool atomicWriteLines(const std::string& path,
                             const std::vector<std::string>& lines,
                             std::string& errMsg)
{
    std::string tmp = path + ".relix.tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out.is_open()) { errMsg = "Cannot open tmp file"; return false; }
        for (const auto& l : lines) out << l << "\n";
        out.flush();
        if (!out.good()) { errMsg = "Write error on tmp file"; return false; }
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        errMsg = std::string("rename() failed: ") + std::strerror(errno);
        return false;
    }
    return true;
}

// Call before any destructive write; saves old file state to undo stack
static void pushUndo(const std::string& path) {
    auto lines = readAllLines(path);
    if (g_undoStack.size() >= k_maxUndo) g_undoStack.erase(g_undoStack.begin());
    g_undoStack.push_back({path, std::move(lines)});
}

static bool applyUndo(std::string& errMsg) {
    if (g_undoStack.empty()) { errMsg = "Nothing to undo."; return false; }
    auto& u = g_undoStack.back();
    if (!atomicWriteLines(u.file, u.lines, errMsg)) return false;
    g_undoStack.pop_back();
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SECTION 10 — TOGGLE LOGIC
 * ═══════════════════════════════════════════════════════════════════════════ */

static bool toggleList(const RepoEntry& repo, std::string& errMsg) {
    auto lines = readAllLines(repo.file);
    bool found = false;
    for (auto& l : lines) {
        if (!found && l == repo.display) {
            found = true;
            l = repo.enabled ? ("# " + l) : trimStr(l.substr(l[1] == ' ' ? 2 : 1));
        }
    }
    if (!found) { errMsg = "Line not found in file (changed externally?)"; return false; }
    pushUndo(repo.file);
    std::string be;
    if (!backupFile(repo.file, be))
        errMsg = "[warn] backup skipped: " + be; // non-fatal
    return atomicWriteLines(repo.file, lines, errMsg);
}

static bool toggleDeb822(const RepoEntry& repo, std::string& errMsg) {
    auto allLines = readAllLines(repo.file);

    // Identify block ranges
    struct Range { int s, e, enabledLine; };
    std::vector<Range> blocks;
    int bs = -1; bool inB = false;
    for (int i = 0; i < (int)allLines.size(); i++) {
        bool blank = trimStr(allLines[i]).empty();
        if (!blank && !inB) { bs = i; inB = true; }
        if ( blank &&  inB) { blocks.push_back({bs, i-1, -1}); inB = false; }
    }
    if (inB) blocks.push_back({bs, (int)allLines.size()-1, -1});

    // Find Enabled: line for each block
    for (auto& b : blocks)
        for (int i = b.s; i <= b.e; i++)
            if (trimStr(allLines[i]).rfind("Enabled:", 0) == 0)
                { b.enabledLine = i; break; }

    if (repo.blockIndex < 0 || repo.blockIndex >= (int)blocks.size()) {
        errMsg = "Block index out of range (file changed externally?)"; return false;
    }
    auto& b = blocks[repo.blockIndex];
    std::string newVal = repo.enabled ? "Enabled: no" : "Enabled: yes";
    if (b.enabledLine >= 0) {
        allLines[b.enabledLine] = newVal;
    } else {
        // Insert after first line of block
        allLines.insert(allLines.begin() + b.s + 1, newVal);
    }
    pushUndo(repo.file);
    std::string be;
    if (!backupFile(repo.file, be))
        errMsg = "[warn] backup skipped: " + be;
    return atomicWriteLines(repo.file, allLines, errMsg);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SECTION 11 — DELETE LOGIC
 * ═══════════════════════════════════════════════════════════════════════════ */

static bool deleteRepoClean(const RepoEntry& repo, std::string& errMsg) {
    auto allLines = readAllLines(repo.file);
    std::vector<std::string> outLines;

    if (!repo.isDeb822) {
        bool found = false;
        for (const auto& l : allLines) {
            if (!found && l == repo.display) { found = true; continue; }
            outLines.push_back(l);
        }
        if (!found) { errMsg = "Line not found in file"; return false; }
    } else {
        struct Range { int s, e; };
        std::vector<Range> blockRanges;
        int bs = -1; bool inB = false;
        for (int i = 0; i < (int)allLines.size(); i++) {
            bool blank = trimStr(allLines[i]).empty();
            if (!blank && !inB) { bs = i; inB = true; }
            if ( blank &&  inB) { blockRanges.push_back({bs, i-1}); inB = false; }
        }
        if (inB) blockRanges.push_back({bs, (int)allLines.size()-1});

        if (repo.blockIndex < 0 || repo.blockIndex >= (int)blockRanges.size()) {
            errMsg = "Block index out of range"; return false;
        }
        int bStart = blockRanges[repo.blockIndex].s;
        int bEnd   = blockRanges[repo.blockIndex].e;
        if (bEnd + 1 < (int)allLines.size() && trimStr(allLines[bEnd+1]).empty())
            bEnd++; // swallow trailing blank

        for (int i = 0; i < (int)allLines.size(); i++) {
            if (i >= bStart && i <= bEnd) continue;
            outLines.push_back(allLines[i]);
        }
    }
    pushUndo(repo.file);
    std::string be;
    if (!backupFile(repo.file, be)) errMsg = "[warn] backup skipped: " + be;
    return atomicWriteLines(repo.file, outLines, errMsg);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SECTION 12 — EXPORT / IMPORT
 * ═══════════════════════════════════════════════════════════════════════════ */

static bool exportRepos(const std::string& path, std::string& errMsg) {
    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open()) { errMsg = "Cannot open " + path; return false; }
    f << "# APT Repository Export — relix\n";
    char ts[32]; auto t = std::time(nullptr);
    std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    f << "# Generated: " << ts << "\n\n";
    for (const auto& r : g_repos) {
        f << (r.enabled ? "" : "# ")
          << "deb " << r.uri << " " << r.suite;
        if (!r.components.empty()) f << " " << r.components;
        f << "  # from: " << r.file << "\n";
    }
    return f.good() ? true : (errMsg = "Write error", false);
}

static bool importRepos(const std::string& path, std::string& errMsg) {
    std::ifstream f(path);
    if (!f.is_open()) { errMsg = "Cannot open " + path; return false; }

    // Collect existing displays for dedup
    std::vector<std::string> existing;
    for (const auto& r : g_repos) existing.push_back(trimStr(r.display));

    std::ofstream out("/etc/apt/sources.list", std::ios::app);
    if (!out.is_open()) { errMsg = "Cannot open /etc/apt/sources.list for append"; return false; }

    std::string line; int added = 0;
    while (std::getline(f, line)) {
        std::string t = trimStr(line);
        if (t.empty() || t[0] == '#') continue;
        if (t.rfind("deb", 0) != 0)    continue;
        // Check dedup
        bool dup = false;
        for (const auto& ex : existing)
            if (toLower(ex).find(toLower(t.substr(4))) != std::string::npos)
                { dup = true; break; }
        if (!dup) { out << t << "\n"; added++; }
    }
    if (added == 0) errMsg = "No new repos found to import.";
    else errMsg = std::to_string(added) + " repo(s) imported.";
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SECTION 13 — REPO METADATA (async, non-blocking, 3 s timeout)
 * ═══════════════════════════════════════════════════════════════════════════ */

struct RepoMeta {
    std::string origin;
    std::string codename;
    std::string suite;
    std::string version;
    std::string date;
    std::string description;
    std::string lastUpdate; // from local apt cache mtime
    bool        reachable = false;
    std::string error;
};

// Read apt cache Release file for this repo
static RepoMeta metaFromCache(const RepoEntry& repo) {
    RepoMeta m;
    // apt cache: /var/lib/apt/lists/<host>_dists_<suite>_Release
    if (repo.uri.empty() || repo.suite.empty()) return m;

    // Derive cache prefix from URI
    // e.g. http://archive.ubuntu.com/ubuntu → archive.ubuntu.com_ubuntu
    std::string host = repo.uri;
    // strip scheme
    auto spos = host.find("://");
    if (spos != std::string::npos) host = host.substr(spos + 3);
    // replace / with _
    std::replace(host.begin(), host.end(), '/', '_');
    // strip trailing _
    while (!host.empty() && host.back() == '_') host.pop_back();

    std::string suite = repo.suite;
    std::replace(suite.begin(), suite.end(), '/', '_');

    std::string relPath = "/var/lib/apt/lists/" + host + "_dists_" + suite + "_Release";

    // Check mtime for "last updated"
    struct stat st{};
    if (::stat(relPath.c_str(), &st) == 0) {
        char buf[64];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", std::localtime(&st.st_mtime));
        m.lastUpdate = buf;
    }

    std::ifstream f(relPath);
    if (!f.is_open()) { m.error = "Cache not found (run apt update)"; return m; }

    std::string line;
    while (std::getline(f, line)) {
        if      (line.rfind("Origin:",      0) == 0) m.origin      = trimStr(line.substr(7));
        else if (line.rfind("Codename:",    0) == 0) m.codename    = trimStr(line.substr(9));
        else if (line.rfind("Suite:",       0) == 0) m.suite       = trimStr(line.substr(6));
        else if (line.rfind("Version:",     0) == 0) m.version     = trimStr(line.substr(8));
        else if (line.rfind("Date:",        0) == 0) m.date        = trimStr(line.substr(5));
        else if (line.rfind("Description:", 0) == 0) m.description = trimStr(line.substr(12));
    }
    return m;
}

// Non-blocking TCP reachability check with timeout_ms milliseconds
static bool checkReachable(const std::string& uri, int timeout_ms = 3000) {
    // Extract host and port from URI
    std::string host;
    std::string portStr = "80";
    auto spos = uri.find("://");
    host = (spos != std::string::npos) ? uri.substr(spos + 3) : uri;
    // check for https
    if (uri.rfind("https", 0) == 0) portStr = "443";
    // strip path
    auto slash = host.find('/');
    if (slash != std::string::npos) host = host.substr(0, slash);
    // split host:port
    auto colon = host.rfind(':');
    if (colon != std::string::npos) { portStr = host.substr(colon + 1); host = host.substr(0, colon); }

    struct addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    // getaddrinfo with timeout via separate thread
    std::atomic<int> gai_ret{-1};
    struct addrinfo* gai_res = nullptr;
    std::mutex mtx; std::condition_variable cv; bool done = false;

    std::thread([&]{
        gai_ret = getaddrinfo(host.c_str(), portStr.c_str(), &hints, &gai_res);
        std::lock_guard<std::mutex> lk(mtx);
        done = true; cv.notify_one();
    }).detach();

    {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&]{ return done; });
        if (!done) return false; // DNS timeout
    }
    if (gai_ret != 0 || !gai_res) return false;

    int sock = socket(gai_res->ai_family, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (sock < 0) { freeaddrinfo(gai_res); return false; }

    ::connect(sock, gai_res->ai_addr, gai_res->ai_addrlen); // will EINPROGRESS
    freeaddrinfo(gai_res);

    fd_set wfds; FD_ZERO(&wfds); FD_SET(sock, &wfds);
    struct timeval tv{ timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    int sel = select(sock + 1, nullptr, &wfds, nullptr, &tv);
    ::close(sock);
    return sel == 1;
}

struct AsyncMeta {
    std::mutex         mtx;
    RepoMeta           meta;
    std::atomic<bool>  ready{false};
    std::atomic<bool>  running{false};
    std::string        lastUri;  // which repo we fetched for
};
static AsyncMeta g_asyncMeta;

static void fetchMetaAsync(const RepoEntry& repo) {
    if (g_asyncMeta.running) return; // already in flight
    g_asyncMeta.ready   = false;
    g_asyncMeta.running = true;
    g_asyncMeta.lastUri = repo.uri + repo.suite;

    // Capture by value so thread is safe after caller returns
    RepoEntry r = repo;
    std::thread([r]() {
        RepoMeta m = metaFromCache(r);
        m.reachable = checkReachable(r.uri, 3000);
        std::lock_guard<std::mutex> lk(g_asyncMeta.mtx);
        g_asyncMeta.meta    = m;
        g_asyncMeta.ready   = true;
        g_asyncMeta.running = false;
    }).detach();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SECTION 14 — UI STATE
 * ═══════════════════════════════════════════════════════════════════════════ */

static int         g_selected    = 0;
static int         g_scrollOff   = 0;
static std::string g_status;
static bool        g_statusErr   = false;
static bool        g_searchMode  = false;
static RepoMeta    g_curMeta;
static bool        g_metaShown   = false;

static void setStatus(const std::string& msg, bool isErr = false) {
    g_status    = msg;
    g_statusErr = isErr;
}

static void clampSelection() {
    int sz = (int)g_filtered.size();
    if (sz == 0) { g_selected = 0; g_scrollOff = 0; return; }
    g_selected = std::max(0, std::min(g_selected, sz - 1));
    int listH  = LINES - 5;
    if (listH < 1) listH = 1;
    if (g_scrollOff > g_selected)              g_scrollOff = g_selected;
    if (g_selected >= g_scrollOff + listH)     g_scrollOff = g_selected - listH + 1;
    g_scrollOff = std::max(0, g_scrollOff);
}

// Handy accessor: index into g_repos for the currently selected filtered entry
static int currentRepoIndex() {
    if (g_filtered.empty()) return -1;
    return g_filtered[g_selected];
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SECTION 15 — LAYOUT CONSTANTS (computed at runtime)
 * ═══════════════════════════════════════════════════════════════════════════ */
//
//  Row 0       : header
//  Row 1       : separator
//  Rows 2..H-5 : list pane (left) + detail pane (right)
//  Row H-4     : separator
//  Row H-3     : detail line 1 (file)
//  Row H-2     : status bar
//  Row H-1     : footer / key hints
//
//  Cols 0..splitCol-1 : list pane
//  Col  splitCol      : vertical separator │
//  Cols splitCol+1..  : detail pane

static int listPaneW()  { return std::max(20, COLS * 60 / 100); }  // 60% width
static int detailPaneX(){ return listPaneW() + 1; }
static int detailPaneW(){ return std::max(0, COLS - detailPaneX()); }
static int listHeight() { return std::max(1, LINES - 5); }

/* ═══════════════════════════════════════════════════════════════════════════
 *  SECTION 16 — DRAWING
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ─── flicker-free render helpers ────────────────────────────────────────────
 *
 *  Root cause of flashing:
 *   1. clear() sends an erase-screen escape to the terminal immediately,
 *      leaving a blank frame visible until refresh() writes new content.
 *   2. Every draw function called refresh() on its own window — multiple
 *      physical writes per frame.
 *   3. The event loop called redraw() twice when async meta arrived (once
 *      from the poll branch, once from the unconditional call below it).
 *
 *  Fix:
 *   • Replace clear() with erase() — marks cells dirty in ncurses' shadow
 *     buffer only; nothing hits the terminal until doupdate().
 *   • Replace every refresh() / wrefresh() with wnoutrefresh(stdscr) /
 *     wnoutrefresh(win) — batches all updates in the shadow buffer.
 *   • One single doupdate() at the end of redraw() flushes everything to
 *     the terminal in one write, producing zero blank frames.
 *   • Remove the redundant double-redraw in the event loop.
 * ─────────────────────────────────────────────────────────────────────────── */

static void drawHeader() {
    attron(COLOR_PAIR(CP_HEADER) | A_BOLD);
    std::string title = " Relix - APT Repository Manager";
    if (g_readOnly) title += "  [READ-ONLY]";
    title += "   OS: " + g_os.id;
    char ver[16]; snprintf(ver, sizeof(ver), " %.2f", g_os.version);
    title += ver;
    title += "   Theme: ";
    title += k_themes[g_cfg.themeIndex].name;
    title += "   Sort: ";
    static const char* sortNames[] = {"File","Status","Alpha"};
    title += sortNames[g_cfg.sortMode];
    if ((int)title.size() < COLS) title += std::string(COLS - title.size(), ' ');
    mvprintw(0, 0, "%s", title.substr(0, COLS).c_str());
    attroff(COLOR_PAIR(CP_HEADER) | A_BOLD);
}

static void drawSeparators() {
    attron(COLOR_PAIR(CP_SEP));
    mvhline(1,       0, ACS_HLINE, COLS);
    mvhline(LINES-4, 0, ACS_HLINE, COLS);
    for (int y = 1; y < LINES - 4; y++)
        mvaddch(y, listPaneW(), ACS_VLINE);
    mvaddch(1,       listPaneW(), ACS_TTEE);
    mvaddch(LINES-4, listPaneW(), ACS_BTEE);
    attroff(COLOR_PAIR(CP_SEP));
}

static void drawList() {
    int top = 2;
    int lh  = listHeight();
    int lpw = listPaneW();

    for (int i = 0; i < lh; i++) {
        int fIdx = i + g_scrollOff;

        // Write a full-width blank line into the shadow buffer (no terminal I/O)
        move(top + i, 0);
        for (int x = 0; x < lpw; x++) addch(' ');

        if (fIdx >= (int)g_filtered.size()) continue;

        int rIdx       = g_filtered[fIdx];
        const auto& r  = g_repos[rIdx];
        bool sel       = (fIdx == g_selected);
        int  pair      = r.enabled ? CP_ENABLED : CP_DISABLED;
        attr_t attrs   = COLOR_PAIR(pair);
        if (sel) attrs |= A_REVERSE | A_BOLD;

        attron(attrs);
        const char* icon = r.enabled ? "\xe2\x97\x8f " : "\xe2\x97\x8b "; // ● / ○ UTF-8
        std::string disp = icon + r.display;
        if ((int)disp.size() > lpw - 2)
            disp = disp.substr(0, lpw - 5) + "...";
        while ((int)disp.size() < lpw - 1) disp += ' ';
        mvprintw(top + i, 1, "%s", disp.substr(0, lpw - 1).c_str());
        attroff(attrs);
    }

    // Scrollbar
    if ((int)g_filtered.size() > lh) {
        attron(COLOR_PAIR(CP_SEP) | A_DIM);
        int barH   = std::max(1, lh * lh / (int)g_filtered.size());
        int barTop = lh * g_scrollOff / (int)g_filtered.size();
        for (int y = 0; y < lh; y++)
            mvaddch(top + y, lpw - 1,
                    (y >= barTop && y < barTop + barH) ? ACS_BLOCK : ACS_VLINE);
        attroff(COLOR_PAIR(CP_SEP) | A_DIM);
    }
}

static void drawDetailPane() {
    int top = 2;
    int lh  = listHeight();
    int dx  = detailPaneX();
    int dw  = detailPaneW();
    if (dw < 5) return;

    // Blank the detail area in the shadow buffer — no terminal write yet
    for (int y = top; y < top + lh; y++) {
        move(y, dx);
        for (int x = dx; x < COLS; x++) addch(' ');
    }

    if (g_filtered.empty()) {
        attron(COLOR_PAIR(CP_DETAIL) | A_DIM);
        mvprintw(top + lh/2, dx + 2, "No repositories found.");
        attroff(COLOR_PAIR(CP_DETAIL) | A_DIM);
        return;
    }

    int rIdx = currentRepoIndex();
    if (rIdx < 0) return;
    const auto& r = g_repos[rIdx];

    int y = top;
    auto printField = [&](const char* label, const std::string& val) {
        if (y >= top + lh) return;
        attron(COLOR_PAIR(CP_DETAIL) | A_BOLD);
        mvprintw(y, dx + 1, "%-12s", label);
        attroff(COLOR_PAIR(CP_DETAIL) | A_BOLD);
        attron(COLOR_PAIR(CP_DETAIL_VAL));
        mvprintw(y, dx + 13, "%s", val.substr(0, (size_t)(dw - 14)).c_str());
        attroff(COLOR_PAIR(CP_DETAIL_VAL));
        y++;
    };

    printField("Status:",  r.enabled ? "ENABLED" : "DISABLED");
    printField("Format:",  r.isDeb822 ? "deb822 (.sources)" : "one-line (.list)");
    printField("Type:",    r.types.empty() ? "deb" : r.types);
    printField("URI:",     r.uri);
    printField("Suite:",   r.suite);
    printField("Comps:",   r.components);
    printField("File:",    r.file);
    if (r.isDeb822) {
        char blk[16]; snprintf(blk, sizeof(blk), "%d", r.blockIndex);
        printField("Block:", blk);
    }
    y++;

    attron(COLOR_PAIR(CP_SEP));
    if (y < top + lh) mvhline(y, dx, ACS_HLINE, dw);
    y++;
    attroff(COLOR_PAIR(CP_SEP));

    // Collect async meta result — only lock briefly to copy the struct
    if (g_asyncMeta.ready.load()) {
        std::lock_guard<std::mutex> lk(g_asyncMeta.mtx);
        g_curMeta   = g_asyncMeta.meta;
        g_metaShown = true;
        // Clear the flag so we don't keep locking on every frame
        g_asyncMeta.ready.store(false);
    }

    if (g_asyncMeta.running) {
        if (y < top + lh) {
            attron(COLOR_PAIR(CP_DETAIL) | A_DIM);
            mvprintw(y++, dx + 1, "Fetching metadata...");
            attroff(COLOR_PAIR(CP_DETAIL) | A_DIM);
        }
    } else if (g_metaShown) {
        int pair = g_curMeta.reachable ? CP_STATUS_OK : CP_STATUS_ERR;
        attron(COLOR_PAIR(pair));
        if (y < top + lh)
            mvprintw(y++, dx + 1, "Reachable:   %s",
                     g_curMeta.reachable ? "Yes" : "No");
        attroff(COLOR_PAIR(pair));
        if (!g_curMeta.error.empty()) {
            if (y < top + lh) {
                attron(COLOR_PAIR(CP_STATUS_ERR) | A_DIM);
                mvprintw(y++, dx + 1, "%s", g_curMeta.error.substr(0, (size_t)(dw-2)).c_str());
                attroff(COLOR_PAIR(CP_STATUS_ERR) | A_DIM);
            }
        } else {
            printField("Origin:",   g_curMeta.origin);
            printField("Codename:", g_curMeta.codename);
            printField("Suite:",    g_curMeta.suite);
            printField("Version:",  g_curMeta.version);
            printField("Date:",     g_curMeta.date);
            printField("Updated:",  g_curMeta.lastUpdate);
            if (!g_curMeta.description.empty())
                printField("Desc:", g_curMeta.description);
        }
    } else {
        if (y < top + lh) {
            attron(COLOR_PAIR(CP_DETAIL) | A_DIM);
            mvprintw(y++, dx + 1, "Press 'm' to fetch metadata");
            attroff(COLOR_PAIR(CP_DETAIL) | A_DIM);
        }
    }
}

static void drawFooter() {
    attron(COLOR_PAIR(CP_FOOTER));
    std::string keys =
        " F2:Toggle F3:Add F4:Del F5:Update F6:Reload "
        "F7:Backup F8:Export m:Meta t:Theme s:Sort /:Search ^Z:Undo q:Quit";
    if ((int)keys.size() < COLS) keys += std::string(COLS - keys.size(), ' ');
    mvprintw(LINES - 1, 0, "%s", keys.substr(0, COLS).c_str());
    attroff(COLOR_PAIR(CP_FOOTER));
}

static void drawStatus() {
    move(LINES - 2, 0);
    for (int x = 0; x < COLS; x++) addch(' '); // blank in shadow buffer only

    if (g_searchMode) {
        attron(COLOR_PAIR(CP_SEARCH) | A_BOLD);
        mvprintw(LINES - 2, 0, " Search: %s_", g_filterStr.c_str());
        attroff(COLOR_PAIR(CP_SEARCH) | A_BOLD);
    } else {
        int pair = g_statusErr ? CP_STATUS_ERR : CP_STATUS_OK;
        attron(COLOR_PAIR(pair));
        char cnt[32];
        snprintf(cnt, sizeof(cnt), " [%d/%d] ",
                 (int)g_filtered.size(), (int)g_repos.size());
        mvprintw(LINES - 2, 0, "%s%s", cnt,
                 g_status.substr(0, COLS - 20).c_str());
        attroff(COLOR_PAIR(pair));
    }
}

static void redraw() {
    clampSelection();
    // erase() marks the shadow buffer as blank — zero terminal writes here.
    // This replaces clear() which flushed a blank frame to the terminal
    // immediately, causing the visible flash on every redraw.
    erase();
    drawHeader();
    drawSeparators();
    drawList();
    drawDetailPane();
    drawStatus();
    drawFooter();
    // wnoutrefresh(stdscr) copies our shadow buffer to ncurses' virtual screen.
    // doupdate() then diffs the virtual screen against what the terminal
    // actually shows and sends only the changed bytes — one atomic write,
    // no blank frame, no flash.
    wnoutrefresh(stdscr);
    doupdate();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SECTION 17 — POPUP DIALOGS
 * ═══════════════════════════════════════════════════════════════════════════ */

static void popupCleanup(WINDOW* win) {
    werase(win);
    wnoutrefresh(win);  // mark popup area dirty in virtual screen — no immediate flush
    delwin(win);
    touchwin(stdscr);   // tell ncurses stdscr needs full repaint
    // doupdate() intentionally omitted — the next redraw() handles it cleanly
}

static bool confirmDialog(const std::string& msg) {
    int w = std::min(74, COLS - 4), h = 6;
    WINDOW* win = newwin(h, w, (LINES-h)/2, (COLS-w)/2);
    wattron(win, COLOR_PAIR(CP_BORDER));
    box(win, 0, 0);
    wattroff(win, COLOR_PAIR(CP_BORDER));
    wattron(win, A_BOLD);
    mvwprintw(win, 1, 2, "Confirm Action");
    wattroff(win, A_BOLD);
    mvwprintw(win, 3, 2, "%s", msg.substr(0, w-4).c_str());
    mvwprintw(win, 4, 2, "Press [y] to confirm, any other key to cancel.");
    keypad(win, TRUE);
    wnoutrefresh(win); doupdate();
    int ch = wgetch(win);
    popupCleanup(win);
    return ch == 'y' || ch == 'Y';
}

static std::string inputDialog(const std::string& title, const std::string& prompt,
                               const std::string& prefill = "") {
    int w = std::min(76, COLS - 4), h = 8;
    WINDOW* win = newwin(h, w, (LINES-h)/2, (COLS-w)/2);
    wattron(win, COLOR_PAIR(CP_BORDER));
    box(win, 0, 0);
    wattroff(win, COLOR_PAIR(CP_BORDER));
    wattron(win, A_BOLD); mvwprintw(win, 1, 2, "%s", title.c_str()); wattroff(win, A_BOLD);
    mvwprintw(win, 2, 2, "%s", prompt.substr(0, w-4).c_str());
    mvwprintw(win, 5, 2, "[Enter] confirm   [Esc] cancel");
    keypad(win, TRUE);
    echo(); curs_set(1);
    char buf[512] = {};
    if (!prefill.empty()) {
        mvwprintw(win, 3, 2, "%s", prefill.substr(0, w-4).c_str());
        strncpy(buf, prefill.c_str(), sizeof(buf)-1);
    }
    wattron(win, COLOR_PAIR(CP_SEARCH));
    mvwgetnstr(win, 3, 2, buf, std::min((int)sizeof(buf)-1, w-4));
    wattroff(win, COLOR_PAIR(CP_SEARCH));
    curs_set(0); noecho();
    popupCleanup(win);
    return trimStr(std::string(buf));
}

/* Scrollable pager popup (for apt update output) */
static void pagerDialog(const std::string& title, const std::vector<std::string>& lines) {
    int w = std::min(COLS - 2, 100), h = LINES - 4;
    WINDOW* win = newwin(h, w, (LINES-h)/2, (COLS-w)/2);
    keypad(win, TRUE);

    int scroll = 0;
    int contentH = h - 4;

    while (true) {
        werase(win);
        wattron(win, COLOR_PAIR(CP_BORDER)); box(win, 0, 0); wattroff(win, COLOR_PAIR(CP_BORDER));
        wattron(win, A_BOLD); mvwprintw(win, 0, 2, " %s ", title.c_str()); wattroff(win, A_BOLD);
        mvwprintw(win, h-1, 2, " [↑/↓/PgUp/PgDn] Scroll   [q/Esc] Close ");

        for (int i = 0; i < contentH; i++) {
            int li = i + scroll;
            if (li >= (int)lines.size()) break;
            const auto& l = lines[li];
            // Color code apt output
            int pair = CP_DETAIL_VAL;
            if (l.rfind("Err:", 0) == 0 || l.rfind("E:", 0) == 0) pair = CP_PAGER_ERR;
            else if (l.rfind("Hit:", 0) == 0)                       pair = CP_PAGER_HIT;
            else if (l.rfind("Get:", 0) == 0)                       pair = CP_PAGER_GET;
            else if (l.rfind("W:", 0) == 0)                         pair = CP_STATUS_ERR;
            wattron(win, COLOR_PAIR(pair));
            mvwprintw(win, i + 2, 1, "%.*s", w - 3, l.c_str());
            wattroff(win, COLOR_PAIR(pair));
        }
        // Scroll bar
        if ((int)lines.size() > contentH) {
            int barH   = std::max(1, contentH * contentH / (int)lines.size());
            int barTop = contentH * scroll / (int)lines.size();
            for (int y = 0; y < contentH; y++)
                mvwaddch(win, y + 2, w - 1,
                         (y >= barTop && y < barTop + barH) ? ACS_BLOCK : ACS_VLINE);
        }
        wnoutrefresh(win); doupdate();

        int ch = wgetch(win);
        if (ch == 'q' || ch == 27 || ch == KEY_F(10)) break;
        else if (ch == KEY_UP)    scroll = std::max(0, scroll - 1);
        else if (ch == KEY_DOWN)  scroll = std::min((int)lines.size() - 1, scroll + 1);
        else if (ch == KEY_NPAGE) scroll = std::min((int)lines.size() - 1, scroll + contentH);
        else if (ch == KEY_PPAGE) scroll = std::max(0, scroll - contentH);
        else if (ch == KEY_HOME)  scroll = 0;
        else if (ch == KEY_END)   scroll = std::max(0, (int)lines.size() - contentH);
    }
    popupCleanup(win);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SECTION 18 — APT UPDATE (captures output)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void runAptUpdate() {
    if (!confirmDialog("Run 'sudo apt update' and show output?")) return;

    def_prog_mode(); endwin();

    // Run apt update, capture output to a temp file
    std::string tmpFile = "/tmp/relix_update.log";
    int ret = std::system(("sudo apt update 2>&1 | tee " + tmpFile).c_str());
    printf("\nPress Enter to view output in pager...");
    fflush(stdout); getchar();
    reset_prog_mode(); refresh();

    // Read captured output
    std::ifstream f(tmpFile);
    std::vector<std::string> output;
    std::string line;
    while (std::getline(f, line)) output.push_back(line);
    std::remove(tmpFile.c_str());

    if (!output.empty()) {
        std::string title = "apt update output  (exit code: " + std::to_string(ret) + ")";
        pagerDialog(title, output);
    }
    setStatus(ret == 0 ? "apt update completed successfully." : "apt update finished with errors.", ret != 0);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SECTION 19 — MOUSE SUPPORT
 * ═══════════════════════════════════════════════════════════════════════════ */

static void handleMouse() {
    MEVENT ev;
    if (getmouse(&ev) != OK) return;

    int listTop = 2;
    int lh      = listHeight();
    int lpw     = listPaneW();

    // Click in list pane
    if (ev.x < lpw && ev.y >= listTop && ev.y < listTop + lh) {
        int clicked = ev.y - listTop + g_scrollOff;
        if (clicked < (int)g_filtered.size()) {
            if (ev.bstate & BUTTON1_CLICKED) {
                g_selected = clicked;
                g_metaShown = false;
                g_asyncMeta.ready = false;
            } else if (ev.bstate & BUTTON1_DOUBLE_CLICKED) {
                // Double click = toggle
                g_selected = clicked;
                if (!g_readOnly && !g_filtered.empty()) {
                    int ri = currentRepoIndex();
                    if (ri >= 0) {
                        std::string err;
                        bool ok = g_repos[ri].isDeb822
                            ? toggleDeb822(g_repos[ri], err)
                            : toggleList  (g_repos[ri], err);
                        int prev = g_selected;
                        loadRepos();
                        g_selected = std::min(prev, (int)g_filtered.size()-1);
                        setStatus(ok ? "Toggled." : "Toggle FAILED: " + err, !ok);
                    }
                }
            }
        }
        // Scroll wheel
        if (ev.bstate & BUTTON4_PRESSED) g_selected = std::max(0, g_selected - 1);
        if (ev.bstate & BUTTON5_PRESSED) g_selected = std::min((int)g_filtered.size()-1, g_selected + 1);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SECTION 20 — SEARCH MODE
 * ═══════════════════════════════════════════════════════════════════════════ */

static void handleSearchInput(int ch) {
    if (ch == 27 || ch == '\n' || ch == KEY_F(10)) {
        // Exit search
        g_searchMode = false;
        if (ch == 27) { g_filterStr.clear(); rebuildFiltered(); }
        setStatus(g_filterStr.empty() ? "Search cleared." :
                  "Filter: '" + g_filterStr + "' — " + std::to_string(g_filtered.size()) + " result(s).");
        return;
    }
    if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
        if (!g_filterStr.empty()) { g_filterStr.pop_back(); rebuildFiltered(); g_selected = 0; }
    } else if (ch >= 32 && ch < 127) {
        g_filterStr += static_cast<char>(ch);
        rebuildFiltered();
        g_selected = 0;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SECTION 21 — MAIN
 * ═══════════════════════════════════════════════════════════════════════════ */

int main() {
    /* ── privilege check ── */
    g_isRoot   = (geteuid() == 0);
    g_readOnly = !g_isRoot;

    /* ── load config + OS info + repos ── */
    loadConfig();
    g_os = detectOS();
    loadRepos();

    /* ── ncurses init ── */
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    start_color();
    use_default_colors();

    // Enable mouse
    mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, nullptr);
    mouseinterval(150); // double-click interval ms

    // Apply saved theme
    applyTheme(g_cfg.themeIndex);

    // Set timeout so we can poll async meta (100 ms)
    timeout(100);

    if (g_readOnly)
        setStatus("Running without root — read-only mode. Use 'sudo' to edit repos.", true);
    else
        setStatus("Ready. " + std::to_string(g_repos.size()) + " repositories loaded.");

    /* ── event loop ── */
    while (true) {
        // Single redraw per frame. drawDetailPane() internally checks
        // g_asyncMeta.ready and picks up new metadata without a second call.
        redraw();
        int ch = getch();
        if (ch == ERR) continue; // 100 ms timeout — loop and redraw

        /* ── search mode ── */
        if (g_searchMode) { handleSearchInput(ch); continue; }

        /* ── mouse ── */
        if (ch == KEY_MOUSE) { handleMouse(); continue; }

        /* ── navigation ── */
        switch (ch) {
            case KEY_UP:
                if (g_selected > 0) { g_selected--; g_metaShown = false; g_asyncMeta.ready = false; }
                break;
            case KEY_DOWN:
                if (g_selected < (int)g_filtered.size()-1) { g_selected++; g_metaShown = false; g_asyncMeta.ready = false; }
                break;
            case KEY_NPAGE:
                g_selected = std::min(g_selected + listHeight(), (int)g_filtered.size()-1);
                g_metaShown = false;
                break;
            case KEY_PPAGE:
                g_selected = std::max(g_selected - listHeight(), 0);
                g_metaShown = false;
                break;
            case KEY_HOME: g_selected = 0;                               g_metaShown = false; break;
            case KEY_END:  g_selected = (int)g_filtered.size()-1;        g_metaShown = false; break;

            /* ── F2: Toggle ── */
            case KEY_F(2): {
                if (g_readOnly) { setStatus("Read-only mode — run as root to edit.", true); break; }
                if (g_filtered.empty()) break;
                int ri = currentRepoIndex();
                if (ri < 0) break;
                if (g_cfg.confirmToggle &&
                    !confirmDialog("Toggle: " + g_repos[ri].display.substr(0, 50) + " ?"))
                    break;
                std::string err;
                bool ok = g_repos[ri].isDeb822
                    ? toggleDeb822(g_repos[ri], err)
                    : toggleList  (g_repos[ri], err);
                int prev = g_selected;
                loadRepos();
                g_selected = std::min(prev, (int)g_filtered.size()-1);
                setStatus(ok ? "Repository toggled." : "Toggle FAILED: " + err, !ok);
                break;
            }

            /* ── F3: Add ── */
            case KEY_F(3): {
                if (g_readOnly) { setStatus("Read-only mode.", true); break; }
                std::string newLine = inputDialog("Add Repository",
                    "Enter new deb line (e.g.: deb http://ppa.../ubuntu focal main):");
                if (newLine.empty()) { setStatus("Add cancelled."); break; }
                if (newLine.rfind("deb", 0) != 0) {
                    setStatus("Invalid — must start with 'deb'.", true); break;
                }
                std::string dest = inputDialog("Add Repository",
                    "Target file (Enter = /etc/apt/sources.list):",
                    "/etc/apt/sources.list");
                if (dest.empty()) dest = "/etc/apt/sources.list";
                pushUndo(dest);
                std::string be;
                backupFile(dest, be);
                std::ofstream f(dest, std::ios::app);
                if (!f.is_open()) { setStatus("Cannot open " + dest, true); break; }
                f << newLine << "\n"; f.flush();
                loadRepos();
                g_selected = (int)g_filtered.size()-1;
                setStatus(f.good() ? "Repository added to " + dest : "Write error!", !f.good());
                break;
            }

            /* ── F4: Delete ── */
            case KEY_F(4): {
                if (g_readOnly) { setStatus("Read-only mode.", true); break; }
                if (g_filtered.empty()) break;
                int ri = currentRepoIndex();
                if (ri < 0) break;
                std::string prompt = "Delete: " + g_repos[ri].display.substr(0,55) + " ?";
                if (!confirmDialog(prompt)) { setStatus("Delete cancelled."); break; }
                std::string err;
                bool ok = deleteRepoClean(g_repos[ri], err);
                int prev = g_selected;
                loadRepos();
                g_selected = std::min(prev, std::max(0, (int)g_filtered.size()-1));
                setStatus(ok ? "Deleted." : "Delete FAILED: " + err, !ok);
                break;
            }

            /* ── F5: apt update ── */
            case KEY_F(5):
                runAptUpdate();
                break;

            /* ── F6: Reload ── */
            case KEY_F(6): {
                int prev = g_selected;
                loadRepos();
                g_selected = std::min(prev, std::max(0, (int)g_filtered.size()-1));
                g_metaShown = false;
                setStatus("Reloaded. " + std::to_string(g_repos.size()) + " repos.");
                break;
            }

            /* ── F7: Manual Backup ── */
            case KEY_F(7): {
                if (g_filtered.empty()) break;
                int ri = currentRepoIndex();
                if (ri < 0) break;
                std::string err;
                bool ok = backupFile(g_repos[ri].file, err);
                setStatus(ok ? "Backed up: " + g_repos[ri].file : "Backup FAILED: " + err, !ok);
                break;
            }

            /* ── F8: Export / Import ── */
            case KEY_F(8): {
                std::string action = inputDialog("Export / Import",
                    "Action: 'export /path/file.txt'  or  'import /path/file.txt'");
                if (action.empty()) break;
                auto words = splitWords(action);
                if (words.size() < 2) { setStatus("Usage: export <path> or import <path>", true); break; }
                std::string err;
                if (toLower(words[0]) == "export") {
                    bool ok = exportRepos(words[1], err);
                    setStatus(ok ? "Exported to " + words[1] : "Export FAILED: " + err, !ok);
                } else if (toLower(words[0]) == "import") {
                    bool ok = importRepos(words[1], err);
                    if (ok) loadRepos();
                    setStatus(err.empty() ? "Imported." : err, !ok);
                } else {
                    setStatus("Unknown action: " + words[0], true);
                }
                break;
            }

            /* ── m: Fetch metadata async ── */
            case 'm':
            case 'M': {
                if (g_filtered.empty()) break;
                int ri = currentRepoIndex();
                if (ri < 0) break;
                g_metaShown = false;
                g_asyncMeta.ready = false;
                fetchMetaAsync(g_repos[ri]);
                setStatus("Fetching metadata (3 s timeout)...");
                break;
            }

            /* ── t: Cycle theme ── */
            case 't':
            case 'T':
                g_cfg.themeIndex = (g_cfg.themeIndex + 1) % k_themeCount;
                applyTheme(g_cfg.themeIndex);
                saveConfig();
                setStatus(std::string("Theme: ") + k_themes[g_cfg.themeIndex].name);
                break;

            /* ── s: Cycle sort ── */
            case 's':
            case 'S':
                g_cfg.sortMode = (g_cfg.sortMode + 1) % 3;
                rebuildFiltered();
                saveConfig();
                { static const char* n[] = {"File","Status","Alphabetical"};
                  setStatus(std::string("Sort: ") + n[g_cfg.sortMode]); }
                break;

            /* ── /: Enter search mode ── */
            case '/':
                g_searchMode = true;
                g_filterStr.clear();
                rebuildFiltered();
                g_selected = 0;
                break;

            /* ── Ctrl+Z: Undo ── */
            case ('z' & 0x1f): {
                if (g_readOnly) { setStatus("Read-only mode.", true); break; }
                std::string err;
                bool ok = applyUndo(err);
                int prev = g_selected;
                loadRepos();
                g_selected = std::min(prev, std::max(0, (int)g_filtered.size()-1));
                setStatus(ok ? "Undo applied." : err, !ok);
                break;
            }

            /* ── q / F10: Quit ── */
            case 'q':
            case 'Q':
            case KEY_F(10):
                saveConfig();
                endwin();
                return 0;
        }
    }

    saveConfig();
    endwin();
    return 0;
}