cmake_minimum_required(VERSION 3.16)

project(relix
    VERSION     1.0.0
    DESCRIPTION "APT Repository Manager (TUI)"
    LANGUAGES   CXX
)

# ─── C++ standard ─────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD          17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)   # -std=c++17, not -std=gnu++17

# ─── Build type default ────────────────────────────────────────────────────────
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING
        "Build type: Debug | Release | RelWithDebInfo | MinSizeRel" FORCE)
    message(STATUS "No build type specified — defaulting to Release")
endif()

# ─── Compiler warnings ────────────────────────────────────────────────────────
add_library(relix_warnings INTERFACE)
target_compile_options(relix_warnings INTERFACE
    $<$<CXX_COMPILER_ID:GNU,Clang>:
        -Wall
        -Wextra
        -Wshadow
        -Wpedantic
        -Wconversion
        -Wnull-dereference
        -Wdouble-promotion
        -Wformat=2
        -Wno-unused-result       # popen/system return value intentionally ignored
    >
    $<$<AND:$<CXX_COMPILER_ID:GNU>,$<CONFIG:Debug>>:
        -Og                      # optimise for debugging
    >
)

# ─── Find dependencies ────────────────────────────────────────────────────────

# ncurses — prefer wide-character build (ncursesw) for full Unicode support
set(CURSES_NEED_NCURSES TRUE)
find_package(Curses REQUIRED)

if(NOT CURSES_FOUND)
    message(FATAL_ERROR
        "ncurses not found.\n"
        "Install with:  sudo apt install libncurses-dev   (Debian/Ubuntu)\n"
        "               sudo dnf install ncurses-devel     (Fedora/RHEL)\n"
        "               sudo pacman -S ncurses             (Arch)\n"
    )
endif()

# Prefer ncursesw (wide-char) library if available
find_library(NCURSESW_LIB NAMES ncursesw)
if(NCURSESW_LIB)
    set(NCURSES_LINK_LIB ${NCURSESW_LIB})
    message(STATUS "Using wide-char ncursesw: ${NCURSESW_LIB}")
else()
    set(NCURSES_LINK_LIB ${CURSES_LIBRARIES})
    message(STATUS "Using ncurses: ${CURSES_LIBRARIES}")
endif()

# POSIX threads
find_package(Threads REQUIRED)

# std::filesystem (GCC < 9 needs -lstdc++fs; GCC >= 9 links it automatically)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS "9.0")
        set(FILESYSTEM_LIB "stdc++fs")
        message(STATUS "GCC < 9 detected — linking stdc++fs explicitly")
    endif()
endif()

# ─── Executable ───────────────────────────────────────────────────────────────
add_executable(relix main.cpp)

target_include_directories(relix
    PRIVATE
        ${CURSES_INCLUDE_DIRS}
)

target_link_libraries(relix
    PRIVATE
        relix_warnings
        ${NCURSES_LINK_LIB}
        Threads::Threads
        $<$<BOOL:${FILESYSTEM_LIB}>:${FILESYSTEM_LIB}>
)

# ─── Compile definitions ──────────────────────────────────────────────────────
target_compile_definitions(relix
    PRIVATE
        relix_VERSION="${PROJECT_VERSION}"
        $<$<CONFIG:Debug>:relix_DEBUG>
)

# ─── Optimisation / sanitisers for Debug builds ───────────────────────────────
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(relix PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
        >
    )
    target_link_options(relix PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:
            -fsanitize=address,undefined
        >
    )
    message(STATUS "Debug build: AddressSanitizer + UBSan enabled")
endif()

# ─── Release hardening ────────────────────────────────────────────────────────
if(CMAKE_BUILD_TYPE STREQUAL "Release" OR
   CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    target_compile_options(relix PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:
            -D_FORTIFY_SOURCE=2
            -fstack-protector-strong
            -fPIE
        >
    )
    target_link_options(relix PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:
            -pie
            -Wl,-z,relro
            -Wl,-z,now
        >
    )
endif()

# ─── Install rules ────────────────────────────────────────────────────────────
include(GNUInstallDirs)

install(TARGETS relix
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}   # /usr/local/bin
)

install(DIRECTORY DESTINATION
    ${CMAKE_INSTALL_LOCALSTATEDIR}/backups/relix  # /var/backups/relix
)

# ─── Uninstall target ─────────────────────────────────────────────────────────
if(NOT TARGET uninstall)
    # Write the uninstall script directly into the build tree — no external
    # template file required.
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake" [=[
if(NOT EXISTS "@CMAKE_CURRENT_BINARY_DIR@/install_manifest.txt")
    message(WARNING "Cannot find install manifest — was 'cmake --install' run?")
    return()
endif()
file(READ "@CMAKE_CURRENT_BINARY_DIR@/install_manifest.txt" files)
string(REGEX REPLACE "\n" ";" files "${files}")
foreach(file ${files})
    message(STATUS "Removing: ${file}")
    if(EXISTS "${file}")
        file(REMOVE "${file}")
    else()
        message(WARNING "File not found: ${file}")
    endif()
endforeach()
]=])
    # Substitute the build-dir path into the script
    configure_file(
        "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
        @ONLY
    )
    add_custom_target(uninstall
        COMMAND ${CMAKE_COMMAND} -P
                "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
        COMMENT "Uninstalling relix..."
    )
endif()

# ─── Summary ──────────────────────────────────────────────────────────────────
message(STATUS "")
message(STATUS "══════════════════════════════════════════")
message(STATUS "  relix ${PROJECT_VERSION} — configuration summary")
message(STATUS "──────────────────────────────────────────")
message(STATUS "  Build type   : ${CMAKE_BUILD_TYPE}")
message(STATUS "  Compiler     : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  C++ standard : ${CMAKE_CXX_STANDARD}")
message(STATUS "  Install to   : ${CMAKE_INSTALL_PREFIX}")
message(STATUS "  ncurses lib  : ${NCURSES_LINK_LIB}")
message(STATUS "  Threads      : ${CMAKE_THREAD_LIBS_INIT}")
message(STATUS "══════════════════════════════════════════")
message(STATUS "")
message(STATUS "  Build:    cmake --build build/")
message(STATUS "  Install:  sudo cmake --install build/")
message(STATUS "  Run:      sudo ./build/relix")
message(STATUS "")