// SPDX-License-Identifier: GPL-3.0-or-later
// Main window: results ListView (top), draggable splitter, RichEdit SQL editor
// (bottom) with live highlighting, and a 2-part status bar. Run the whole editor
// (Ctrl+E) or the statement at the cursor (Ctrl+Enter); cancel with Ctrl+.;
// navigate command history with Ctrl+Up/Down. Dot-commands, read-only mode,
// destructive-statement confirmation, and transactions are handled here via the
// pure SqlApp logic. Execution runs off the UI thread (DatabaseSession).
#include "ui/MainWindow.h"

#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "app/DotCommandHandler.h"
#include "app/ResultFormat.h"
#include "app/TerminalLogic.h"
#include "core/SqlStatementSplitter.h"
#include "core/SqlSyntaxHighlighter.h"
#include "db/DatabaseSession.h"
#include "models/ConnectionProfile.h"
#include "models/DatabaseConnection.h"
#include "models/DatabaseEngine.h"
#include "models/QueryResult.h"
#include "persistence/Stores.h"
#include "platform/Updater.h"
#include "resource.h"
#include "security/CredentialStore.h"
#include "version.h"
#include "ui/CellDetailDialog.h"
#include "ui/ConnectionDialog.h"
#include "ui/HistoryDialog.h"
#include "ui/SqlEditorControl.h"
#include "ui/SqlGridControl.h"
#include "ui/ThemedDialog.h"
#include "ui/Theme.h"

namespace sqlterm {
namespace {

constexpr wchar_t kClassName[] = L"SQLTerminalMainWindow";
constexpr wchar_t kSplitterClass[] = L"SQLTerminalSplitter";
constexpr wchar_t kVSplitterClass[] = L"SQLTerminalVSplitter";
constexpr wchar_t kCmdBarClass[] = L"SQLTerminalCmdBar";

enum : int {
    ID_OPEN = 1001,
    ID_EXIT = 1002,
    ID_RUN = 1003,
    ID_RUN_STMT = 1004,
    ID_CANCEL = 1005,
    ID_READONLY = 1006,
    ID_TX_BEGIN = 1007,
    ID_TX_COMMIT = 1008,
    ID_TX_ROLLBACK = 1009,
    ID_HIST_UP = 1010,
    ID_HIST_DOWN = 1011,
    ID_REFRESH_SCHEMA = 1012,
    ID_CTX_INSERT = 1013,
    ID_CTX_PREVIEW = 1014,
    ID_CTX_COPY = 1015,
    ID_HISTORY = 1021,
    ID_CONN_DETAILS = 1022,
    ID_NEW = 1023,
    ID_CLOSE = 1024,
    ID_ABOUT = 1025,
    ID_HELP = 1026,
    ID_CHECK_UPDATES = 1027,
    ID_MENU = 1028,
    ID_TOGGLE_THEME = 1029,
    IDC_LIST = 2001,
    IDC_EDIT = 2002,
    IDC_SPLIT = 2004,
    IDC_STATUS = 2005,
    IDC_TREE = 2006,
    IDC_VSPLIT = 2007,
};

constexpr UINT WM_APP_RESULT = WM_APP + 1;     // lParam = QueryResult*
constexpr UINT WM_APP_CONNECTED = WM_APP + 2;  // lParam = ConnectMsg*
constexpr UINT WM_APP_TABLES = WM_APP + 3;     // lParam = TablesMsg*
constexpr UINT WM_APP_COLUMNS = WM_APP + 4;    // lParam = ColumnsMsg*

struct ConnectMsg {
    bool ok;
    std::wstring error;
};
struct TablesMsg {
    std::vector<std::wstring> tables;
};
struct ColumnsMsg {
    HTREEITEM node;
    std::vector<std::wstring> lines;
};

constexpr int kSplitterHeight = 4;
constexpr int kMinEditor = 80;
constexpr int kMinList = 100;
constexpr int kCmdBarH = 46;
constexpr int kPaneInset = 2;   // child margin inside its slot (the rounded-card gap)
constexpr int kFrameInset = 0;  // frame flush to the slot edge (tight inter-pane seam)
constexpr int kPaneRadius = 8;  // rounded-card corner radius

// Multi-window: one heap AppState per window; quit when the last one closes.
int g_windowCount = 0;
HINSTANCE g_appInstance = nullptr;

struct AppState {
    HWND hwnd = nullptr;
    HWND hList = nullptr;
    HWND hEdit = nullptr;
    HWND hSplitter = nullptr;
    HWND hStatus = nullptr;
    HWND hTree = nullptr;
    HWND hVSplitter = nullptr;
    HFONT hUi = nullptr;
    HFONT hGlyph = nullptr;     // Segoe MDL2 Assets, for command-bar icons
    HWND hCmdBar = nullptr;     // custom top command bar (replaces the menu)
    int cmdHover = -1;          // hovered command-bar button index, or -1
    int capHover = -1;          // hovered caption button (0=min 1=max 2=close), or -1
    int dpi = 96;               // window DPI (per-monitor-v2)
    std::wstring statusMsg;   // custom-painted status bar text (left part)
    int editorHeight = 150;
    int sidebarWidth = 220;
    RECT treeSlot{}, listSlot{}, editSlot{};  // pane rects for the rounded frames (parent-painted)
    std::wstring contextTable;  // table the schema context menu acted on
    bool busy = false;
    bool cancelRequested = false;
    DatabaseSession session;

    std::wstring dbName;
    DatabaseConnection currentConnection;
    DatabaseEngine currentEngine = DatabaseEngine::Sqlite;
    bool readOnly = false;
    bool inTransaction = false;
    bool sslActive = false;
    CommandHistory cmdHistory;
    std::vector<std::wstring> lastRunStatements;

