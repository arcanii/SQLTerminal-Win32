# SQLTerminal-Win32

A native **Windows (Win32 / C++17)** port of [SQLTerminal](../SQLTerminal) — a minimal SQL
terminal for **SQLite** and **PostgreSQL**. Write SQL, run it (Ctrl+E), and see results in a
sortable grid, with a schema sidebar, saved connections, and more.

A modern dark UI (Claude-desktop-style, coral accent) with a custom title bar and a
**GPU-rendered editor and results grid** (Direct2D + DirectWrite). No .NET runtime, no
Electron — just a small native `.exe` plus a few bundled DLLs.

## Features

- **Engines:** SQLite (vendored amalgamation) and PostgreSQL (libpq), with SSL modes
  (off / prefer / require) and rich connection-error messages (including the exact
  `pg_hba.conf` line to add for "no entry" rejections).
- **Editor:** a custom **Direct2D/DirectWrite** SQL editor with live **syntax highlighting**,
  word-wrap, and undo/redo; run the whole editor (**Ctrl+E**) or the statement under the
  cursor (**Ctrl+Enter**); **Ctrl+↑/↓** command history.
- **Responsive:** queries run off the UI thread; **Ctrl+.** cancels a running query
  (the connection survives).
- **Results grid:** a custom **Direct2D/DirectWrite** grid — virtualized (handles large
  result sets), click-to-sort columns (numeric-aware, NULL-last), resizable columns,
  alternating rows, per-cell copy, **Copy as TSV/CSV**, and a cell-detail view
  (double-click or right-click) with **JSON pretty-printing**.
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
- **Modern dark UI:** Claude-desktop-style theme (coral accent), a custom command/title
  bar, themed dialogs, per-monitor-v2 **DPI** scaling, and an in-app light/dark toggle.
- **History & Snippets** panel (Ctrl+R), **multi-window** (Ctrl+N), and **auto-update**
  via WinSparkle (a GitHub-hosted, EdDSA-signed appcast — see [`docs/RELEASING.md`](docs/RELEASING.md)).

## Building

**Requirements:** Visual Studio 2022/2026 with the **Desktop C++** workload (MSVC, plus the
bundled CMake + Ninja). All third-party code is vendored under `third_party/` — no vcpkg.

```cmd
scripts\build-and-test.cmd
```

This configures (CMake + Ninja), builds everything, and runs the test suites via `ctest`.
The app is `build\SQLTerminal.exe` (libpq / OpenSSL / WinSparkle DLLs are copied next to it).

### Tests

Eight suites (~280 checks), all pure-logic except the live PostgreSQL test:

- `SqlCoreTests` — the byte-for-byte port of the SQL splitter/classifier/scanner/pg_hba.
- `EditorCoreTests` — the pure editor model (caret/selection, edits, navigation, word
  boundaries, surrogate pairs, undo/redo).
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
src/editor/      EditorCore — pure text model behind the Direct2D editor (unit-tested)
src/ui/                    — Win32 windows + dialogs; Direct2D/DirectWrite editor & grid
third_party/               — vendored sqlite3, libpq (prebuilt), nlohmann/json, WinSparkle
```

## Notes / backlog

- **Code signing:** the exe + installer are not Authenticode-signed yet, so SmartScreen may
  warn on first download/run. (Auto-update integrity is separately EdDSA-signed.)
- **Auto-update:** wired to a GitHub-hosted, EdDSA-signed appcast (shared key with the macOS
  app); publishing a release is scripted — see [`docs/RELEASING.md`](docs/RELEASING.md).
- **Engines:** a **MySQL/MariaDB** provider is the main candidate feature (the stack is
  engine-agnostic behind `DatabaseProvider`).

## License

GPL-3.0-or-later, like the upstream project. See [LICENSE](LICENSE).
