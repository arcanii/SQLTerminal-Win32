# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.21)
project(SQLTerminalWin32 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /EHsc)
  add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# ---------------------------------------------------------------------------
# SqlCore — the pure, dependency-free logic ported byte-for-byte from the
# macOS `SQLCore` Swift package. No external dependencies (no vcpkg needed).
# ---------------------------------------------------------------------------
add_library(SqlCore STATIC
  src/core/SqlStatementSplitter.cpp
  src/core/SqlStatementClassifier.cpp
  src/core/SqlLiteralScanner.cpp
  src/core/PostgresHba.cpp
)
target_include_directories(SqlCore PUBLIC src)

# ---------------------------------------------------------------------------
# Golden tests — replay the Swift SQLCoreTests assertion tables. Zero deps;
# uses the tiny in-tree harness in tests/test_harness.h.
# ---------------------------------------------------------------------------
enable_testing()
add_executable(SqlCoreTests tests/sqlcore_tests.cpp)
target_link_libraries(SqlCoreTests PRIVATE SqlCore)
target_include_directories(SqlCoreTests PRIVATE tests)
add_test(NAME SqlCoreTests COMMAND SqlCoreTests)

# ---------------------------------------------------------------------------
# SQLTerminal — the Win32 GUI app. P0 is an empty window; later phases add the
# providers (sqlite3.c, libpq via vcpkg), UI controls, etc.
# ---------------------------------------------------------------------------
add_executable(SQLTerminal WIN32 src/WinMain.cpp)
target_link_libraries(SQLTerminal PRIVATE SqlCore comctl32)

# Future phases (P1/P4) will add, gated so P0 keeps building with bare MSVC:
#   * third_party/sqlite/sqlite3.c compiled into a SQLite provider
#   * find_package(PostgreSQL) / libpq + nlohmann_json via the vcpkg toolchain
#     (declared in vcpkg.json). Configure with
#     -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