    // Pending connect (applied on WM_APP_CONNECTED success).
    DatabaseConnection pendingConn;
    std::optional<std::wstring> pendingSaveAs;
    bool pendingRemember = false;
    bool pendingTouchCredentials = false;
};

// Scale a 96-dpi design value to the window's current DPI.
int dp(int v, int dpi) { return MulDiv(v, dpi, 96); }

// Create (or recreate, on DPI change) the UI + glyph fonts at st->dpi. (Content
// renders via DirectWrite now; the editor and grid own their own Consolas formats.)
void createFonts(AppState* st) {
    if (st->hUi) DeleteObject(st->hUi);
    if (st->hGlyph) DeleteObject(st->hGlyph);
    st->hUi = CreateFontW(-dp(14, st->dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
    st->hGlyph = CreateFontW(-dp(17, st->dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe MDL2 Assets");
}

double epochNow() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

void applyDarkTitleBar(HWND hwnd) {
    const Theme& th = currentTheme();
    BOOL dark = th.dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    // Win11 (build 22000+): paint the caption + border to match the command bar
    // for a seamless dark top strip. No-ops harmlessly on older Windows.
    COLORREF caption = th.panelElevBg, text = th.textSecondary, border = th.border;
    DwmSetWindowAttribute(hwnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));
    DwmSetWindowAttribute(hwnd, 36 /* DWMWA_TEXT_COLOR */, &text, sizeof(text));
    DwmSetWindowAttribute(hwnd, 34 /* DWMWA_BORDER_COLOR */, &border, sizeof(border));
}

// editorText() and caretOffset() now live in SqlEditorControl.cpp — the Direct2D
// editor reads its EditorModel directly. Syntax highlighting is internal to that
// control, so the RichEdit-era applyHighlight()/colorFor() helpers are gone.

void showInfoDialog(HWND owner, const wchar_t* title, const std::wstring& body);

void setStatus(AppState* st, const std::wstring& text) {
    st->statusMsg = text;
    if (st->hStatus) InvalidateRect(st->hStatus, nullptr, FALSE);
}

void updateFlags(AppState* st) {
    if (st->hStatus) InvalidateRect(st->hStatus, nullptr, FALSE);  // flags read from st in paint
}

void setTitle(AppState* st) {
    std::wstring title = L"SQLTerminal";
    if (!st->dbName.empty()) title += L" — " + st->dbName;
    SetWindowTextW(st->hwnd, title.c_str());
}

int statusHeight(AppState* st) {
    if (!st->hStatus) return 0;
    RECT r;
    GetWindowRect(st->hStatus, &r);
    return r.bottom - r.top;
}

void layout(AppState* st) {
    if (st->hStatus) SendMessageW(st->hStatus, WM_SIZE, 0, 0);
    RECT rc;
    GetClientRect(st->hwnd, &rc);
    const int d = st->dpi;
    const int cw = rc.right;
    const int top = dp(kCmdBarH, d);
    const int splitH = dp(kSplitterHeight, d);
    const int minList = dp(kMinList, d);
    const int minEdit = dp(kMinEditor, d);
    const int ch = (std::max)(0, static_cast<int>(rc.bottom) - statusHeight(st) - top);

    // Batch all pane moves into one atomic pass so a resize repaints the panes
    // together rather than child-by-child (which causes drag lag).
    HDWP dwp = BeginDeferWindowPos(6);
    auto place = [&](HWND h, int x, int y, int w, int hh) {
        if (h && dwp)
            dwp = DeferWindowPos(dwp, h, nullptr, x, y, w, hh, SWP_NOZORDER | SWP_NOACTIVATE);
    };

    place(st->hCmdBar, 0, 0, cw, top);

    // Left: schema sidebar (tree) + vertical splitter.
    const int vsW = splitH;
    int sw = st->sidebarWidth;
    int maxSidebar = cw - dp(200, d) - vsW;
    if (maxSidebar < dp(120, d)) maxSidebar = (std::max)(0, cw - vsW);
    if (sw > maxSidebar) sw = maxSidebar;
    if (sw < 0) sw = 0;
    const int rx = sw + vsW;
    const int rw = (std::max)(0, cw - rx);
    const int paneM = dp(kPaneInset, d);
    auto placeInset = [&](HWND h, const RECT& s) {
        place(h, static_cast<int>(s.left) + paneM, static_cast<int>(s.top) + paneM,
              (std::max)(0, static_cast<int>(s.right - s.left) - 2 * paneM),
              (std::max)(0, static_cast<int>(s.bottom - s.top) - 2 * paneM));
    };
    st->treeSlot = {0, top, sw, top + ch};
    placeInset(st->hTree, st->treeSlot);
    place(st->hVSplitter, sw, top, vsW, ch);

    // Right: results / horizontal splitter / editor.
    int editH = st->editorHeight;
    int maxEdit = ch - minList - splitH;
    if (maxEdit < minEdit) maxEdit = (std::max)(0, ch - splitH);
    if (editH > maxEdit) editH = maxEdit;
    if (editH < 0) editH = 0;
    int listH = ch - editH - splitH;
    if (listH < 0) listH = 0;

    st->listSlot = {rx, top, rx + rw, top + listH};
    st->editSlot = {rx, top + listH + splitH, rx + rw, top + listH + splitH + editH};
    placeInset(st->hList, st->listSlot);
    place(st->hSplitter, rx, top + listH, rw, splitH);
    placeInset(st->hEdit, st->editSlot);
    if (dwp) EndDeferWindowPos(dwp);

    if (st->hStatus) {
        int parts[2] = {cw - dp(220, d), -1};
        SendMessageW(st->hStatus, SB_SETPARTS, 2, reinterpret_cast<LPARAM>(parts));
        updateFlags(st);
    }
    InvalidateRect(st->hwnd, nullptr, FALSE);  // repaint the rounded pane frames in the margins
}

// The results grid is the self-contained SqlGridControl (src/ui/SqlGridControl.*):
// it owns the data, sort, selection, scrolling, and the per-cell context menu.
void showResult(AppState* st, const QueryResult& r) {
    wchar_t buf[160];
    if (!r.columns.empty()) {
        gridSetResult(st->hList, r);
        std::swprintf(buf, 160, L"%llu rows · %.0f ms",
                      static_cast<unsigned long long>(r.rows.size()), r.executionTimeSec * 1000.0);
    } else {
        gridClear(st->hList);
        std::swprintf(buf, 160, L"%lld rows affected · %.0f ms", r.rowsAffected,
                      r.executionTimeSec * 1000.0);
    }
    setStatus(st, buf);
}

void startExecute(AppState* st, const std::wstring& sql) {
    st->busy = true;
    st->cancelRequested = false;
    if (st->hCmdBar) InvalidateRect(st->hCmdBar, nullptr, FALSE);
    setStatus(st, L"Running…  (Ctrl+. to cancel)");
    HWND hwnd = st->hwnd;
    st->session.executeAsync(sql, [hwnd](QueryResult r) {
        auto* p = new QueryResult(std::move(r));
        if (!PostMessageW(hwnd, WM_APP_RESULT, 0, reinterpret_cast<LPARAM>(p))) delete p;
    });
}

void doReconnect(AppState* st, const std::wstring& dbName) {
    DatabaseConnection cfg = st->currentConnection;
    cfg.databaseName = dbName;
    st->pendingConn = cfg;
    st->pendingSaveAs = std::nullopt;
    st->pendingRemember = false;
    st->pendingTouchCredentials = false;
    setStatus(st, L"Switching to \"" + dbName + L"\"…");
    HWND hwnd = st->hwnd;
    st->session.connectAsync(cfg, [hwnd](bool ok, std::wstring err) {
        auto* m = new ConnectMsg{ok, std::move(err)};
        if (!PostMessageW(hwnd, WM_APP_CONNECTED, 0, reinterpret_cast<LPARAM>(m))) delete m;
    });
}

// Apply the read-only block / destructive confirm, then run.
void guardAndRun(AppState* st, const std::vector<std::wstring>& statements) {
    const GuardDecision d = evaluateGuard(statements, st->readOnly);
    if (d.action == GuardAction::Block) {
        setStatus(st, d.message);
        return;
    }
    if (d.action == GuardAction::Confirm) {
        if (themedMessageBox(st->hwnd, d.message.c_str(), L"Confirm destructive statement",
                             MB_YESNO | MB_ICONWARNING) != IDYES) {
            setStatus(st, L"Cancelled — destructive statement not run.");
            return;
        }
    }
    st->lastRunStatements = statements;
    std::wstring joined;
    for (size_t i = 0; i < statements.size(); ++i) {
        if (i) joined += L"\n";
        joined += statements[i];
    }
    startExecute(st, joined);
}

void runText(AppState* st, const std::wstring& rawText, bool clearAfter, bool recordHistory = true) {
    if (!st->session.isConnected()) {
        setStatus(st, L"No database — press Ctrl+O to connect.");
        return;
    }
    if (st->busy) {
        setStatus(st, L"A query is already running — Ctrl+. to cancel.");
        return;
    }
    const std::wstring input = normalizeSmartCharacters(rawText);
    if (input.empty()) return;

    st->cmdHistory.add(input);
    if (recordHistory) QueryHistoryStore::record(input, epochNow());
    if (clearAfter) SetWindowTextW(st->hEdit, L"");

    if (auto dot = handleDotCommand(input, st->currentEngine)) {
        switch (dot->kind) {
            case DotKind::Sql:
            case DotKind::MultiSql:
                guardAndRun(st, dot->statements);
                break;
            case DotKind::Message:
                showInfoDialog(st->hwnd, L"SQLTerminal", dot->text);
                break;
            case DotKind::Clear:
                gridClear(st->hList);
                setStatus(st, L"Cleared.");
                break;
            case DotKind::Reconnect:
                doReconnect(st, dot->text);
                break;
        }
    } else {
        guardAndRun(st, {input});
    }
}

void doRunWhole(AppState* st) { runText(st, editorText(st->hEdit), /*clearAfter=*/true); }

void doRunStatement(AppState* st) {
    const std::wstring text = editorText(st->hEdit);
    const auto stmt = sqlcore::SqlStatementSplitter::statementAtOffset(caretOffset(st->hEdit), text);
    if (stmt && !stmt->empty())
        runText(st, *stmt, /*clearAfter=*/false);
    else
        runText(st, text, /*clearAfter=*/true);
}

void doTransaction(AppState* st, const std::wstring& keyword) {
    runText(st, keyword, /*clearAfter=*/false, /*recordHistory=*/false);
}

void doCancel(AppState* st) {
    if (!st->busy) return;
    st->cancelRequested = true;
    st->session.cancel();
    setStatus(st, L"Cancelling…");
}

void doHistoryUp(AppState* st) {
    if (auto t = st->cmdHistory.up(editorText(st->hEdit))) SetWindowTextW(st->hEdit, t->c_str());
}
void doHistoryDown(AppState* st) {
    if (auto t = st->cmdHistory.down()) SetWindowTextW(st->hEdit, t->c_str());
}

void toggleReadOnly(AppState* st) {
    st->readOnly = !st->readOnly;
    updateFlags(st);  // checkmark is set when the popup menu is built
}

void applyConnectionPersistence(AppState* st) {
    const DatabaseConnection& c = st->pendingConn;
    st->currentConnection = c;
    st->currentEngine = c.engine;
    st->inTransaction = false;
    if (c.engine == DatabaseEngine::Sqlite) {
        const size_t slash = c.filePath.find_last_of(L"\\/");
        st->dbName = (slash == std::wstring::npos) ? c.filePath : c.filePath.substr(slash + 1);
    } else {
        st->dbName = c.databaseName;
    }
    RecentConnectionsStore::add(ConnectionProfile(c));
    if (st->pendingSaveAs) SavedProfilesStore::save(ConnectionProfile(c, st->pendingSaveAs));
    if (st->pendingTouchCredentials && c.engine == DatabaseEngine::Postgres) {
        const std::wstring acct = CredentialStore::accountKey(c);
        if (st->pendingRemember && !c.password.empty())
            CredentialStore::savePassword(acct, c.password);
        else
            CredentialStore::deletePassword(acct);
    }
}

void doOpen(AppState* st) {
    if (st->busy) {
        setStatus(st, L"A query is running — Ctrl+. to cancel before opening.");
        return;
    }
    auto req = showConnectionDialog(st->hwnd);
    if (!req) return;
    st->pendingConn = req->connection;
    st->pendingSaveAs = req->saveAsName;
    st->pendingRemember = req->rememberPassword;
    st->pendingTouchCredentials = true;
    setStatus(st, L"Connecting…");
    HWND hwnd = st->hwnd;
    st->session.connectAsync(req->connection, [hwnd](bool ok, std::wstring err) {
        auto* m = new ConnectMsg{ok, std::move(err)};
        if (!PostMessageW(hwnd, WM_APP_CONNECTED, 0, reinterpret_cast<LPARAM>(m))) delete m;
    });
}

void doHistory(AppState* st) {
    if (auto sql = showHistorySnippets(st->hwnd, editorText(st->hEdit)))
        SetWindowTextW(st->hEdit, sql->c_str());
}

// ---- generic dark info dialog (Help, Connection Details, dot-command output) -
struct InfoState {
    UINT dpi = 96;
    bool done = false;
};

LRESULT CALLBACK InfoProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* st = reinterpret_cast<InfoState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        case WM_ERASEBKGND: {
            // Paint the surface from the *current* theme, not the class brush
            // captured at first registration (which would stay dark in light mode).
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect(reinterpret_cast<HDC>(wParam), &rc, themeBrush(currentTheme().panelBg));
            return 1;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORBTN:
            return dialogCtlColor(msg, wParam);
        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) st->done = true;
            return 0;
        case WM_CLOSE:
            st->done = true;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void showInfoDialog(HWND owner, const wchar_t* title, const std::wstring& body) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = InfoProc;
        wc.hInstance = g_appInstance;
        wc.lpszClassName = L"SQLTerminalInfo";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;  // painted per current theme in WM_ERASEBKGND
        RegisterClassExW(&wc);
        registered = true;
    }

    InfoState st;
    const int W = 470, H = 320;
    const UINT odpi = GetDpiForWindow(owner);
    RECT orc{};
    GetWindowRect(owner, &orc);
    const int fullW = dpiScale(W + 16, odpi), fullH = dpiScale(H + 39, odpi);
    const int x = orc.left + ((orc.right - orc.left) - fullW) / 2;
    const int y = orc.top + ((orc.bottom - orc.top) - fullH) / 2;
    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"SQLTerminalInfo", title,
                                WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, fullW, fullH, owner, nullptr,
                                g_appInstance, &st);
    if (!hwnd) return;
    st.dpi = GetDpiForWindow(hwnd);
    HFONT mono = CreateFontW(-dpiScale(13, st.dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    HFONT ui = CreateFontW(-dpiScale(14, st.dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
    std::wstring bodyCrlf;  // EDIT controls render bare LF as a box — use CRLF.
    bodyCrlf.reserve(body.size() + 32);
    for (wchar_t c : body) {
        if (c == L'\r') continue;
        if (c == L'\n')
            bodyCrlf += L"\r\n";
        else
            bodyCrlf += c;
    }
    HWND edit = CreateWindowExW(0, L"EDIT", bodyCrlf.c_str(),
                                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
                                    ES_AUTOVSCROLL,
                                dpiScale(14, st.dpi), dpiScale(12, st.dpi), dpiScale(W - 28, st.dpi),
                                dpiScale(H - 60, st.dpi), hwnd,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(100)), g_appInstance,
                                nullptr);
    SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(mono), TRUE);
    HWND ok = CreateWindowExW(0, L"BUTTON", L"OK",
                              WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP,
                              dpiScale(W - 100, st.dpi), dpiScale(H - 40, st.dpi), dpiScale(88, st.dpi),
                              dpiScale(28, st.dpi), hwnd,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), g_appInstance,
                              nullptr);
    SendMessageW(ok, WM_SETFONT, reinterpret_cast<WPARAM>(ui), TRUE);
    applyDialogDarkMode(hwnd);
    EnableWindow(owner, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (!st.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    DestroyWindow(hwnd);
    DeleteObject(mono);
    DeleteObject(ui);
}

void doConnectionDetails(AppState* st) {
    // Left-justify each "Label:" to a fixed width so values line up in a column
    // (the info dialog renders in a monospace font).
    auto row = [](const std::wstring& label, const std::wstring& value) {
        std::wstring s = label;
        while (s.size() < 12) s.push_back(L' ');
        return s + value;
    };
    std::wstring m;
    if (!st->session.isConnected()) {
        m = L"Not connected.";
    } else if (st->currentConnection.engine == DatabaseEngine::Postgres) {
        const auto& c = st->currentConnection;
        m = row(L"Engine:", L"PostgreSQL") + L"\n" + row(L"Host:", c.host) + L"\n" +
            row(L"Port:", c.port) + L"\n" + row(L"Database:", c.databaseName) + L"\n" +
            row(L"User:", c.username) + L"\n" +
            row(L"Encryption:", st->sslActive ? L"SSL/TLS" : L"none");
    } else {
        m = row(L"Engine:", L"SQLite") + L"\n" + row(L"File:", st->currentConnection.filePath);
    }
    showInfoDialog(st->hwnd, L"Connection Details", m);
}

LRESULT CALLBACK StatusSubclassProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

void createChildren(AppState* st, HINSTANCE hInst) {
    const Theme& th = currentTheme();
    createFonts(st);

    st->hList = CreateWindowExW(
        0, L"SqlD2DGrid", L"", WS_CHILD | WS_VISIBLE | WS_HSCROLL | WS_VSCROLL, 0, 0, 0, 0,
        st->hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LIST)), hInst, nullptr);
    if (th.dark) SetWindowTheme(st->hList, L"DarkMode_Explorer", nullptr);

    st->hSplitter = CreateWindowExW(0, kSplitterClass, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0,
                                    st->hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SPLIT)),
                                    hInst, nullptr);
    SetWindowLongPtrW(st->hSplitter, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));

