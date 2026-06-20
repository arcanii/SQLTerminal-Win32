# SQLTerminal-Win32

A native **Windows (Win32 / C++17)** port of [SQLTerminal](../SQLTerminal) — a minimal SQL
terminal for **SQLite** and **PostgreSQL**. Write SQL, run it (Ctrl+E), and see results in a
sortable grid, with a schema sidebar, saved connections, and more.

No .NET runtime, no Electron — just a small native `.exe` plus a few bundled DLLs.

## Features

- **Engines:** SQLite (vendored amalgamation) and PostgreSQL (libpq), with SSL modes
  (off / prefer / require) and rich connection-error messages (including the exact
  `pg_hba.conf` line to add for "no entry" rejections).
- **Editor:** RichEdit with SQL **syntax highlighting**; run the whole editor (**Ctrl+E**)
  or the statement under the cursor (**Ctrl+Enter**); **Ctrl+↑/↓** command history.
- **Responsive:** queries run off the UI thread; **Ctrl+.** cancels a running query
  (the connection survives).
- **Results grid:** virtual (handles large result sets), click-to-sort columns
  (numeric-aware, NULL-last), alternating rows, per-cell copy, **Copy as TSV/CSV**, and a
  cell-detail view with **JSON pretty-printing**.
- **Connections:** a connect dialog with recent + saved profiles; passwords stored in the
  **Windows Credential Manager**; profiles/recents/history/snippets persisted as JSON under
  `%APPDATA%\SQLTerminal`.
- **Safety:** per-window **read-only** mode and **destructive-statement** confirmation
  (DROP / TRUNCATE / unqualified DELETE-UPDATE).
- **Transactions:** Begin / Commit / Rollback with an in-transaction status indicator.
- **Schema sidebar:** tables → columns (lazy-loaded); double-click to insert a SELECT;
  right-click to preview/insert/copy.
- **Dot-commands:** `.tables`, `.schema`, `.columns`, `.count`, `.first`/`.last`, `.fk`,
  `.dbinfo`, `.connect`/`.use`, `.help`, … (type `.help` in the editor).
- **History & Snippets** panel (Ctrl+R), **multi-window** (Ctrl+N), and **auto-update**
  via WinSparkle.

## Building

**Requirements:** Visual Studio 2022/2026 with the **Desktop C++** workload (MSVC, plus the
bundled CMake + Ninja). All third-party code is vendored under `third_party/` — no vcpkg.

```cmd
scripts\build-and-test.cmd
```

This configures (CMake + Ninja), builds everything, and runs the test suites via `ctest`.
The app is `build\SQLTerminal.exe` (libpq / OpenSSL / WinSparkle DLLs are copied next to it).

### Tests

Seven suites (~200 checks), all pure-logic except the live PostgreSQL test:

- `SqlCoreTests` — the byte-for-byte port of the SQL splitter/classifier/scanner/pg_hba.
- `SqlDbTests`, `SqlSessionTests` — SQLite provider + the worker-thread/cancel model.
- `PgConnInfoTests`, `PostgresLiveTest` — libpq conninfo + a live integration test
  (set `SQLT_PG_HOST`/`PORT`/`DB`/`USER`/`PASS` to run it; otherwise it self-skips).
- `SqlStoreTests` — JSON stores + Credential Manager round-trip.
- `AppTests` — dot-commands, guards, transactions, history nav, result formatting.

### Installer

After a build, with [Inno Setup](https://jrsoftware.org/isinfo.php) installed:

```cmd
scripts\build-installer.cmd
```

produces `build\installer\SQLTerminal-<ver>-setup.exe`.

## Project layout

```
src/core/        SqlCore   — pure SQL logic (splitter, classifier, scanner, pg_hba, highlighter)
src/models/                — data types (connection, query result, profile, history)
src/db/          SqlDb     — providers (SQLite, Postgres/libpq) + off-thread DatabaseSession
src/persistence/ \ SqlStore — JSON stores (%APPDATA%) + Windows Credential Manager
src/security/    /
src/app/         SqlApp    — dot-commands, guards, transactions, history, result formatting
src/ui/                    — Win32 windows + dialogs
third_party/               — vendored sqlite3, libpq (prebuilt), nlohmann/json, WinSparkle
```

## Notes / known gaps

- **DPI:** the app is *system* DPI-aware; per-monitor scaling of the fixed-pixel layout is
  future work.
- **Dark mode:** title bars follow the system theme; full dark theming of the controls is
  future work.
- **Auto-update:** wired to a Windows appcast; shipping real updates needs a published,
  EdDSA-signed appcast (see `packaging/appcast.xml`).

## License

GPL-3.0-or-later, like the upstream project. See [LICENSE](LICENSE).
