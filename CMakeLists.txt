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
# libpq (PostgreSQL client) — vendored prebuilt binaries (EDB), like sqlite3.
# No vcpkg/build-from-source. Exposed as the PostgreSQL::PostgreSQL imported
# target; PG_RUNTIME_DLLS are copied next to each exe that links it.
# ---------------------------------------------------------------------------
set(PG_DIR "${CMAKE_SOURCE_DIR}/third_party/postgresql")
add_library(PostgreSQL::PostgreSQL SHARED IMPORTED)
set_target_properties(PostgreSQL::PostgreSQL PROPERTIES
  IMPORTED_IMPLIB "${PG_DIR}/lib/libpq.lib"
  IMPORTED_LOCATION "${PG_DIR}/bin/libpq.dll"
  INTERFACE_INCLUDE_DIRECTORIES "${PG_DIR}/include")
set(PG_RUNTIME_DLLS
  "${PG_DIR}/bin/libpq.dll"
  "${PG_DIR}/bin/libcrypto-3-x64.dll"
  "${PG_DIR}/bin/libssl-3-x64.dll"
  "${PG_DIR}/bin/libintl-9.dll"
  "${PG_DIR}/bin/libiconv-2.dll"
  "${PG_DIR}/bin/libwinpthread-1.dll")

# Copy the libpq runtime DLLs next to an executable target so it runs in place.
function(sqlt_copy_pg_dlls target)
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PG_RUNTIME_DLLS} "$<TARGET_FILE_DIR:${target}>"
    COMMAND_EXPAND_LISTS)
endfunction()

# ---------------------------------------------------------------------------
# SqlDb — engine providers (SqliteProvider, PostgresProvider) + DatabaseSession.
# ---------------------------------------------------------------------------
add_library(SqlDb STATIC
  src/db/SqliteProvider.cpp
  src/db/PostgresProvider.cpp
  src/db/DatabaseSession.cpp)
target_include_directories(SqlDb PUBLIC src)
target_link_libraries(SqlDb PUBLIC sqlite3 SqlCore PostgreSQL::PostgreSQL)

# ---------------------------------------------------------------------------
# SqlApp — pure terminal-controller logic (dot-commands, guards, transactions,
# command history, smart-quotes, schema SQL).
# ---------------------------------------------------------------------------
add_library(SqlApp STATIC
  src/app/DotCommandHandler.cpp
  src/app/TerminalLogic.cpp)
target_include_directories(SqlApp PUBLIC src)
target_link_libraries(SqlApp PUBLIC SqlCore)

# ---------------------------------------------------------------------------
# SqlStore — JSON persistence (%APPDATA%) + Windows Credential Manager.
# ---------------------------------------------------------------------------
add_library(SqlStore STATIC
  src/persistence/Stores.cpp
  src/security/CredentialStore.cpp)
target_include_directories(SqlStore PUBLIC src)
target_include_directories(SqlStore PRIVATE third_party/json)
target_link_libraries(SqlStore PUBLIC advapi32 shell32 ole32)

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

# Pure conninfo builder test — no libpq, no server.
add_executable(PgConnInfoTests tests/postgres_conninfo_tests.cpp)
target_include_directories(PgConnInfoTests PRIVATE src tests)
add_test(NAME PgConnInfoTests COMMAND PgConnInfoTests)

# Live PostgreSQL integration test — self-skips unless SQLT_PG_HOST is set.
add_executable(PostgresLiveTest tests/postgres_live_test.cpp)
target_link_libraries(PostgresLiveTest PRIVATE SqlDb)
target_include_directories(PostgresLiveTest PRIVATE tests)
add_test(NAME PostgresLiveTest COMMAND PostgresLiveTest)

# Test exes that link SqlDb (-> libpq) need the libpq DLLs alongside them.
sqlt_copy_pg_dlls(SqlDbTests)
sqlt_copy_pg_dlls(SqlSessionTests)
sqlt_copy_pg_dlls(PostgresLiveTest)

add_executable(SqlStoreTests tests/store_tests.cpp)
target_link_libraries(SqlStoreTests PRIVATE SqlStore)
target_include_directories(SqlStoreTests PRIVATE tests)
add_test(NAME SqlStoreTests COMMAND SqlStoreTests)

add_executable(AppTests tests/app_tests.cpp)
target_link_libraries(AppTests PRIVATE SqlApp)
target_include_directories(AppTests PRIVATE tests)
add_test(NAME AppTests COMMAND AppTests)

# ---------------------------------------------------------------------------
# SQLTerminal — the Win32 GUI app.
# ---------------------------------------------------------------------------
add_executable(SQLTerminal WIN32
  src/WinMain.cpp
  src/ui/MainWindow.cpp
  src/ui/ConnectionDialog.cpp)
target_link_libraries(SQLTerminal PRIVATE SqlDb SqlCore SqlStore comctl32 comdlg32)
sqlt_copy_pg_dlls(SQLTerminal)

# Future phases (P4) will add find_package(PostgreSQL)/libpq + nlohmann_json via
# the vcpkg toolchain (declared in vcpkg.json).