    st->hTree = CreateWindowExW(
        0, WC_TREEVIEW, L"",
        WS_CHILD | WS_VISIBLE | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS,
        0, 0, 0, 0, st->hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TREE)), hInst, nullptr);
    TreeView_SetBkColor(st->hTree, th.panelBg);
    TreeView_SetTextColor(st->hTree, th.textPrimary);
    if (th.dark) SetWindowTheme(st->hTree, L"DarkMode_Explorer", nullptr);
    SendMessageW(st->hTree, TVM_SETEXTENDEDSTYLE, TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);
    SendMessageW(st->hTree, WM_SETFONT, reinterpret_cast<WPARAM>(st->hUi), TRUE);

    st->hVSplitter = CreateWindowExW(0, kVSplitterClass, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0,
                                     st->hwnd,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_VSPLIT)),
                                     hInst, nullptr);
    SetWindowLongPtrW(st->hVSplitter, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));

    st->hEdit = CreateWindowExW(
        0, L"SqlD2DEditor", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL, 0, 0, 0, 0, st->hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EDIT)), hInst, nullptr);
    if (th.dark) SetWindowTheme(st->hEdit, L"DarkMode_Explorer", nullptr);
    SetWindowTextW(st->hEdit, L"SELECT * FROM users;");

    st->hStatus = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0,
                                  st->hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STATUS)),
                                  hInst, nullptr);
    SetWindowSubclass(st->hStatus, StatusSubclassProc, 2, reinterpret_cast<DWORD_PTR>(st));

    st->hCmdBar = CreateWindowExW(0, kCmdBarClass, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, st->hwnd,
                                  nullptr, hInst, nullptr);
    SetWindowLongPtrW(st->hCmdBar, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));

    setStatus(st, L"Connect a database to begin — Ctrl+O");
}

// ---- dark popup menus -------------------------------------------------------
// uxtheme's dark-mode entry points are exported by ordinal only (undocumented,
// but stable since Win10 1809 and the standard way apps get dark menus). Every
// call is guarded — if the export is missing, menus simply stay light.
void enableDarkMenus() {
    HMODULE ux = GetModuleHandleW(L"uxtheme.dll");
    if (!ux) return;
    using SetPreferredAppModeFn = int(WINAPI*)(int);
    using FlushMenuThemesFn = void(WINAPI*)();
    auto setMode = reinterpret_cast<SetPreferredAppModeFn>(GetProcAddress(ux, MAKEINTRESOURCEA(135)));
    auto flush = reinterpret_cast<FlushMenuThemesFn>(GetProcAddress(ux, MAKEINTRESOURCEA(136)));
    if (setMode) setMode(1);  // AllowDark: menus follow the system theme
    if (flush) flush();
}

void allowDarkForWindow(HWND hwnd) {
    HMODULE ux = GetModuleHandleW(L"uxtheme.dll");
    if (!ux) return;
    using AllowDarkFn = BOOL(WINAPI*)(HWND, BOOL);
    auto allow = reinterpret_cast<AllowDarkFn>(GetProcAddress(ux, MAKEINTRESOURCEA(133)));
    if (allow) allow(hwnd, TRUE);
}

// The full menu now lives behind the command-bar hamburger as a popup.
HMENU buildMenuPopup(AppState* st) {
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, ID_NEW, L"New Window\tCtrl+N");
    AppendMenuW(file, MF_STRING, ID_OPEN, L"Open Database…\tCtrl+O");
    AppendMenuW(file, MF_STRING, ID_CONN_DETAILS, L"Connection Details…");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, ID_CLOSE, L"Close Window\tCtrl+W");
    AppendMenuW(file, MF_STRING, ID_EXIT, L"Exit");

    HMENU query = CreatePopupMenu();
    AppendMenuW(query, MF_STRING, ID_RUN, L"Run\tCtrl+E");
    AppendMenuW(query, MF_STRING, ID_RUN_STMT, L"Run Statement at Cursor\tCtrl+Enter");
    AppendMenuW(query, MF_STRING, ID_CANCEL, L"Cancel Running Query\tCtrl+.");
    AppendMenuW(query, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(query, MF_STRING | (st->readOnly ? MF_CHECKED : MF_UNCHECKED), ID_READONLY,
                L"Read-only mode");
    AppendMenuW(query, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(query, MF_STRING, ID_TX_BEGIN, L"Begin Transaction");
    AppendMenuW(query, MF_STRING, ID_TX_COMMIT, L"Commit Transaction");
    AppendMenuW(query, MF_STRING, ID_TX_ROLLBACK, L"Rollback Transaction");
    AppendMenuW(query, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(query, MF_STRING, ID_REFRESH_SCHEMA, L"Refresh Schema");
    AppendMenuW(query, MF_STRING, ID_HISTORY, L"History && Snippets…\tCtrl+R");

    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, ID_CHECK_UPDATES, L"Check for Updates…");
    AppendMenuW(help, MF_STRING, ID_HELP, L"SQLTerminal Help\tF1");
    AppendMenuW(help, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(help, MF_STRING, ID_ABOUT, L"About SQLTerminal");

    HMENU view = CreatePopupMenu();
    AppendMenuW(view, MF_STRING, ID_TOGGLE_THEME, L"Toggle light / dark theme");

    HMENU root = CreatePopupMenu();
    AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"File");
    AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(query), L"Query");
    AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"View");
    AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(help), L"Help");
    return root;
}

