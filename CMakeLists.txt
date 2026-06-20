# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.21)
project(SQLTerminalWin32 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

if(MSVC)
  # C++-only warning/conformance flags (kept off the C amalgamation); /utf-8 for both.
  add_compile_options(
    $<$<COMPILE_LANGUAGE:CXX>:/W4>
    $<$<COMPILE_LANGUAGE:CXX>:/permissive->
    $<$<COMPILE_LANGUAGE:CXX>:/EHsc>
    /utf-8)
  add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# ---------------------------------------------------------------------------
# SqlCore — pure, dependency-free logic (byte-for-byte port of Swift SQLCore).
# ---------------------------------------------------------------------------
add_library(SqlCore STATIC
  src/core/SqlStatementSplitter.cpp
  src/core/SqlStatementClassifier.cpp
  src/core/SqlLiteralScanner.cpp
  src/core/SqlSyntaxHighlighter.cpp
  src/core/PostgresHba.cpp)
target_include_directories(SqlCore PUBLIC src)

# ---------------------------------------------------------------------------
# sqlite3 — vendored public-domain amalgamation, compiled into the app.
# ---------------------------------------------------------------------------
add_library(sqlite3 STATIC third_party/sqlite/sqlite3.c)
target_include_directories(sqlite3 PUBLIC third_party/sqlite)
target_compile_definitions(sqlite3 PRIVATE _CRT_SECURE_NO_WARNINGS SQLITE_OMIT_LOAD_EXTENSION)
if(MSVC)
  target_compile_options(sqlite3 PRIVATE /w)  # silence third-party warnings
endif()

# ---------------------------------------------------------------------------
# SqlDb — engine providers (SqliteProvider now; PostgresProvider in P4).
# ---------------------------------------------------------------------------
add_library(SqlDb STATIC
  src/db/SqliteProvider.cpp
  src/db/DatabaseSession.cpp)
target_include_directories(SqlDb PUBLIC src)
target_link_libraries(SqlDb PUBLIC sqlite3)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()

add_executable(SqlCoreTests tests/sqlcore_tests.cpp)
target_link_libraries(SqlCoreTests PRIVATE SqlCore)
target_include_directories(SqlCoreTests PRIVATE tests)
add_test(NAME SqlCoreTests COMMAND SqlCoreTests)

add_executable(SqlDbTests tests/sqlite_provider_tests.cpp)
target_link_libraries(SqlDbTests PRIVATE SqlDb)
target_include_directories(SqlDbTests PRIVATE tests)
add_test(NAME SqlDbTests COMMAND SqlDbTests)

add_executable(SqlSessionTests tests/database_session_tests.cpp)
target_link_libraries(SqlSessionTests PRIVATE SqlDb)
target_include_directories(SqlSessionTests PRIVATE tests)
add_test(NAME SqlSessionTests COMMAND SqlSessionTests)

# ---------------------------------------------------------------------------
# SQLTerminal — the Win32 GUI app.
# ---------------------------------------------------------------------------
add_executable(SQLTerminal WIN32
  src/WinMain.cpp
  src/ui/MainWindow.cpp)
target_link_libraries(SQLTerminal PRIVATE SqlDb SqlCore comctl32 comdlg32)

# Future phases (P4) will add find_package(PostgreSQL)/libpq + nlohmann_json via
# the vcpkg toolchain (declared in vcpkg.json).
