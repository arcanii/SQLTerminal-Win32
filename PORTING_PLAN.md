# SQLTerminal → Win32 Port Plan

A plan to port **SQLTerminal** (a native macOS SwiftUI app for running SQL against SQLite/PostgreSQL) to a **native Windows desktop app** with full feature parity.

- **Source:** `../SQLTerminal` (Swift 6 / SwiftUI / AppKit, ~5,400 LOC across 34 files). Sibling repo on disk: `G:\SQLTerminal`.
- **Target:** this repo, `G:\SQLTerminal-Win32`.
- **License:** GPL-3.0 (carried over).

---

## 1. Locked decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| **Stack** | **C++17 + raw Win32 API + Common Controls v6**, CMake + MSVC (x64) | Matches the "Win32" name; no runtime dependency; one small self-contained `.exe`. |
| **Scope** | **Full feature parity** with the current macOS release (v1.1.4) | Reproduce everything: editor highlighting, history/snippets, sortable/JSON results, read-only + destructive guards, transactions, multi-window, auto-update, secure credentials. |

### Dependency choices (consequences of the above)

| Concern | Library | Note |
|---------|---------|------|
| SQLite | **Vendored `sqlite3.c` amalgamation** | Identical C API to macOS's system SQLite — near-zero-friction reuse. |
| PostgreSQL | **`libpq`** (official C client, via vcpkg; pulls OpenSSL) | Replaces the pure-Swift `PostgresClientKit`. libpq natively handles SCRAM/MD5/cleartext/trust **and** SSL, which *collapses* the Swift app's manual 4-credential probe loop and SSL fallback into a single connection string. **Net simplification.** |
| Secrets | **Windows Credential Manager** (`CredWriteW`/`CredReadW`/`CredDeleteW`, `CRED_TYPE_GENERIC`) | Replaces macOS Keychain. |
| Settings/profiles/history | **JSON files under `%APPDATA%\SQLTerminal\`** (header-only `nlohmann/json`) | Replaces `UserDefaults` + `Codable`. |
| Auto-update | **WinSparkle** | Replaces Sparkle; reuse the appcast-URL concept. |
| JSON / GUIDs / clipboard | `nlohmann/json`, `CoCreateGuid`, `CF_UNICODETEXT` | — |
| Tests | **GoogleTest** (or Catch2) | For the pure-logic golden suite. |

---

## 2. Architecture overview

The macOS app is clean MVVM. The port preserves that separation, splitting cleanly into **platform-independent logic** (a near-direct port) and a **rebuilt Win32 UI/platform layer**.

```
┌─────────────────────────────────────────────────────────────┐
│ WinMain · message loop · accelerators · WindowManager        │  ← all new (Win32)
├───────────────┬───────────────────────┬─────────────────────┤
│ UI controls   │ Dialogs               │ Menus / status bar   │  ← all new (Win32 controls)
│ (RichEdit     │ (Connection, History, │                      │
│  editor,      │  CellDetail, Help,    │                      │
│  ListView     │  About, ConnDetails)  │                      │
│  results,     └───────────────────────┴─────────────────────┤
│  TreeView     │ TerminalController · ConnectionController     │  ← rebuilt (logic ports 1:1)
│  schema)      │ SyntaxHighlighter · DotCommandHandler         │
├───────────────┴──────────────────────────────────────────────┤
│ DatabaseSession (worker thread + cancellation)                │  ← rebuilt primitives, same semantics
├──────────────────────────┬───────────────────────────────────┤
│ SqliteProvider (sqlite3) │ PostgresProvider (libpq)           │  ← SQLite ~direct; PG rebuilt on libpq
├──────────────────────────┴───────────────────────────────────┤
│ Persistence (JSON) · CredentialStore (Cred Mgr) · Models      │  ← logic ports 1:1, backend rebuilt
├───────────────────────────────────────────────────────────────┤
│ SqlCore  (splitter · classifier · scanner · pg_hba)           │  ← BYTE-FOR-BYTE port + golden tests
└───────────────────────────────────────────────────────────────┘
```

### Proposed repo layout

```
SQLTerminal-Win32/
├─ CMakeLists.txt              vcpkg.json
├─ third_party/sqlite/         sqlite3.c, sqlite3.h
├─ src/
│  ├─ core/        SqlStatementSplitter, SqlStatementClassifier,
│  │              SqlLiteralScanner, PostgresHba      (→ SqlCore.lib)
│  ├─ models/      DatabaseConnection, ConnectionProfile, DatabaseEngine,
│  │              SslMode, QueryHistoryEntry, QuerySnippet, QueryResult
│  ├─ persistence/ RecentConnectionsStore, SavedProfilesStore,
│  │              QueryHistoryStore, SnippetStore  (JSON under %APPDATA%)
│  ├─ security/    CredentialStore                 (Credential Manager)
│  ├─ db/          DatabaseProvider (iface), SqliteProvider,
│  │              PostgresProvider, DatabaseSession (worker thread)
│  ├─ app/         TerminalController, ConnectionController, DotCommandHandler
│  ├─ ui/          MainWindow, SqlEditor + SyntaxHighlighter, ResultsGrid,
│  │              ScrollbackLog, SchemaSidebar, Toolbar, StatusBar, dialogs/
│  ├─ platform/    WindowManager, accelerators, clipboard, file dialogs,
│  │              relative-time, version info, WinSparkle wiring
│  └─ WinMain.cpp
├─ tests/core/     golden tests (transcribed from Swift SQLCoreTests)
└─ packaging/      app.manifest, version.rc, appcast.xml, installer (Inno/WiX)
```

---

## 3. Source → Win32 mapping (high level)

| macOS component | Win32 replacement | Effort | Notes |
|---|---|:--:|---|
| `SQLCore` (4 pure algorithms) | `SqlCore.lib` (C++) | M | **Byte-for-byte**; pinned by golden tests. |
| Models + Codable/UserDefaults | Plain structs + JSON files | S→M | Logic 1:1; storage rebuilt. |
| `KeychainHelper` | `CredentialStore` (Cred Mgr) | S | Preserve exact account key. |
| `SQLiteProvider` (system SQLite3) | `SqliteProvider` (vendored `sqlite3.c`) | M | Same C API; drop sandbox dance. |
| `PostgresProvider` (PostgresClientKit) | `PostgresProvider` (libpq) | L | Auth/SSL collapse into conninfo; PQcancel. |
| `DatabaseSession` (DispatchQueue+NSLock+continuation) | `std::thread` + job queue + mutex + `PostMessage` | L | **Highest correctness risk.** |
| `TerminalViewModel`/`ConnectionViewModel` (`@Published`) | Controllers + dirty-flag/`InvalidateRect` | L/M | Decision logic 1:1; binding rebuilt. |
| SwiftUI `App`/`WindowGroup`/`.commands` | `WinMain` + `WindowManager` + per-window `HMENU` | L | Multi-window built by hand. |
| `NSTextView` + attribute runs (editor) | **RichEdit 4.1 (MSFTEDIT)** + `CHARFORMAT2` | M | Highlight via `EM_SETCHARFORMAT` over `EM_EXSETSEL`. |
| SwiftUI results grid | **`SysListView32`** report mode, **`LVS_OWNERDATA`** | L | Virtual mode mandatory for large sets. |
| SwiftUI schema list | **`SysTreeView32`** (lazy `TVN_ITEMEXPANDING`) | M | — |
| `NSOpenPanel`/`NSSavePanel` | `IFileOpenDialog`/`IFileSaveDialog` | S | Drop security-scoped bookmarks. |
| `NSEvent` key monitor + `.keyboardShortcut` | Accelerator table (`ACCEL[]` + `TranslateAccelerator`) | S | Cmd→Ctrl. |
| Sparkle | WinSparkle | S | Ed25519 supported by modern WinSparkle. |
| `NSColor` semantic colors | `GetSysColor` + fixed syntax RGB | S | Honor dark mode optionally. |

---

## 4. Concurrency model (the load-bearing rebuild)

This is the part most likely to break if rebuilt naively. Mirror the Swift discipline **exactly**:

- **One worker `std::thread` per window**, each owning **one** connection (SQLite handle or `PGconn`) and a single-item job queue (`std::mutex` + `std::condition_variable`).
- A **connection-pointer mutex** is held **only briefly** to read/swap the handle — **never** across a blocking `PQexec`/`sqlite3_step`/connect. Holding it across blocking I/O = deadlock.
- **`cancel()` runs on the UI thread.** It takes the mutex, reads the live cancel handle, fires the interrupt, releases:
  - SQLite: `sqlite3_interrupt(db)` — connection **survives**.
  - Postgres: snapshot a `PGcancel*` via `PQgetCancel()` right after connect (store under the mutex); cancel calls `PQcancel()` on **that handle only** (documented thread-safe; does not touch `PGconn`). Then the worker drains the in-flight result.
- **Result marshalling:** worker builds a heap `QueryResult*` and `PostMessage(hwnd, WM_APP_RESULT, ...)`; the UI thread takes ownership and frees it.
- **`disconnect()` serializes through the worker queue** so it can't free a handle that `cancel`/`execute` is touching.
- Preserve the **synchronous busy-flag-before-dispatch** (prevents double execution) and the **cancel double-check** (before *and* after execute).
- Keep the **cancellable vs. `executeUncancellable`** split (schema fetches use the uncancellable path).

> **Cancellation decision:** adopt `PQcancel` (connection survives) and therefore **drop** the macOS `recoverAfterCancel` auto-reconnect for Postgres. (PostgresClientKit had no CancelRequest, so the Swift app force-closed the socket and reconnected; libpq does it properly.) Keep SQLite's no-recovery path. Soak-test cancel-mid-large-query on both engines.

---

## 5. The four pure algorithms (the fidelity contract)

`SqlCore` must reproduce these **byte-for-byte** — the editor's "run statement under cursor", the read-only/destructive guards, and syntax highlighting all depend on their exact outputs.

1. **`SqlStatementSplitter`** — single-pass scanner splitting on top-level `;`, ignoring `;` inside `'...'` strings (with `''` escape via *toggle-then-peek*), `-- ` / `/* */` comments, and `$tag$...$tag$` dollar-quotes (PL/pgSQL bodies stay one statement). Plus `statementAtOffset` (maps a cursor offset to its statement, favoring the statement *ending* at the cursor). Retains trailing `;`.
2. **`SqlStatementClassifier`** — `strippedUppercasedCode` (blanks literal/comment bodies to one space) → tokenize → classify leading keyword as read/write/neutral using **4 hardcoded keyword sets** (precedence: write→read→neutral, then `WITH`/`EXPLAIN` special-cased), plus `isDestructive` (`DROP`/`TRUNCATE`, or `DELETE`/`UPDATE` **without** `WHERE`).
3. **`SqlLiteralScanner`** — returns UTF-16 `(location, length, isComment)` ranges for the highlighter. **Note:** its dollar-tag charset is ASCII-only `[A-Za-z0-9_]`, unlike the splitter/classifier (Unicode-aware). Mirror this divergence.
4. **`PostgresHba`** — turns a "no pg_hba.conf entry for host…" rejection into a suggested `host  <db>  <user>  <cidr>  scram-sha-256` line (exactly **four spaces** between fields; `/32` IPv4, `/128` IPv6).

**Index space:** standardize the editor + splitter on **UTF-16 (`wchar_t`)** throughout so RichEdit selection offsets feed `statementAtOffset` without conversion. For ASCII these coincide with the original grapheme indices; document the choice. Exact keyword tables, edge cases, and the full assertion tables are in **Appendix B**.

**De-risking gate:** transcribe the Swift `SQLCoreTests` assertion tables verbatim into the C++ golden suite and get them green **before** building anything that depends on `SqlCore`.

---

## 6. Phased plan

Each phase is independently runnable/testable. The two de-risking gates (P0 golden tests, P3 concurrency) come early on purpose.

| Phase | Goal | Key deliverables | Exit criteria |
|---|---|---|---|
| **P0** Toolchain + SqlCore | Build + pin pure logic before any UI/DB risk | CMake/MSVC x64 skeleton + empty `WinMain` window (comctl32 v6 manifest); vcpkg manifest (libpq, openssl, nlohmann-json, gtest); `sqlite3.c` compiling; `SqlCore.lib`; golden tests | **All four Swift assertion tables green.** |
| **P1** Hello window + SQLite | The milestone: connect to a `.sqlite`, run SQL, show a grid | Minimal `SqliteProvider` (tail-pointer multi-statement loop, `NULL`→"NULL"); `QueryResult`; plain `SysListView32`; multiline edit + Run (Ctrl+E); sync exec OK for now | SELECT renders a grid. |
| **P2** Editor + highlighting + scrollback | Real SQL editor + terminal-style output | MSFTEDIT editor + `SyntaxHighlighter` (numbers→keywords→literals order, 4 colors, IME-safe, reentrancy-guarded); scrollback log w/ auto-scroll; draggable splitter; Ctrl+Enter via `statementAtOffset` | Highlighting + run-selection work. |
| **P3** Worker session + cancellation | Move DB I/O off the UI thread, correctly | `DatabaseSession` (thread + queue + mutex); `WM_APP_RESULT` marshalling; cancellable/uncancellable; Ctrl+. cancel via `sqlite3_interrupt` | **SQLite cancel verified mid-query; no deadlock/UAF.** |
| **P4** PostgreSQL via libpq | Full PG engine + proper cancel | conninfo (`sslmode` disable/prefer/require); single `PQconnectdb`; `isSSLActive` via `PQsslInUse`; multi-statement (`SqlCore` split + per-stmt `PQexec`, skip meta/comment-only, first-error-stops, last-set-wins); rich errors via `PQresultErrorField` + HBA hint; `PQgetCancel`/`PQcancel` | Connect to RDS/Supabase/local; cancel survives. |
| **P5** Persistence + credentials + connect UI | Profiles, recents, history, snippets, passwords, dialog | JSON stores under `%APPDATA%` (caps/dedup/sort, mutex-guarded); `CredentialStore`; `ConnectionSheet` dialog (engine picker, file dialogs, fields, password eye-toggle, SSL picker, saved/recent menu, save-profile); deferred last-connection save | Reconnect from a saved profile with stored password. |
| **P6** Guards + transactions + dot-commands + schema | `TerminalController` completeness | Read-only block + destructive-confirm dialog (exact keyword sets/messages); transaction tracking + toolbar menu; full `DotCommandHandler` (Appendix A); Ctrl+Up/Down history nav; smart-quote normalization; schema TreeView (lazy columns via uncancellable path) | All dot-commands + guards behave like macOS. |
| **P7** Results polish + secondary dialogs | Grid parity + remaining surfaces | Virtual `LVS_OWNERDATA` grid; column-click sort (`smartCompare`, NULL-last, natural, stable index sort); `NM_CUSTOMDRAW` alternating rows + sort arrow; per-cell context menu; CSV/TSV export; History/Snippets dialog; CellDetail (JSON pretty + char count); ConnectionDetails popover; status-bar lock/running/readonly/in-tx | 100k-row result scrolls smoothly. |
| **P8** Multi-window + menus + lifecycle | Multi-document behavior | `WindowManager` (`HWND`→session map); Ctrl+N / Ctrl+W; quit-on-last-window; full accelerator table; per-window `HMENU`; About/Help singletons | Open/close many windows, no thread/handle leaks. |
| **P9** Auto-update + packaging + hardening | Ship-ready | WinSparkle init + Check-for-Updates menu (enable/disable); Windows `appcast.xml` + signed package; `VERSIONINFO`/About; installer (Inno/WiX or zip); dark-mode palette; final error-message/edge pass; cancel/recover soak test | Signed build self-updates end-to-end on a test channel. |

A **genuinely usable internal build** lands at the end of **P6** (both engines, off-thread execution, persistence, guards, dot-commands, schema). P7–P9 are parity polish and shipping.

---

## 7. Risks & mitigations

| # | Risk | Sev | Mitigation |
|---|------|:---:|-----------|
| 1 | **libpq cancellation** differs from the force-close the app was built around (connection survives). | High | Use `PQgetCancel()`+`PQcancel()`; snapshot the `PGcancel*` after connect; cancel touches only that handle; **drop** `recoverAfterCancel` for PG. |
| 2 | **Off-thread exec + marshalling** — naive rebuild risks deadlock (mutex across blocking I/O) or use-after-free (disconnect frees handle under cancel). | High | The §4 discipline: brief mutex only; `disconnect` via the worker queue; heap `QueryResult*` + `PostMessage`; busy-flag-before-dispatch. |
| 3 | **RichEdit highlighting** reentrancy/flicker/IME corruption; O(n) per keystroke. | Med | `isProgrammaticChange` guard; `WM_SETREDRAW` off/on + save/restore selection+scroll; suppress recolor between `WM_IME_STARTCOMPOSITION`/`ENDCOMPOSITION`; `WM_TIMER` debounce on large buffers. |
| 4 | **ListView virtual mode** — default insert-per-row dies past ~10–50k rows; `OWNERDATA` can't reorder. | Med | `LVS_OWNERDATA` from the start; serve via `LVN_GETDISPINFO`; sort via a separate index-permutation vector; `NM_CUSTOMDRAW` for shading/arrow; `PQsetSingleRowMode` for huge PG results. Perf-gate at 100k rows. |
| 5 | **Multi-window** must be hand-built (registry, key routing, quit-on-last). | Med | Central `WindowManager` owning `map<HWND, WindowState>`; route accelerators via foreground window; on `WM_DESTROY` remove, join worker, `PostQuitMessage(0)` at zero. Open/close leak test. |
| 6 | **Algorithm fidelity** — `''` toggle-then-peek, stripped-code spacing, ASCII-vs-Unicode dollar tags, keyword precedence. | Med | Port from source; gate with golden tests at P0 before dependents; document deliberate ASCII simplifications. |
| 7 | **Credential storage divergence** (no exact `WhenUnlocked` analog). | Low | Account key `"<user>@<host>:<port>/<db>"`; TargetName `"SQLTerminal:"+account`; `CRED_TYPE_GENERIC`, `CRED_PERSIST_LOCAL_MACHINE`; delete-then-write upsert; idempotent delete. Round-trip unit test. |
| 8 | **WinSparkle signature/appcast** scheme mismatch (Ed25519 vs DSA; package format). | Low | Target a modern WinSparkle (Ed25519); reuse the key; produce a Windows appcast referencing a signed zip/installer; verify end-to-end first. |
| 9 | **Dot-command SQL injection** (args interpolated unescaped) carried forward. | Low | Port faithfully for parity; harden `quotedIdentifier` (double embedded quotes consistently); note in docs. |

---

## 8. Parity gaps (deliberate deltas on Windows)

| Feature | Difficulty | Decision |
|---|---|---|
| App Sandbox security-scoped bookmarks | Trivial (drop) | No Windows analog; plain file path. **Removes** code. |
| `~` home-path expansion | Trivial | Drop, or map leading `~`→`%USERPROFILE%`. |
| Single global macOS menu bar | Medium | Per-window `HMENU`; Help rightmost, About under Help; Cmd→Ctrl. Functionally equal, not pixel-identical. |
| **Free-text selection + embedded grids in one scroll surface** | **Hard** | Compromise: custom virtual scrollback where result blocks host child ListViews (grid selection via the ListView; copy input/error/system lines via context menu). The single biggest layout-fidelity rebuild. |
| Reactive `@Published` auto-binding | Medium | Explicit dirty-flag + targeted `InvalidateRect`/control updates. |
| Animated auto-scroll / `.relative` timestamps / `localizedStandardCompare` | Easy–Med | Drop the animation; hand-write a relative-time formatter; natural sort via `CompareStringEx`+`SORT_DIGITSASNUMBERS`. |
| Dark mode semantic colors | Medium | Manual palette + `DwmSetWindowAttribute`; the 4 syntax colors + accent tint are the must-haves. |
| Reading existing macOS `UserDefaults` history | Trivial (skipped) | Clean-room store under `%APPDATA%`; no cross-platform data interchange. |

---

## 9. Effort estimate

For **one experienced Win32/C++ engineer**:

- **Full parity:** ~**12–18 weeks** (~3–4.5 months).
- **Usable internal build (through P6):** ~**7–10 weeks**.

Rough weighting: SqlCore+tests ~1w (low-risk); SQLite+first window ~1w; editor/highlighting/scrollback ~1.5–2w; **concurrency core ~1.5–2w (highest risk)**; libpq PG ~2w; persistence+credentials+dialog ~1.5–2w; guards+tx+dot-cmds+schema ~1.5–2w; results virtual grid+dialogs ~2w; multi-window+menus ~1w; auto-update+packaging+hardening ~1–1.5w.

> **~60% of the effort is UI rebuild + the threading/cancellation model.** The pure Core and the SQLite engine are the cheapest, safest, near-direct ports. libpq materially *reduces* PostgreSQL effort vs. the original (auth probing + SSL fallback collapse into one connection string).

---

## Appendix A — Dot-commands (reproduce exactly)

Parser: trim; if not prefixed `.` → treat as raw SQL. Split once on first whitespace → `command` (lowercased) + `argument` (case preserved). Result is one of: `sql(String)`, `multiSQL([String])`, `message(String)`, `clear`, `reconnect(String)`.

| Command | SQLite | PostgreSQL |
|---|---|---|
| `.tables` | `SELECT name FROM sqlite_master WHERE type='table' ORDER BY name;` | `SELECT tablename FROM pg_tables WHERE schemaname='public' ORDER BY tablename;` |
| `.views` / `.indexes [t]` / `.schema [t]` / `.columns <t>` / `.count <t>` / `.first <t>` / `.last <t>` / `.fk <t>` / `.dbinfo` / `.size` / `.encoding` | `sqlite_master` / `PRAGMA` family, `rowid` | `pg_*` / `information_schema`, `ctid` |
| `.databases` / `.schemas` | (n/a → message) | `pg_database` / `information_schema.schemata` |
| `.connect <db>` / `.use <db>` | — | → `reconnect(db)` (keep credentials) |
| `.clear` | → `clear` | → `clear` |
| `.help` | `sqliteHelpText` | `postgresHelpText` |

> Full per-command SQL templates live in `../SQLTerminal/SQLTerminal/Utilities/DotCommandHandler.swift` — transcribe verbatim, preserving the SQLite-vs-Postgres branches and identifier quoting (double embedded quotes).

## Appendix B — Algorithm contract details

- **Keyword sets** (uppercase, exact):
  - `read = {SELECT, TABLE, VALUES, SHOW, DESCRIBE, DESC, PRAGMA}`
  - `write = {INSERT, UPDATE, DELETE, MERGE, REPLACE, UPSERT, CREATE, DROP, ALTER, TRUNCATE, RENAME, GRANT, REVOKE, COMMENT, SECURITY, REINDEX, VACUUM, CLUSTER, COPY, CALL, DO, REFRESH, IMPORT, LOAD, ATTACH, DETACH}`
  - `neutral = {BEGIN, START, COMMIT, ROLLBACK, END, ABORT, SAVEPOINT, RELEASE, SET, RESET, USE, DISCARD, LISTEN, UNLISTEN, NOTIFY, CHECKPOINT, ANALYZE, DEALLOCATE, PREPARE, EXECUTE, FETCH, MOVE, CLOSE, DECLARE}`
  - `dataModifying = {INSERT, UPDATE, DELETE, MERGE}` · `WITH`/`EXPLAIN` are special-cased (not in any set).
- **Classifier order:** write→read→neutral; then `WITH`→write iff any token ∈ dataModifying else read; `EXPLAIN`→write iff token `ANALYZE` present else read. Destructive: `DROP`/`TRUNCATE` always; `DELETE`/`UPDATE` iff no `WHERE` token (on stripped code, so `WHERE` inside a string doesn't count).
- **Splitter `''` rule:** on `'`, toggle `inSingleQuote`; only when the toggle turned it *off* and the next char is also `'` is it an escaped pair (re-enter string, consume 2). Reproduce the toggle-then-peek, not a generic "two quotes = escape".
- **`strippedUppercasedCode`:** emit exactly one space when *closing* a literal/comment; drop the opening delimiter and the body entirely.
- **Scanner ranges:** UTF-16 `(location,length)`; line comment **excludes** the trailing `\n`; block comment **includes** `*/`; unterminated literal/comment runs to end. Dollar-tag charset ASCII-only.
- **Golden tests to transcribe** from `../SQLTerminal/Tests/SQLCoreTests/`: `split(";;;")==[";",";",";"]`; whole `$$…$$` body is one statement; `statementAtOffset` mapping; classifier reads/writes/neutral + `WHERE`-in-string + commented-out keyword; scanner `(location,length,isComment)` tuples; HBA four-space output + nil cases. **These tables are the contract.**

## Appendix C — Platform contracts

- **Credential key:** account = `"<username>@<host>:<port>/<databaseName>"`; TargetName = `"SQLTerminal:"+account`. SQLite stores no password.
- **Persistence (JSON under `%APPDATA%\SQLTerminal\`):** `recents` (cap 10, MRU, dedup by `ConnectionProfile.id`), `profiles` (uncapped, sorted by displayName, case-insensitive), `history` (cap 200, dedup by exact trimmed SQL, bump `runCount`/`lastRun`), `snippets` (uncapped, upsert by case-insensitive name, sorted). Decode failure → empty list. `ConnectionProfile.id` = `"named:<name>"` | `"sqlite:<filePath>"` | `"postgres:<user>@<host>:<port>/<db>"`. `sslMode` optional → default `prefer`.
- **Keyboard (Cmd→Ctrl):** Ctrl+E run editor · Ctrl+Enter run selection/statement-under-cursor · Ctrl+. cancel · Ctrl+Up/Down history · Ctrl+N new window · Ctrl+W close · F1/Ctrl+? Help.
- **WinSparkle:** appcast concept reused (macOS feed: `https://raw.githubusercontent.com/arcanii/SQLTerminal/main/appcast.xml`, EdDSA key `sKPprIa95Hw+DX3bMoxWMsyC0w9vc4MzEpgx7TBDP1I=`). Produce a **Windows** appcast + package; reuse the Ed25519 key on modern WinSparkle.
- **Window/UI constants:** default window 1100×750 (min 900×600); editor default height 150; splitter clamp `[80, total-100-30]`, cursor `IDC_SIZENS`; sidebar 240; status bar 30. Dialog sizes: ConnectionSheet 520×(380/660), History 540×460, CellDetail 540×440, Help 480×580, About w350.

---

*Generated from a full subsystem analysis of `../SQLTerminal` (Core, Models, Providers, ViewModels, Views, App/Utilities) plus a Win32 architecture synthesis. See `../SQLTerminal/docs/BACKLOG.md` for candidate post-parity features (e.g. MySQL/MariaDB engine).*