void showMainMenu(AppState* st) {
    HMENU m = buildMenuPopup(st);
    RECT rc;
    GetWindowRect(st->hCmdBar, &rc);
    TrackPopupMenu(m, TPM_LEFTBUTTON, rc.left + 8, rc.bottom, 0, st->hwnd, nullptr);
    DestroyMenu(m);  // also frees the appended submenus
}

// ---- command bar ------------------------------------------------------------
struct CmdItem {
    int id;
    const wchar_t* glyph;   // Segoe MDL2 Assets codepoint
    const wchar_t* label;   // non-null = wider labelled (primary) button
    bool accent;            // coral pill
};
const CmdItem kCmdItems[] = {
    {ID_MENU, L"", nullptr, false},
    {ID_RUN, L"", L"Run", true},
    {ID_OPEN, L"", nullptr, false},
    {ID_REFRESH_SCHEMA, L"", nullptr, false},
    {ID_HISTORY, L"", nullptr, false},
};
constexpr int kCmdCount = static_cast<int>(sizeof(kCmdItems) / sizeof(kCmdItems[0]));

void cmdRects(HWND bar, RECT btn[kCmdCount], RECT* chip, RECT cap[3]) {
    auto* st = reinterpret_cast<AppState*>(GetWindowLongPtrW(bar, GWLP_USERDATA));
    const int d = st ? st->dpi : 96;
    RECT rc;
    GetClientRect(bar, &rc);
    int x = dp(10, d);
    for (int i = 0; i < kCmdCount; ++i) {
        const int w = kCmdItems[i].label ? dp(78, d) : dp(38, d);
        btn[i] = {x, dp(7, d), x + w, rc.bottom - dp(8, d)};
        x += w + dp(6, d);
    }
    // Caption buttons (min / max / close), full height, flush to the right edge.
    const int capW = dp(46, d);
    for (int i = 0; i < 3; ++i)
        cap[i] = {rc.right - capW * (3 - i), 0, rc.right - capW * (2 - i), rc.bottom};
    const int chipW = dp(196, d);
    const int chipRight = rc.right - capW * 3 - dp(8, d);
    *chip = {chipRight - chipW, dp(8, d), chipRight, rc.bottom - dp(9, d)};
}

void fillRound(HDC dc, const RECT& r, COLORREF fill, COLORREF edge, int radius) {
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = edge ? CreatePen(PS_SOLID, 1, edge) : nullptr;
    HGDIOBJ ob = SelectObject(dc, b);
    HGDIOBJ op = SelectObject(dc, p ? p : GetStockObject(NULL_PEN));
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    if (p) DeleteObject(p);
}

