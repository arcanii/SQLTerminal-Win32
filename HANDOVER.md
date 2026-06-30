# SQLTerminal-Win32 — Handover

Native **Windows (Win32 / C++17)** port of the macOS SwiftUI app **SQLTerminal**
(SQLite + PostgreSQL query terminal). This document is the single starting point
for anyone (human or agent) continuing the work.

## Current state — 0.1.1 shipped + auto-updating (2026-07-01, build 55)

On top of the feature-complete port, the GUI is **fully modernized** (Claude-
desktop-style dark theme, coral accent) **and the heavy surfaces are now GPU-
rendered**: the SQL editor and the results grid are custom **Direct2D + DirectWrite**
controls (replacing RichEdit and the virtual ListView), the remaining system
MessageBoxes are themed, and the panes have rounded inset frames. Auto-update has a
complete GitHub release workflow. Everything was committed **directly to `main`**
(owner's instruction — no feature branches); the owner pushes manually (currently
in sync with `origin/main`).

Every change built clean `/W4`, kept the **8 ctest suites green** (~280 checks),
and was launch-verified. Caveat: most of it was built **without screen access** —
the owner reviews visually and reports issues, which get fixed (the GPU grid, panes,
and the 0.1.1 features are all owner-reviewed and tuned from feedback). **Dev machine
runs 150% scaling (144 dpi).** After the final commit
of a change, **rebuild** so `build\SQLTerminal.exe`'s version stamp matches HEAD
(it's captured at CMake configure time).

**0.1.1 is shipped and auto-updating.** Since build 46: a **full-text row filter** +
**unbounded column resize** in the results grid, **About**-box GitHub/Report-Issue
links + a dedication, splitter/pane-seam polish, the marketing bump to **`0.1.1`**,
and the **`0.1.1.54` release** (signed installer + EdDSA appcast on GitHub) — an old
build auto-updated to it live. The auto-update path is now proven in production. See
`docs/RELEASING.md` + the `winsparkle-update-validated` memory.

### What's new (newest first)
- **0.1.1 — row filter, wider columns, About links, splitter polish, shipped (builds 47-55).**
  **Full-text row filter** at the top of the results grid (live, case-insensitive, any
  column, "N of M" count, Esc clears; `ResultFormat::rowMatchesFilter` is unit-tested;
  the grid rebuilds `rowOrder` = filter over the current sort). **Column resize is now
  unbounded** (auto-size still caps at 440px; drag to any width). **About** box gained
  GitHub + Report-Issue links and a "for Daniel Kenny and Bryan Mark" dedication.
  **Splitter polish**: dropped the redundant center hairline + tightened the pane
  gutters to a single clean seam. **Bumped to `0.1.1`** (4 version files) and
  **released `0.1.1.54`**; an old build auto-updated to it live. Row filter
  adversarially reviewed (clean).
- **Polish + rounded panels + auto-update workflow (builds 43–46).** Editor Up/Down
  at the first/last line go to doc start/end; grid double-click opens cell detail and
  the header title stays put on sort. **Rounded inset pane frames** (parent-painted in
  the inset margins — flicker-safe, no `SetWindowRgn`). **Auto-update release workflow**:
  a GitHub-hosted EdDSA-signed appcast (shares the macOS app's key),
  `scripts/make-appcast.ps1` + `docs/RELEASING.md`; the app reports `APP_VERSION.BUILD`
  for update checks.
- **GPU results grid + themed dialogs + cleanup.** Replaced the virtual ListView
  with a Direct2D/DirectWrite `SqlGridControl` (sort/select/resizable columns/2-axis
  scroll/context-menu/theme/DPI; build 40); replaced the last three system
  MessageBoxes with a dark `themedMessageBox` (build 41); removed dead status-bar /
  font / init code (build 42). D2D factories are now shared via `src/ui/D2DSupport.h`.
- **GPU editor (Direct2D/DirectWrite).** Replaced RichEdit with a custom
  `ID2D1HwndRenderTarget` control — a pure, unit-tested `editor::EditorModel`
  (`EditorCore` lib, `EditorCoreTests`) + `src/ui/SqlEditorControl.cpp`. Kills the
  splitter resize-reflow lag (we own the DirectWrite layout, re-wrap only on width
  change); reuses `SqlSyntaxHighlighter`; word-wrap, full editing/undo/clipboard/
  IME, device-loss recovery, per-monitor DPI. No new deps (links d2d1/dwrite/imm32).
  Adversarially reviewed; **owner-confirmed visually at build 39.**
- Smoother splitter resize: `WS_CLIPCHILDREN`, tree double-buffer, a `DeferWindowPos`
  batch in `layout()`, and `RedrawWindow(RDW_UPDATENOW)` after a drag step so the
  panes don't trail the cursor. (Residual RichEdit reflow lag remains — see below.)
- Dark info dialog for Help (F1) / Connection Details / dot-command output
  (`showInfoDialog`, CRLF-normalized).
- Status bar: fully custom double-buffered paint (no white etch, no resize flash).
- Custom merged-caption **title bar** (`WM_NCCALCSIZE`): the command bar paints
  min/max/close, drags the window (`DragDetect` → `WM_NCLBUTTONDOWN HTCAPTION`),
  double-click-maximizes, has a top-edge resize zone. Vetted by a multi-agent
  adversarial review (Workflow).
- Dark **About** dialog: app icon + `0.1.0 (N)`, N = git commit count.
- In-app **light/dark toggle** (View menu; `Theme.h::themeOverride` + `reapplyTheme`).
- **Per-monitor-v2 DPI** (manifest + `dp()` scaling + `WM_DPICHANGED`).
- Dark + DPI **Connect / CellDetail / History** dialogs.
- Tier 1–3 dark theme: all controls themed, custom **command bar** replacing the
  menu (hamburger popup), dark popup menus (uxtheme ordinals), status pills, dark
  grid header. **App icon** = the shared macOS logo (`art/transparent_logo.png`).

### New UI architecture (all under src/ui)
- `Theme.h` — dark+light palette (coral `#D97757`); `currentTheme()` follows the
  system unless `themeOverride()` is set; `dp()/dpiScale()`; shared dialog helpers
  `themeBrush` / `dialogCtlColor` / `applyDialogDarkMode`.
- `MainWindow.cpp` — the command bar **is** the title bar (`CmdBarProc`); the
  status bar is a custom-painted subclass (`StatusSubclassProc`); custom About +
  generic info dialogs; `reapplyTheme` live-re-themes on toggle. The window is
  `WS_CLIPCHILDREN` and paints **rounded inset frames** around the panes in its own
  `WM_PAINT`; `layout()` insets the panes via `DeferWindowPos`.
- `SqlEditorControl.*` (+ pure `editor::EditorModel` in the `EditorCore` lib) — the
  GPU SQL editor: a child window (class `SqlD2DEditor`) drawn with Direct2D /
  DirectWrite via `ID2D1HwndRenderTarget`, replacing RichEdit. The model is
  unit-tested (`EditorCoreTests`); the view owns rendering + input + caret/selection
  geometry (DirectWrite hit-testing). `editorText()`/`caretOffset()` now live here.
- `SqlGridControl.*` — the GPU results grid (class `SqlD2DGrid`), replacing the
  virtual ListView: owns the `QueryResult` + sort (`ResultFormat`) + **full-text row
  filter** (`gridSetFilter`/`gridGetCounts`; rebuilds `rowOrder` = filter over sort)
  + selection + scroll + **freely resizable columns** (auto-size caps at 440px, manual
  resize unbounded) + the per-cell context menu (`gridSetResult` / `gridClear` /
  `gridApplyTheme` / `gridUpdateDpi`). The filter field + "N of M" count live in
  `MainWindow` (top of the results card; themed EDIT/STATIC via `WM_CTLCOLOR`).
- `ThemedDialog.*` — `themedMessageBox()`, a dark drop-in for `MessageBoxW`.
- `D2DSupport.h` — shared process-lifetime Direct2D/DirectWrite factories +
  `colorToD2D` + `SafeRelease`, used by both D2D controls.
- The 3 popup dialogs use the shared dark+DPI helpers.
- Version: CMake runs `git rev-list --count HEAD` → `build/generated/version.h`
  (from `src/version.h.in`).

### Shipped this modernization (with build/commit refs)
- **GPU editor — DONE (builds 37–38, commits `7bbaf54` + `670e39d`).** RichEdit is
  replaced by a custom **Direct2D + DirectWrite** editor rendered via
  **`ID2D1HwndRenderTarget`** — *not* DirectComposition: `DCompositionCreateTargetForHwnd`
  can't target a child window, and the lag fix comes from owning the DirectWrite
  layout, not the compositor (links only `d2d1`+`dwrite`+`imm32`). Fixes the splitter
  resize-reflow lag and a latent DPI font-scaling bug. **Owner-confirmed visually
  at build 39** (150% DPI). The **results grid is now also GPU** — a custom
  Direct2D/DirectWrite `SqlGridControl` (build 40, commit `a41e8a9`); the tree
  stays GDI, so all heavy surfaces are GPU-rendered now.
- **Themed message dialogs — DONE (build 41, commit `f71388f`).** The destructive
  Yes/No confirm, clear-history confirm, and connection-failed error now use a dark
  `themedMessageBox` (`src/ui/ThemedDialog.*`). Only the fatal can't-register-class
  startup box (no window yet) remains a system MessageBox.
- **Dead code removed — DONE (build 42, commit `002ac99`):** the status-bar
  `WM_DRAWITEM` handler + `AppState::flagsText`, the unused `AppState::hMono`
  (DirectWrite owns the code font now), and `ICC_LISTVIEW_CLASSES`.
- **Rounded inset pane frames — DONE (build 45).** Parent-painted rounded hairline
  frames in the panes' inset margins — the flicker-safe approach (no `SetWindowRgn`,
  so no fight with `WS_CLIPCHILDREN`). Tunable via `kPaneInset`/`kFrameInset`/`kPaneRadius`.

## Status

**All 10 porting phases (P0–P9) are complete and merged to `main`** (PR #1).
The app is feature-complete; 8 test suites (~280 checks) pass; clean `/W4` build.
The original phased plan is in [PORTING_PLAN.md](PORTING_PLAN.md); user-facing
docs are in [README.md](README.md).

**The GPU surfaces are now owner-reviewed** — the dark chrome, editor, Direct2D grid
(sort / filter / resize / select / scroll / context-menu), and rounded-card panes +
splitter have all been exercised and tuned from visual feedback. Pure logic stays
heavily unit-tested; both DB engines are verified end-to-end. **Biggest remaining
items: Authenticode signing and the MySQL engine** (see Backlog).

## Source of truth

- **Upstream (the spec):** the macOS Swift app at **`../SQLTerminal`** (sibling
  repo, `G:\SQLTerminal`). When porting/fixing behavior, read the corresponding
  Swift file — the C++ is meant to match it.
- **PORTING_PLAN.md** — the architecture mapping, risks, and phase breakdown.
- **Agent memory** (if you're a Claude session): `MEMORY.md` +
  `project-port-overview.md` + `port-fidelity-gotchas.md` in the project memory dir.

## Toolchain (important — non-obvious)

- **Visual Studio 2026 Community** at `C:\Program Files\Microsoft Visual Studio\18\Community`
  (MSVC 14.51), with the **Desktop C++** workload (bundled CMake + Ninja).
  `cmake`/`cl` are **not** on PATH by default.
- **No vcpkg.** Its meson-based `libpq` build **hangs** on this machine because
  the only `python` on PATH is the sandboxed Microsoft Store / Python Manager
  build (can't read vcpkg's download dir). A real python.org install exists at
  `%LocalAppData%\Programs\Python\Python313` if ever needed. **All third-party
  deps are vendored under `third_party/` instead** — do not reintroduce vcpkg.
- **Inno Setup 6.7.3** (installer) is installed per-user at
  `%LocalAppData%\Programs\Inno Setup 6` (via `winget install JRSoftware.InnoSetup`).
- **Docker** is available (used for the live Postgres test).

## Build, test, package

```cmd
scripts\build-and-test.cmd      :: configure (CMake+Ninja) + build + ctest
scripts\build-installer.cmd     :: Inno Setup installer (run after a build)
```

- App: `build\SQLTerminal.exe` (+ 7 runtime DLLs copied next to it).
- Installer: `build\installer\SQLTerminal-<ver>-setup.exe`.
- The build script sets up vcvars64 + the VS-bundled CMake/Ninja, configures with
  `-G Ninja`, builds, and runs `ctest`.

### Test suites (ctest)

| Suite | Covers |
|---|---|
| `SqlCoreTests` | byte-for-byte SQL splitter/classifier/scanner/pg_hba/highlighter (golden tables) |
| `EditorCoreTests` | pure editor model: newline-normalize, caret/offset math, edits, nav, word boundaries, surrogate pairs, selection, undo/redo |
| `SqlDbTests` | SqliteProvider against a temp DB |
| `SqlSessionTests` | worker-thread session + **cancel-mid-query** (connection survives) |
| `PgConnInfoTests` | libpq conninfo builder (pure) |
| `PostgresLiveTest` | live PG integration — **self-skips unless `SQLT_PG_*` env is set** |
| `SqlStoreTests` | JSON stores (caps/dedup/sort) + Credential Manager round-trip |
| `AppTests` | dot-commands, guards, transactions, history nav, result formatting |

To run the **live Postgres** test:
```cmd
docker run -d --rm --name sqlt-pg -e POSTGRES_PASSWORD=postgres -e POSTGRES_DB=demo -e POSTGRES_USER=postgres -p 5433:5432 postgres:16
set SQLT_PG_HOST=localhost & set SQLT_PG_PORT=5433 & set SQLT_PG_DB=demo & set SQLT_PG_USER=postgres & set SQLT_PG_PASS=postgres & set SQLT_PG_SSL=off
build\PostgresLiveTest.exe
docker stop sqlt-pg
```
A sample SQLite DB for manual testing: `samples\demo.db` (gitignored).

## Architecture (libraries, bottom-up)

```
SqlCore   src/core/      pure SQL logic (no platform deps): SqlStatementSplitter,
                         SqlStatementClassifier, SqlLiteralScanner, PostgresHba,
                         SqlSyntaxHighlighter. UTF-16 (wchar_t).
models    src/models/    plain structs: DatabaseConnection, QueryResult,
                         ConnectionProfile, QueryHistory(Entry/Snippet), DatabaseEngine.
SqlDb     src/db/        DatabaseProvider iface + SqliteProvider + PostgresProvider(libpq)
                         + DatabaseProviderFactory + DatabaseSession (worker thread).
SqlStore  src/persistence/ + src/security/  JSON stores (%APPDATA%) + CredentialStore.
SqlApp    src/app/       controller logic (pure): DotCommandHandler, TerminalLogic
                         (guards/transactions/history/smart-quotes/schema SQL), ResultFormat.
ui        src/ui/        Win32 windows + dialogs (MainWindow, ConnectionDialog,
                         CellDetailDialog, HistoryDialog). + src/platform/ (Encoding, Updater).
third_party/             sqlite3 (3.53.2), postgresql/libpq (EDB 17.6 prebuilt),
                         json (nlohmann 3.12), winsparkle (0.9.3 prebuilt).
```

UI uses `PostMessage` to marshal worker-thread results back: `WM_APP_RESULT`
(query), `WM_APP_CONNECTED`, `WM_APP_TABLES`, `WM_APP_COLUMNS` (schema).

## Invariants — do not break these

1. **`SqlCore` is byte-for-byte from the Swift originals.** If you change it, keep
   the golden tests (transcribed from `../SQLTerminal/Tests/SQLCoreTests`) in
   lock-step. They are the contract for run-statement-at-cursor, the read-only /
   destructive guards, and highlighting.
2. **UTF-16 (`wchar_t`) end-to-end** in the editor + SqlCore, so RichEdit offsets
   (`EM_EXSETSEL`) line up with scanner/splitter offsets without conversion.
   RichEdit text is read with single-`\r` line breaks then normalized to `\n`.
3. **`DatabaseSession` threading discipline** (see `src/db/DatabaseSession.*` and
   `port-fidelity-gotchas` memory): `connMutex_` guards **only the provider
   pointer**, held briefly — **never** across a blocking `execute`/`connect`.
   `cancel()` runs on the UI thread; for Postgres it uses a `PGcancel*` snapshot
   (`PQgetCancel`/`PQcancel`) so the connection survives; `disconnect` is
   serialized on the worker. Completion callbacks fire on the worker thread.
4. **Credential key** is `"<user>@<host>:<port>/<db>"`, Credential Manager
   TargetName `"SQLTerminal:" + key`. Persistence JSON lives under
   `%APPDATA%\SQLTerminal` (override with `SQLT_DATA_DIR`, used by tests).
5. **Vendored deps, no vcpkg.** Vendored import libs (`*.lib`) are force-committed
   via `.gitignore` negations.

## Backlog

The dark theme, the GPU editor + results grid (with the row filter), themed dialogs,
per-monitor-v2 DPI, rounded-card panes, and the **auto-update — now shipped & proven**
(`0.1.1.54` released and auto-updated live) are all done. Genuinely remaining:

- **Code signing (Authenticode)** of the exe + installer — **not set up**, so
  SmartScreen warns on first download/run (and during the auto-update install). Top
  item before wider distribution. Separate from the EdDSA update-integrity signature.
- **MySQL/MariaDB engine** (upstream backlog) — add a `MySqlProvider` implementing
  `DatabaseProvider` and a case in `DatabaseProviderFactory`; the rest of the stack is
  engine-agnostic. Vendor a client lib under `third_party/` (no vcpkg).
- Optional polish: IME inline composition (best-effort today); rounded-card
  radius/inset tuning.

## Gotchas seen during the port

- **Defender / AV** occasionally locks a freshly-linked `.exe` → transient
  `LNK1104: cannot open file ...exe`. Re-run the build; it clears.
- **`vcvars64.bat`** prints a harmless `vswhere.exe not recognized` line; the
  build script adds the VS Installer dir to PATH to avoid it.
- Git shows **LF→CRLF** normalization warnings on commit — harmless.