LRESULT CALLBACK CmdBarProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* st = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            HDC dc = CreateCompatibleDC(hdc);
            HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
            HGDIOBJ obmp = SelectObject(dc, bmp);
            const Theme& th = currentTheme();

            HBRUSH bg = CreateSolidBrush(th.panelElevBg);
            FillRect(dc, &rc, bg);
            DeleteObject(bg);
            HPEN line = CreatePen(PS_SOLID, 1, th.border);
            HGDIOBJ ol = SelectObject(dc, line);
            MoveToEx(dc, 0, rc.bottom - 1, nullptr);
            LineTo(dc, rc.right, rc.bottom - 1);
            SelectObject(dc, ol);
            DeleteObject(line);
            SetBkMode(dc, TRANSPARENT);

            RECT btn[kCmdCount];
            RECT chip;
            RECT cap[3];
            cmdRects(hwnd, btn, &chip, cap);
            const bool busy = st && st->busy;
            for (int i = 0; i < kCmdCount; ++i) {
                const CmdItem& it = kCmdItems[i];
                const bool hov = st && st->cmdHover == i;
                if (it.accent)
                    fillRound(dc, btn[i], busy ? th.hoverBg : th.accent, 0, 8);
                else if (hov)
                    fillRound(dc, btn[i], th.hoverBg, 0, 8);

                const COLORREF fg = it.accent ? (busy ? th.textMuted : th.accentText)
                                              : (hov ? th.textPrimary : th.textSecondary);
                RECT gr = btn[i];
                if (it.label) gr.right = gr.left + dp(30, st->dpi);
                SelectObject(dc, st->hGlyph);
                SetTextColor(dc, fg);
                DrawTextW(dc, it.glyph, -1, &gr, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
                if (it.label) {
                    RECT lr = btn[i];
                    lr.left += dp(30, st->dpi);
                    SelectObject(dc, st->hUi);
                    SetTextColor(dc, fg);
                    DrawTextW(dc, it.label, -1, &lr, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
                }
            }

            const bool connected = st && st->session.isConnected();
            fillRound(dc, chip, th.panelBg, th.border, 8);
            const int cy = (chip.top + chip.bottom) / 2;
            HBRUSH db = CreateSolidBrush(connected ? RGB(61, 220, 132) : th.textMuted);
            HGDIOBJ odb = SelectObject(dc, db);
            HGDIOBJ opn = SelectObject(dc, GetStockObject(NULL_PEN));
            Ellipse(dc, chip.left + dp(12, st->dpi), cy - dp(4, st->dpi), chip.left + dp(20, st->dpi),
                    cy + dp(4, st->dpi));
            SelectObject(dc, odb);
            SelectObject(dc, opn);
            DeleteObject(db);
            RECT cr = chip;
            cr.left += dp(28, st->dpi);
            cr.right -= dp(10, st->dpi);
            SelectObject(dc, st->hUi);
            SetTextColor(dc, connected ? th.textPrimary : th.textSecondary);
            const std::wstring chipText =
                connected ? (st->dbName.empty() ? L"connected" : st->dbName) : L"Connect a database…";
            DrawTextW(dc, chipText.c_str(), -1, &cr,
                      DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);

            // Caption buttons: minimize / maximize-restore / close.
            const bool zoomed = IsZoomed(st->hwnd) != 0;
            for (int i = 0; i < 3; ++i) {
                const bool chov = st && st->capHover == i;
                const bool isClose = (i == 2);
                if (chov) {
                    HBRUSH hb = CreateSolidBrush(isClose ? RGB(196, 43, 28) : th.hoverBg);
                    FillRect(dc, &cap[i], hb);
                    DeleteObject(hb);
                }
                const COLORREF gc = (chov && isClose) ? RGB(255, 255, 255)
                                                      : (chov ? th.textPrimary : th.textSecondary);
                HPEN gp = CreatePen(PS_SOLID, (std::max)(1, dp(1, st->dpi)), gc);
                HGDIOBJ ogp = SelectObject(dc, gp);
                HGDIOBJ obr = SelectObject(dc, GetStockObject(NULL_BRUSH));
                const int mx = (cap[i].left + cap[i].right) / 2;
                const int my = (cap[i].top + cap[i].bottom) / 2;
                const int s = dp(5, st->dpi);
                if (i == 0) {  // minimize
                    MoveToEx(dc, mx - s, my, nullptr);
                    LineTo(dc, mx + s + 1, my);
                } else if (i == 1) {  // maximize / restore
                    if (zoomed) {
                        Rectangle(dc, mx - s, my - s + 2, mx + s - 2, my + s);
                        MoveToEx(dc, mx - s + 2, my - s + 2, nullptr);
                        LineTo(dc, mx - s + 2, my - s);
                        LineTo(dc, mx + s, my - s);
                        LineTo(dc, mx + s, my + s - 2);
                    } else {
                        Rectangle(dc, mx - s, my - s, mx + s, my + s);
                    }
                } else {  // close
                    MoveToEx(dc, mx - s, my - s, nullptr);
                    LineTo(dc, mx + s + 1, my + s + 1);
                    MoveToEx(dc, mx + s, my - s, nullptr);
                    LineTo(dc, mx - s - 1, my + s + 1);
                }
                SelectObject(dc, ogp);
                SelectObject(dc, obr);
                DeleteObject(gp);
            }

            BitBlt(hdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
            SelectObject(dc, obmp);
            DeleteObject(bmp);
            DeleteDC(dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_MOUSEMOVE: {
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            RECT btn[kCmdCount];
            RECT chip;
            RECT cap[3];
            cmdRects(hwnd, btn, &chip, cap);
            int hov = -1;
            for (int i = 0; i < kCmdCount; ++i)
                if (PtInRect(&btn[i], pt)) hov = i;
            int chov = -1;
            for (int i = 0; i < 3; ++i)
                if (PtInRect(&cap[i], pt)) chov = i;
            if (st && (hov != st->cmdHover || chov != st->capHover)) {
                st->cmdHover = hov;
                st->capHover = chov;
                InvalidateRect(hwnd, nullptr, FALSE);
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            if (st && (st->cmdHover != -1 || st->capHover != -1)) {
                st->cmdHover = -1;
                st->capHover = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_SETCURSOR: {
            // Show the resize cursor over the thin top edge (above the buttons).
            if (st && LOWORD(lParam) == HTCLIENT && !IsZoomed(st->hwnd)) {
                POINT cp;
                GetCursorPos(&cp);
                ScreenToClient(hwnd, &cp);
                RECT btn[kCmdCount];
                RECT chip;
                RECT cap[3];
                cmdRects(hwnd, btn, &chip, cap);
                bool onCap = false;
                for (int i = 0; i < 3; ++i)
                    if (PtInRect(&cap[i], cp)) onCap = true;
                if (cp.y < dp(5, st->dpi) && !onCap) {
                    SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
                    return TRUE;
                }
            }
            break;
        }
        case WM_LBUTTONDOWN: {
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            RECT btn[kCmdCount];
            RECT chip;
            RECT cap[3];
            cmdRects(hwnd, btn, &chip, cap);
            for (int i = 0; i < kCmdCount; ++i) {
                if (PtInRect(&btn[i], pt)) {
                    SendMessageW(st->hwnd, WM_COMMAND, MAKEWPARAM(kCmdItems[i].id, 0), 0);
                    return 0;
                }
            }
            if (PtInRect(&chip, pt)) {
                SendMessageW(st->hwnd, WM_COMMAND, MAKEWPARAM(ID_OPEN, 0), 0);
                return 0;
            }
            if (PtInRect(&cap[0], pt)) {
                SendMessageW(st->hwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);
                return 0;
            }
            if (PtInRect(&cap[1], pt)) {
                SendMessageW(st->hwnd, WM_SYSCOMMAND, IsZoomed(st->hwnd) ? SC_RESTORE : SC_MAXIMIZE,
                             0);
                return 0;
            }
            if (PtInRect(&cap[2], pt)) {
                SendMessageW(st->hwnd, WM_CLOSE, 0, 0);
                return 0;
            }
            // Thin top edge (above the buttons): forward a top-edge resize.
            if (pt.y < dp(5, st->dpi) && !IsZoomed(st->hwnd)) {
                ReleaseCapture();
                SendMessageW(st->hwnd, WM_NCLBUTTONDOWN, HTTOP, 0);
                return 0;
            }
            // Empty area: only begin a window drag once the pointer actually moves,
            // so a plain click falls through and WM_LBUTTONDBLCLK (maximize) can fire.
            POINT scr = pt;
            ClientToScreen(hwnd, &scr);
            if (DragDetect(hwnd, scr)) {
                ReleaseCapture();
                SendMessageW(st->hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            }
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            RECT btn[kCmdCount];
            RECT chip;
            RECT cap[3];
            cmdRects(hwnd, btn, &chip, cap);
            bool onControl = PtInRect(&chip, pt) != 0;
            for (int i = 0; i < kCmdCount; ++i)
                if (PtInRect(&btn[i], pt)) onControl = true;
            for (int i = 0; i < 3; ++i)
                if (PtInRect(&cap[i], pt)) onControl = true;
            if (!onControl)
                SendMessageW(st->hwnd, WM_SYSCOMMAND, IsZoomed(st->hwnd) ? SC_RESTORE : SC_MAXIMIZE,
                             0);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Fully custom-paint the status bar (double-buffered): theme background, a top
// border line, the left message and the right status pills. Replaces the comctl
// owner-draw + etch line so there's no white outline and no resize flicker.
LRESULT CALLBACK StatusSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR,
                                    DWORD_PTR ref) {
    auto* st = reinterpret_cast<AppState*>(ref);
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, StatusSubclassProc, 2);
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_PAINT && st) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HDC dc = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HGDIOBJ obmp = SelectObject(dc, bmp);
        const Theme& th = currentTheme();
        const int d = st->dpi;

        FillRect(dc, &rc, themeBrush(th.panelElevBg));
        HPEN pen = CreatePen(PS_SOLID, 1, th.border);
        HGDIOBJ op = SelectObject(dc, pen);
        MoveToEx(dc, 0, 0, nullptr);
        LineTo(dc, rc.right, 0);
        SelectObject(dc, op);
        DeleteObject(pen);
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, st->hUi);

        RECT lr = rc;
        lr.left += dp(12, d);
        lr.right -= dp(210, d);
        SetTextColor(dc, th.textPrimary);
        DrawTextW(dc, st->statusMsg.c_str(), -1, &lr,
                  DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);

        struct Flag {
            bool on;
            const wchar_t* text;
            COLORREF fg;
        };
        const Flag flags[] = {
            {st->sslActive, L"SSL", RGB(120, 190, 255)},
            {st->readOnly, L"read-only", RGB(240, 181, 97)},
            {st->inTransaction, L"in transaction", th.accent},
        };
        int right = rc.right - dp(12, d);
        for (int i = static_cast<int>(sizeof(flags) / sizeof(flags[0])) - 1; i >= 0; --i) {
            if (!flags[i].on) continue;
            SIZE sz{};
            GetTextExtentPoint32W(dc, flags[i].text, lstrlenW(flags[i].text), &sz);
            const int w = sz.cx + dp(18, d);
            const int h = dp(18, d);
            const int midY = (rc.top + rc.bottom) / 2;
            RECT pr{right - w, midY - h / 2, right, midY + h / 2};
            HBRUSH pb = CreateSolidBrush(th.panelBg);
            HGDIOBJ ob = SelectObject(dc, pb);
            HGDIOBJ opn = SelectObject(dc, GetStockObject(NULL_PEN));
            RoundRect(dc, pr.left, pr.top, pr.right, pr.bottom, dp(9, d), dp(9, d));
            SelectObject(dc, ob);
            SelectObject(dc, opn);
            DeleteObject(pb);
            SetTextColor(dc, flags[i].fg);
            DrawTextW(dc, flags[i].text, -1, &pr, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
            right -= w + dp(6, d);
        }

        BitBlt(hdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, obmp);
        DeleteObject(bmp);
        DeleteDC(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK SplitterProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* st = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            // The rounded card edges (parent-painted) are the divider now; the
            // splitter just fills the gutter so it reads as one clean seam. No
            // center line — it would triple up with the two adjacent card edges.
            FillRect(dc, &rc, themeBrush(currentTheme().windowBg));
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN:
            SetCapture(hwnd);
            return 0;
        case WM_MOUSEMOVE:
            if (st && (wParam & MK_LBUTTON) && GetCapture() == hwnd) {
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(st->hwnd, &pt);
                RECT rc;
                GetClientRect(st->hwnd, &rc);
                const int d = st->dpi;
                const int splitH = dp(kSplitterHeight, d);
                const int minEdit = dp(kMinEditor, d);
                const int contentBottom = static_cast<int>(rc.bottom) - statusHeight(st);
                const int contentH = (std::max)(0, contentBottom - dp(kCmdBarH, d));
                int newEdit = contentBottom - splitH - pt.y;
                int maxEdit = contentH - dp(kMinList, d) - splitH;
                if (maxEdit < minEdit) maxEdit = minEdit;
                if (newEdit < minEdit) newEdit = minEdit;
                if (newEdit > maxEdit) newEdit = maxEdit;
                st->editorHeight = newEdit;
                layout(st);
                RedrawWindow(st->hwnd, nullptr, nullptr,
                             RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_NOERASE);
            }
            return 0;
        case WM_LBUTTONUP:
            if (GetCapture() == hwnd) ReleaseCapture();
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK VSplitterProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* st = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            // The rounded card edges (parent-painted) are the divider now; the
            // splitter just fills the gutter so it reads as one clean seam. No
            // center line — it would triple up with the two adjacent card edges.
            FillRect(dc, &rc, themeBrush(currentTheme().windowBg));
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN:
            SetCapture(hwnd);
            return 0;
        case WM_MOUSEMOVE:
            if (st && (wParam & MK_LBUTTON) && GetCapture() == hwnd) {
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(st->hwnd, &pt);
                RECT rc;
                GetClientRect(st->hwnd, &rc);
                const int d = st->dpi;
                const int minW = dp(120, d);
                int newW = pt.x;
                int maxW = rc.right - dp(200, d) - dp(kSplitterHeight, d);
                if (maxW < minW) maxW = minW;
                if (newW < minW) newW = minW;
                if (newW > maxW) newW = maxW;
                st->sidebarWidth = newW;
                layout(st);
                RedrawWindow(st->hwnd, nullptr, nullptr,
                             RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_NOERASE);
            }
            return 0;
        case WM_LBUTTONUP:
            if (GetCapture() == hwnd) ReleaseCapture();
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---- schema sidebar ---------------------------------------------------------

std::wstring treeItemText(HWND tree, HTREEITEM node) {
    wchar_t buf[512] = L"";
    TVITEMW it{};
    it.mask = TVIF_TEXT;
    it.hItem = node;
    it.pszText = buf;
    it.cchTextMax = 512;
    TreeView_GetItem(tree, &it);
    return buf;
}

void copyToClipboard(HWND hwnd, const std::wstring& text) {
    if (!OpenClipboard(hwnd)) return;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* p = GlobalLock(h)) {
            std::memcpy(p, text.c_str(), bytes);
            GlobalUnlock(h);
            SetClipboardData(CF_UNICODETEXT, h);
        }
    }
    CloseClipboard();
}

void fetchTablesAsync(AppState* st) {
    if (!st->session.isConnected()) return;
    HWND hwnd = st->hwnd;
    st->session.executeUncancellableAsync(tableNamesSql(st->currentEngine), [hwnd](QueryResult r) {
        auto* m = new TablesMsg{};
        if (!r.error)
            for (const auto& row : r.rows)
                if (!row.empty()) m->tables.push_back(row[0]);
        if (!PostMessageW(hwnd, WM_APP_TABLES, 0, reinterpret_cast<LPARAM>(m))) delete m;
    });
}

void populateTables(AppState* st, const std::vector<std::wstring>& tables) {
    TreeView_DeleteAllItems(st->hTree);
    for (const auto& t : tables) {
        TVINSERTSTRUCTW ins{};
        ins.hParent = TVI_ROOT;
        ins.hInsertAfter = TVI_LAST;
        ins.item.mask = TVIF_TEXT | TVIF_PARAM;
        ins.item.pszText = const_cast<LPWSTR>(t.c_str());
        ins.item.lParam = 0;  // table, columns not yet loaded
        HTREEITEM node = TreeView_InsertItem(st->hTree, &ins);
        // A placeholder child so the [+] expander shows; replaced on expand.
        TVINSERTSTRUCTW dummy{};
        dummy.hParent = node;
        dummy.hInsertAfter = TVI_LAST;
        dummy.item.mask = TVIF_TEXT | TVIF_PARAM;
        dummy.item.pszText = const_cast<LPWSTR>(L"(loading…)");
        dummy.item.lParam = 2;
        TreeView_InsertItem(st->hTree, &dummy);
    }
}

void fetchColumnsForNode(AppState* st, HTREEITEM node) {
    const std::wstring table = treeItemText(st->hTree, node);
    const DatabaseEngine engine = st->currentEngine;
    HWND hwnd = st->hwnd;
    st->session.executeUncancellableAsync(columnsSql(engine, table), [hwnd, node, engine](QueryResult r) {
        auto* m = new ColumnsMsg{};
        m->node = node;
        if (!r.error) {
            for (const auto& row : r.rows) {
                std::wstring name, type;
                if (engine == DatabaseEngine::Postgres) {
                    name = row.size() > 0 ? row[0] : L"";
                    type = row.size() > 1 ? row[1] : L"";
                } else {  // SQLite PRAGMA table_info: cid, name, type, ...
                    name = row.size() > 1 ? row[1] : L"";
                    type = row.size() > 2 ? row[2] : L"";
                }
                m->lines.push_back(type.empty() ? name : (name + L"  :  " + type));
            }
        }
        if (!PostMessageW(hwnd, WM_APP_COLUMNS, 0, reinterpret_cast<LPARAM>(m))) delete m;
    });
}

void populateColumns(AppState* st, HTREEITEM node, const std::vector<std::wstring>& lines) {
    TVITEMW mark{};
    mark.mask = TVIF_PARAM;
    mark.hItem = node;
    mark.lParam = 1;  // loaded
    TreeView_SetItem(st->hTree, &mark);

    HTREEITEM child = TreeView_GetChild(st->hTree, node);
    while (child) {
        HTREEITEM next = TreeView_GetNextSibling(st->hTree, child);
        TreeView_DeleteItem(st->hTree, child);
        child = next;
    }
    const std::vector<std::wstring> shown = lines.empty() ? std::vector<std::wstring>{L"(no columns)"} : lines;
    for (const auto& line : shown) {
        TVINSERTSTRUCTW ins{};
        ins.hParent = node;
        ins.hInsertAfter = TVI_LAST;
        ins.item.mask = TVIF_TEXT | TVIF_PARAM;
        ins.item.pszText = const_cast<LPWSTR>(line.c_str());
        ins.item.lParam = 2;
        TreeView_InsertItem(st->hTree, &ins);
    }
    TreeView_Expand(st->hTree, node, TVE_EXPAND);
}

void onTreeExpand(AppState* st, NMTREEVIEWW* nm) {
    if (nm->action != TVE_EXPAND) return;
    TVITEMW q{};
    q.mask = TVIF_PARAM;
    q.hItem = nm->itemNew.hItem;
    TreeView_GetItem(st->hTree, &q);
    if (q.lParam != 0) return;  // already loaded or loading
    q.lParam = 3;               // mark loading
    TreeView_SetItem(st->hTree, &q);
    fetchColumnsForNode(st, nm->itemNew.hItem);
}

void onTreeDoubleClick(AppState* st) {
    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(st->hTree, &pt);
    TVHITTESTINFO ht{};
    ht.pt = pt;
    HTREEITEM item = TreeView_HitTest(st->hTree, &ht);
    if (item && !TreeView_GetParent(st->hTree, item)) {  // a table (top-level)
        SetWindowTextW(st->hEdit, selectStatementFor(treeItemText(st->hTree, item)).c_str());
        SetFocus(st->hEdit);
    }
}

void onTreeContextMenu(AppState* st) {
    POINT screen;
    GetCursorPos(&screen);
    POINT pt = screen;
    ScreenToClient(st->hTree, &pt);
    TVHITTESTINFO ht{};
    ht.pt = pt;
    HTREEITEM item = TreeView_HitTest(st->hTree, &ht);
    if (!item || TreeView_GetParent(st->hTree, item)) return;  // tables only
    TreeView_SelectItem(st->hTree, item);
    st->contextTable = treeItemText(st->hTree, item);
    HMENU pm = CreatePopupMenu();
    AppendMenuW(pm, MF_STRING, ID_CTX_INSERT, L"Insert SELECT");
    AppendMenuW(pm, MF_STRING, ID_CTX_PREVIEW, L"Preview rows (LIMIT 100)");
    AppendMenuW(pm, MF_STRING, ID_CTX_COPY, L"Copy name");
    TrackPopupMenu(pm, TPM_RIGHTBUTTON, screen.x, screen.y, 0, st->hwnd, nullptr);
    DestroyMenu(pm);
}

HWND createMainWindow(int nCmdShow) {
    auto* state = new AppState();
    const int sysDpi = static_cast<int>(GetDpiForSystem());
    HWND hwnd = CreateWindowExW(0, kClassName, L"SQLTerminal", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, MulDiv(1100, sysDpi, 96),
                                MulDiv(750, sysDpi, 96), nullptr, nullptr, g_appInstance, state);
    if (!hwnd) {
        delete state;
        return nullptr;
    }
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    return hwnd;
}

// ---- About dialog (custom, dark, app icon + build number) -------------------
struct AboutState {
    UINT dpi = 96;
    bool done = false;
    RECT githubRect{};  // "GitHub" link bounds (client px) for cursor + click hit-testing
    RECT issueRect{};   // "Report Issue" link bounds (client px)
};

LRESULT CALLBACK AboutProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* st = reinterpret_cast<AboutState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            return dialogCtlColor(msg, wParam);
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            const Theme& th = currentTheme();
            const int d = st->dpi;
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, themeBrush(th.panelBg));
            HICON ic = LoadIconW(g_appInstance, MAKEINTRESOURCEW(IDI_APPICON));
            DrawIconEx(hdc, dpiScale(22, d), dpiScale(22, d), ic, dpiScale(56, d), dpiScale(56, d), 0,
                       nullptr, DI_NORMAL);
            SetBkMode(hdc, TRANSPARENT);
            HFONT title = CreateFontW(-dpiScale(19, d), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                      CLEARTYPE_QUALITY, VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
            HFONT body = CreateFontW(-dpiScale(14, d), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                     CLEARTYPE_QUALITY, VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
            auto line = [&](HFONT f, COLORREF c, const wchar_t* s, int x, int y) {
                SelectObject(hdc, f);
                SetTextColor(hdc, c);
                RECT r{dpiScale(x, d), dpiScale(y, d), rc.right - dpiScale(14, d), rc.bottom};
                DrawTextW(hdc, s, -1, &r, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            };
            line(title, th.textPrimary, L"SQLTerminal (Win32)", 92, 24);
            line(body, th.accent, L"Version " SQLT_VERSION_DISPLAY_W, 92, 52);
            line(body, th.textPrimary, L"A native Windows SQL terminal for SQLite and PostgreSQL.", 22,
                 96);
            line(body, th.textSecondary, L"Licensed under GPL-3.0.", 22, 120);
            // Clickable links (accent + underline); store bounds for cursor + click.
            HFONT link = CreateFontW(-dpiScale(14, d), 0, 0, 0, FW_NORMAL, FALSE, TRUE, FALSE,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                     CLEARTYPE_QUALITY, VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
            SelectObject(hdc, link);
            SetTextColor(hdc, th.accent);
            auto drawLink = [&](const wchar_t* s, int px, int py) -> RECT {
                SIZE sz;
                GetTextExtentPoint32W(hdc, s, lstrlenW(s), &sz);
                TextOutW(hdc, px, py, s, lstrlenW(s));
                return RECT{px, py, px + sz.cx, py + sz.cy};
            };
            const int linkY = dpiScale(150, d);
            st->githubRect = drawLink(L"GitHub", dpiScale(22, d), linkY);
            st->issueRect = drawLink(L"Report Issue", st->githubRect.right + dpiScale(18, d), linkY);
            line(body, th.textSecondary, L"for Daniel Kenny and Bryan Mark", 22, 178);
            SelectObject(hdc, GetStockObject(SYSTEM_FONT));
            DeleteObject(title);
            DeleteObject(body);
            DeleteObject(link);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_SETCURSOR:
            if (st && LOWORD(lParam) == HTCLIENT) {
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                if (PtInRect(&st->githubRect, pt) || PtInRect(&st->issueRect, pt)) {
                    SetCursor(LoadCursorW(nullptr, IDC_HAND));
                    return TRUE;
                }
            }
            break;
        case WM_LBUTTONDOWN:
            if (st) {
                POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                if (PtInRect(&st->githubRect, pt))
                    ShellExecuteW(hwnd, L"open", L"https://github.com/arcanii/SQLTerminal-Win32",
                                  nullptr, nullptr, SW_SHOWNORMAL);
                else if (PtInRect(&st->issueRect, pt))
                    ShellExecuteW(hwnd, L"open",
                                  L"https://github.com/arcanii/SQLTerminal-Win32/issues", nullptr,
                                  nullptr, SW_SHOWNORMAL);
            }
            return 0;
        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) st->done = true;
            return 0;
        case WM_CLOSE:
            st->done = true;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void doAbout(HWND owner) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = AboutProc;
        wc.hInstance = g_appInstance;
        wc.lpszClassName = L"SQLTerminalAbout";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = themeBrush(currentTheme().panelBg);
        RegisterClassExW(&wc);
        registered = true;
    }

    AboutState st;
    const int W = 440, H = 244;
    const UINT odpi = GetDpiForWindow(owner);
    RECT orc{};
    GetWindowRect(owner, &orc);
    const int fullW = dpiScale(W + 16, odpi), fullH = dpiScale(H + 39, odpi);
    const int x = orc.left + ((orc.right - orc.left) - fullW) / 2;
    const int y = orc.top + ((orc.bottom - orc.top) - fullH) / 2;
    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"SQLTerminalAbout", L"About SQLTerminal",
                                WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, fullW, fullH, owner, nullptr,
                                g_appInstance, &st);
    if (!hwnd) return;
    st.dpi = GetDpiForWindow(hwnd);
    HFONT okFont = CreateFontW(-dpiScale(14, st.dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
    HWND ok = CreateWindowExW(0, L"BUTTON", L"OK",
                              WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP,
                              dpiScale(W - 100, st.dpi), dpiScale(H - 44, st.dpi), dpiScale(88, st.dpi),
                              dpiScale(28, st.dpi), hwnd,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), g_appInstance,
                              nullptr);
    SendMessageW(ok, WM_SETFONT, reinterpret_cast<WPARAM>(okFont), TRUE);
    applyDialogDarkMode(hwnd);
    EnableWindow(owner, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (!st.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    DestroyWindow(hwnd);
    DeleteObject(okFont);
}

void doHelp(HWND hwnd) {
    showInfoDialog(hwnd, L"SQLTerminal Help",
                   L"Keyboard shortcuts\n"
                   L"  Ctrl+E          Run the whole editor\n"
                   L"  Ctrl+Enter      Run the statement at the cursor\n"
                   L"  Ctrl+.          Cancel the running query\n"
                   L"  Ctrl+Up/Down    Previous / next command\n"
                   L"  Ctrl+O          Open / connect\n"
                   L"  Ctrl+R          History & snippets\n"
                   L"  Ctrl+N          New window\n"
                   L"  Ctrl+W          Close window\n"
                   L"  F1              This help\n\n"
                   L"Type .help in the editor for SQL dot-commands (.tables, .schema, …).");
}

// Force popup/context menus to the given mode (uxtheme ordinals; guarded).
void setMenuMode(bool dark) {
    HMODULE ux = GetModuleHandleW(L"uxtheme.dll");
    if (!ux) return;
    using SetModeFn = int(WINAPI*)(int);
    using FlushFn = void(WINAPI*)();
    auto setMode = reinterpret_cast<SetModeFn>(GetProcAddress(ux, MAKEINTRESOURCEA(135)));
    auto flush = reinterpret_cast<FlushFn>(GetProcAddress(ux, MAKEINTRESOURCEA(136)));
    if (setMode) setMode(dark ? 2 : 3);  // ForceDark / ForceLight
    if (flush) flush();
}

// Re-apply the current theme to every live control (for the in-app toggle).
void reapplyTheme(AppState* st) {
    const Theme& th = currentTheme();
    const wchar_t* explorerTheme = th.dark ? L"DarkMode_Explorer" : L"Explorer";

    applyDarkTitleBar(st->hwnd);
    setMenuMode(th.dark);

    gridApplyTheme(st->hList);

    TreeView_SetBkColor(st->hTree, th.panelBg);
    TreeView_SetTextColor(st->hTree, th.textPrimary);
    SetWindowTheme(st->hTree, explorerTheme, nullptr);
    InvalidateRect(st->hTree, nullptr, TRUE);

    editorApplyTheme(st->hEdit);  // rebuild D2D brushes + scrollbar theme, repaint

    InvalidateRect(st->hCmdBar, nullptr, TRUE);
    InvalidateRect(st->hStatus, nullptr, TRUE);
    InvalidateRect(st->hSplitter, nullptr, TRUE);
    InvalidateRect(st->hVSplitter, nullptr, TRUE);
    InvalidateRect(st->hwnd, nullptr, TRUE);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* st = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            auto* state = reinterpret_cast<AppState*>(cs->lpCreateParams);
            state->hwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            ++g_windowCount;
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        case WM_CREATE: {
            st->dpi = static_cast<int>(GetDpiForWindow(hwnd));
            st->sidebarWidth = dp(st->sidebarWidth, st->dpi);
            st->editorHeight = dp(st->editorHeight, st->dpi);
            allowDarkForWindow(hwnd);
            createChildren(st, reinterpret_cast<LPCREATESTRUCTW>(lParam)->hInstance);
            layout(st);
            applyDarkTitleBar(hwnd);
            DWORD corner = 2;  // DWMWCP_ROUND
            DwmSetWindowAttribute(hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &corner, sizeof(corner));
            SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                         SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        // Custom title bar: drop the standard caption, keep the resize frame. The
        // command bar paints the caption + window buttons and drives drag/min/max/close.
        case WM_NCCALCSIZE: {
            if (!wParam) break;  // fall through to DefWindowProc
            auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
            const UINT dpi = GetDpiForWindow(hwnd);
            const int frameX = GetSystemMetricsForDpi(SM_CXFRAME, dpi) +
                               GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            const int frameY = GetSystemMetricsForDpi(SM_CYFRAME, dpi) +
                               GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            RECT& r = params->rgrc[0];
            r.left += frameX;
            r.right -= frameX;
            r.bottom -= frameY;
            if (IsZoomed(hwnd)) r.top += frameY;  // maximized: avoid top clip
            return 0;                             // else: reclaim the caption area
        }
        case WM_SETTINGCHANGE:
            applyDarkTitleBar(hwnd);  // react to light/dark toggle
            return 0;
        case WM_SIZE:
            if (st) layout(st);
            return 0;
        case WM_ERASEBKGND:
            return 1;  // background + rounded pane frames are painted in WM_PAINT
        case WM_PAINT: {
            // Paint the window backdrop and a rounded hairline frame around each
            // pane (in the inset margins; WS_CLIPCHILDREN keeps it off the children).
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            HDC dc = CreateCompatibleDC(hdc);
            HBITMAP bmp =
                CreateCompatibleBitmap(hdc, (std::max)(1L, rc.right), (std::max)(1L, rc.bottom));
            HGDIOBJ obmp = SelectObject(dc, bmp);
            const Theme& th = currentTheme();
            FillRect(dc, &rc, themeBrush(th.windowBg));
            if (st) {
                HPEN pen = CreatePen(PS_SOLID, 1, th.border);
                HGDIOBJ opn = SelectObject(dc, pen);
                HGDIOBJ obr = SelectObject(dc, GetStockObject(NULL_BRUSH));
                const int rad = dp(kPaneRadius, st->dpi);
                const int fi = dp(kFrameInset, st->dpi);
                const RECT* slots[] = {&st->treeSlot, &st->listSlot, &st->editSlot};
                for (const RECT* s : slots) {
                    RECT f = *s;
                    InflateRect(&f, -fi, -fi);
                    if (f.right > f.left && f.bottom > f.top)
                        RoundRect(dc, f.left, f.top, f.right, f.bottom, rad, rad);
                }
                SelectObject(dc, opn);
                SelectObject(dc, obr);
                DeleteObject(pen);
            }
            BitBlt(hdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
            SelectObject(dc, obmp);
            DeleteObject(bmp);
            DeleteDC(dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DPICHANGED:
            if (st) {
                const int oldDpi = st->dpi;
                st->dpi = HIWORD(wParam);
                if (oldDpi > 0) {
                    st->sidebarWidth = MulDiv(st->sidebarWidth, st->dpi, oldDpi);
                    st->editorHeight = MulDiv(st->editorHeight, st->dpi, oldDpi);
                }
                createFonts(st);
                SendMessageW(st->hTree, WM_SETFONT, reinterpret_cast<WPARAM>(st->hUi), TRUE);
                gridUpdateDpi(st->hList, static_cast<UINT>(st->dpi));
                editorUpdateDpi(st->hEdit, static_cast<UINT>(st->dpi));
                auto* sug = reinterpret_cast<RECT*>(lParam);
                SetWindowPos(hwnd, nullptr, sug->left, sug->top, sug->right - sug->left,
                             sug->bottom - sug->top, SWP_NOZORDER | SWP_NOACTIVATE);
                layout(st);
                InvalidateRect(st->hCmdBar, nullptr, TRUE);
            }
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            const int d = st ? st->dpi : static_cast<int>(GetDpiForSystem());
            mmi->ptMinTrackSize.x = dp(900, d);
            mmi->ptMinTrackSize.y = dp(600, d);
            return 0;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            if (id == ID_MENU) showMainMenu(st);
            else if (id == ID_OPEN) doOpen(st);
            else if (id == ID_RUN) doRunWhole(st);
            else if (id == ID_RUN_STMT) doRunStatement(st);
            else if (id == ID_CANCEL) doCancel(st);
            else if (id == ID_READONLY) toggleReadOnly(st);
            else if (id == ID_TX_BEGIN) doTransaction(st, L"BEGIN");
            else if (id == ID_TX_COMMIT) doTransaction(st, L"COMMIT");
            else if (id == ID_TX_ROLLBACK) doTransaction(st, L"ROLLBACK");
            else if (id == ID_HIST_UP) doHistoryUp(st);
            else if (id == ID_HIST_DOWN) doHistoryDown(st);
            else if (id == ID_REFRESH_SCHEMA) fetchTablesAsync(st);
            else if (id == ID_CTX_INSERT) {
                if (!st->contextTable.empty()) {
                    SetWindowTextW(st->hEdit, selectStatementFor(st->contextTable).c_str());
                    SetFocus(st->hEdit);
                }
            } else if (id == ID_CTX_PREVIEW) {
                if (!st->contextTable.empty()) runText(st, selectStatementFor(st->contextTable), false);
            } else if (id == ID_CTX_COPY) {
                if (!st->contextTable.empty()) copyToClipboard(hwnd, st->contextTable);
            } else if (id == ID_HISTORY) {
                doHistory(st);
            } else if (id == ID_CONN_DETAILS) {
                doConnectionDetails(st);
            } else if (id == ID_NEW) {
                createMainWindow(SW_SHOW);
            } else if (id == ID_CLOSE) {
                DestroyWindow(hwnd);
            } else if (id == ID_TOGGLE_THEME) {
                themeOverride() = currentTheme().dark ? 0 : 1;
                reapplyTheme(st);
            } else if (id == ID_ABOUT) {
                doAbout(hwnd);
            } else if (id == ID_HELP) {
                doHelp(hwnd);
            } else if (id == ID_CHECK_UPDATES) {
                checkForUpdates();
            } else if (id == ID_EXIT) {
                DestroyWindow(hwnd);
            }
            return 0;
        }
        case WM_NOTIFY: {
            auto* nh = reinterpret_cast<NMHDR*>(lParam);
            if (st && nh->idFrom == IDC_TREE) {
                if (nh->code == TVN_ITEMEXPANDINGW) {
                    onTreeExpand(st, reinterpret_cast<NMTREEVIEWW*>(lParam));
                    return 0;
                }
                if (nh->code == NM_DBLCLK) {
                    onTreeDoubleClick(st);
                    return 0;
                }
                if (nh->code == NM_RCLICK) {
                    onTreeContextMenu(st);
                    return TRUE;
                }
            }
            // The results grid (SqlGridControl) handles its own notifications,
            // sorting, and context menu internally.
            return 0;
        }
        case WM_APP_TABLES: {
            auto* m = reinterpret_cast<TablesMsg*>(lParam);
            populateTables(st, m->tables);
            delete m;
            return 0;
        }
        case WM_APP_COLUMNS: {
            auto* m = reinterpret_cast<ColumnsMsg*>(lParam);
            populateColumns(st, m->node, m->lines);
            delete m;
            return 0;
        }
        case WM_APP_RESULT: {
            auto* r = reinterpret_cast<QueryResult*>(lParam);
            st->busy = false;
            if (st->hCmdBar) InvalidateRect(st->hCmdBar, nullptr, FALSE);
            if (st->cancelRequested) {
                setStatus(st, L"Query cancelled.");
            } else {
                if (r->error)
                    setStatus(st, L"Error: " + *r->error);
                else
                    showResult(st, *r);
                st->inTransaction = updateInTransaction(st->inTransaction, st->lastRunStatements);
                updateFlags(st);
            }
            st->cancelRequested = false;
            delete r;
            return 0;
        }
        case WM_APP_CONNECTED: {
            auto* m = reinterpret_cast<ConnectMsg*>(lParam);
            if (m->ok) {
                applyConnectionPersistence(st);
                st->sslActive = st->session.isSSLActive();
                gridClear(st->hList);
                setTitle(st);
                setStatus(st, st->session.statusMessage());
                updateFlags(st);
                fetchTablesAsync(st);
            } else {
                st->sslActive = false;
                updateFlags(st);
                themedMessageBox(hwnd, m->error.c_str(), L"Connection failed", MB_ICONERROR | MB_OK);
                setStatus(st, L"Connection failed.");
            }
            if (st->hCmdBar) InvalidateRect(st->hCmdBar, nullptr, FALSE);
            delete m;
            return 0;
        }
        case WM_SETFOCUS:
            if (st && st->hEdit) SetFocus(st->hEdit);
            return 0;
        case WM_DESTROY:
            if (st && st->hUi) DeleteObject(st->hUi);
            if (st && st->hGlyph) DeleteObject(st->hGlyph);
            return 0;
        case WM_NCDESTROY:
            delete st;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            if (--g_windowCount <= 0) PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

int runApp(HINSTANCE hInstance, int nCmdShow) {
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_WIN95_CLASSES | ICC_TREEVIEW_CLASSES | ICC_BAR_CLASSES |
                ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);
    enableDarkMenus();

    if (!registerSqlEditorClass(hInstance)) {
        MessageBoxW(nullptr, L"Failed to register the SQL editor window class.", L"SQLTerminal",
                    MB_ICONERROR | MB_OK);
        return 1;
    }
    registerSqlGridClass(hInstance);

    WNDCLASSEXW splitter{};
    splitter.cbSize = sizeof(splitter);
    splitter.style = CS_HREDRAW | CS_VREDRAW;  // fully repaint the separator on resize
    splitter.lpfnWndProc = SplitterProc;
    splitter.hInstance = hInstance;
    splitter.lpszClassName = kSplitterClass;
    splitter.hCursor = LoadCursorW(nullptr, IDC_SIZENS);
    splitter.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    RegisterClassExW(&splitter);

    WNDCLASSEXW vsplitter{};
    vsplitter.cbSize = sizeof(vsplitter);
    vsplitter.style = CS_HREDRAW | CS_VREDRAW;
    vsplitter.lpfnWndProc = VSplitterProc;
    vsplitter.hInstance = hInstance;
    vsplitter.lpszClassName = kVSplitterClass;
    vsplitter.hCursor = LoadCursorW(nullptr, IDC_SIZEWE);
    vsplitter.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    RegisterClassExW(&vsplitter);

    WNDCLASSEXW cmdbar{};
    cmdbar.cbSize = sizeof(cmdbar);
    cmdbar.style = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;  // dbl-click + full repaint on resize
    cmdbar.lpfnWndProc = CmdBarProc;
    cmdbar.hInstance = hInstance;
    cmdbar.lpszClassName = kCmdBarClass;
    cmdbar.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cmdbar.hbrBackground = nullptr;
    RegisterClassExW(&cmdbar);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(currentTheme().windowBg);
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hIconSm = reinterpret_cast<HICON>(LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APPICON),
                                                    IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                                    GetSystemMetrics(SM_CYSMICON), 0));
    if (!RegisterClassExW(&wc)) return 1;

    g_appInstance = hInstance;
    if (!createMainWindow(nCmdShow)) return 1;
    initUpdater();

    ACCEL accels[] = {
        {FCONTROL | FVIRTKEY, static_cast<WORD>('E'), ID_RUN},
        {FCONTROL | FVIRTKEY, static_cast<WORD>('O'), ID_OPEN},
        {FCONTROL | FVIRTKEY, VK_RETURN, ID_RUN_STMT},
        {FCONTROL | FVIRTKEY, VK_OEM_PERIOD, ID_CANCEL},
        {FCONTROL | FVIRTKEY, VK_UP, ID_HIST_UP},
        {FCONTROL | FVIRTKEY, VK_DOWN, ID_HIST_DOWN},
        {FCONTROL | FVIRTKEY, static_cast<WORD>('R'), ID_HISTORY},
        {FCONTROL | FVIRTKEY, static_cast<WORD>('N'), ID_NEW},
        {FCONTROL | FVIRTKEY, static_cast<WORD>('W'), ID_CLOSE},
        {FVIRTKEY, VK_F1, ID_HELP},
    };
    HACCEL hAccel = CreateAcceleratorTableW(accels, static_cast<int>(sizeof(accels) /
                                                                     sizeof(accels[0])));

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        HWND active = GetActiveWindow();
        if (!active || !TranslateAcceleratorW(active, hAccel, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (hAccel) DestroyAcceleratorTable(hAccel);
    shutdownUpdater();
    return static_cast<int>(msg.wParam);
}

}  // namespace sqlterm
