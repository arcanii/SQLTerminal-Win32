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
#include <uxtheme.h>
#include <windowsx.h>

#define _RICHEDIT_VER 0x0500
#include <richedit.h>

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
    ID_GRID_VIEW = 1016,
    ID_GRID_COPYVAL = 1017,
    ID_GRID_COPYROW = 1018,
    ID_GRID_TSV = 1019,
    ID_GRID_CSV = 1020,
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

constexpr int kSplitterHeight = 6;
constexpr int kMinEditor = 80;
constexpr int kMinList = 100;
constexpr int kCmdBarH = 46;

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
    HFONT hMono = nullptr;
    HFONT hUi = nullptr;
    HFONT hGlyph = nullptr;     // Segoe MDL2 Assets, for command-bar icons
    HWND hCmdBar = nullptr;     // custom top command bar (replaces the menu)
    int cmdHover = -1;          // hovered command-bar button index, or -1
    int capHover = -1;          // hovered caption button (0=min 1=max 2=close), or -1
    int dpi = 96;               // window DPI (per-monitor-v2)
    std::wstring statusMsg;   // owner-drawn status bar, left part
    std::wstring flagsText;   // owner-drawn status bar, right part (SSL/read-only/tx)
    int editorHeight = 150;
    int sidebarWidth = 220;
    std::wstring contextTable;  // table the schema context menu acted on
    bool suppressHighlight = false;
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

    // Results grid (virtual / owner-data).
    QueryResult result;
    int sortColumn = -1;
    bool sortAscending = true;
    std::vector<size_t> rowOrder;
    int ctxRow = -1;
    int ctxCol = -1;

    // Pending connect (applied on WM_APP_CONNECTED success).
    DatabaseConnection pendingConn;
    std::optional<std::wstring> pendingSaveAs;
    bool pendingRemember = false;
    bool pendingTouchCredentials = false;
};

// Scale a 96-dpi design value to the window's current DPI.
int dp(int v, int dpi) { return MulDiv(v, dpi, 96); }

// Create (or recreate, on DPI change) the UI/monospace/glyph fonts at st->dpi.
void createFonts(AppState* st) {
    if (st->hUi) DeleteObject(st->hUi);
    if (st->hMono) DeleteObject(st->hMono);
    if (st->hGlyph) DeleteObject(st->hGlyph);
    st->hUi = CreateFontW(-dp(14, st->dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
    st->hMono = CreateFontW(-dp(14, st->dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            FIXED_PITCH | FF_MODERN, L"Consolas");
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

COLORREF colorFor(sqlcore::SyntaxToken t) {
    const Theme& th = currentTheme();
    switch (t) {
        case sqlcore::SyntaxToken::Keyword: return th.synKeyword;
        case sqlcore::SyntaxToken::Number: return th.synNumber;
        case sqlcore::SyntaxToken::StringLiteral: return th.synString;
        case sqlcore::SyntaxToken::Comment: return th.synComment;
    }
    return th.textPrimary;
}

std::wstring editorText(HWND edit) {
    GETTEXTLENGTHEX gtl{};
    gtl.flags = GTL_NUMCHARS;
    gtl.codepage = 1200;
    const LONG n = static_cast<LONG>(SendMessageW(edit, EM_GETTEXTLENGTHEX,
                                                  reinterpret_cast<WPARAM>(&gtl), 0));
    if (n <= 0) return std::wstring();
    std::wstring buf(static_cast<size_t>(n), L'\0');
    GETTEXTEX gt{};
    gt.cb = static_cast<DWORD>((n + 1) * sizeof(wchar_t));
    gt.flags = GT_DEFAULT;
    gt.codepage = 1200;
    const LONG got = static_cast<LONG>(SendMessageW(
        edit, EM_GETTEXTEX, reinterpret_cast<WPARAM>(&gt), reinterpret_cast<LPARAM>(buf.data())));
    buf.resize(static_cast<size_t>(got));
    for (auto& c : buf)
        if (c == L'\r') c = L'\n';
    return buf;
}

LONG caretOffset(HWND edit) {
    CHARRANGE cr{};
    SendMessageW(edit, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&cr));
    return cr.cpMin;
}

void applyHighlight(AppState* st) {
    if (st->suppressHighlight) return;
    st->suppressHighlight = true;
    HWND e = st->hEdit;

    CHARRANGE saved{};
    SendMessageW(e, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&saved));
    SendMessageW(e, WM_SETREDRAW, FALSE, 0);

    const std::wstring text = editorText(e);
    const auto spans = sqlcore::SqlSyntaxHighlighter::computeSpans(text);

    CHARFORMAT2W base{};
    base.cbSize = sizeof(base);
    base.dwMask = CFM_COLOR;
    base.crTextColor = currentTheme().textPrimary;
    SendMessageW(e, EM_SETSEL, 0, -1);
    SendMessageW(e, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&base));

    for (const auto& s : spans) {
        CHARRANGE cr{static_cast<LONG>(s.location), static_cast<LONG>(s.location + s.length)};
        SendMessageW(e, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&cr));
        CHARFORMAT2W cf{};
        cf.cbSize = sizeof(cf);
        cf.dwMask = CFM_COLOR;
        cf.crTextColor = colorFor(s.type);
        SendMessageW(e, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&cf));
    }

    SendMessageW(e, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&saved));
    SendMessageW(e, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(e, nullptr, TRUE);
    st->suppressHighlight = false;
}

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
    place(st->hTree, 0, top, sw, ch);
    place(st->hVSplitter, sw, top, vsW, ch);

    // Right: results / horizontal splitter / editor.
    int editH = st->editorHeight;
    int maxEdit = ch - minList - splitH;
    if (maxEdit < minEdit) maxEdit = (std::max)(0, ch - splitH);
    if (editH > maxEdit) editH = maxEdit;
    if (editH < 0) editH = 0;
    int listH = ch - editH - splitH;
    if (listH < 0) listH = 0;

    place(st->hList, rx, top, rw, listH);
    place(st->hSplitter, rx, top + listH, rw, splitH);
    place(st->hEdit, rx, top + listH + splitH, rw, editH);
    if (dwp) EndDeferWindowPos(dwp);

    if (st->hStatus) {
        int parts[2] = {cw - dp(220, d), -1};
        SendMessageW(st->hStatus, SB_SETPARTS, 2, reinterpret_cast<LPARAM>(parts));
        updateFlags(st);
    }
}

void clearGrid(AppState* st) {
    ListView_SetItemCount(st->hList, 0);
    while (ListView_DeleteColumn(st->hList, 0)) {
    }
    st->result = QueryResult{};
    st->rowOrder.clear();
    st->sortColumn = -1;
}

// Cell value in *display* order (resolving the sort permutation), or "" out of range.
const std::wstring& cellAt(AppState* st, int displayRow, int col) {
    static const std::wstring empty;
    if (displayRow < 0 || static_cast<size_t>(displayRow) >= st->rowOrder.size()) return empty;
    const size_t src = st->rowOrder[static_cast<size_t>(displayRow)];
    if (src >= st->result.rows.size()) return empty;
    const auto& row = st->result.rows[src];
    if (col < 0 || static_cast<size_t>(col) >= row.size()) return empty;
    return row[static_cast<size_t>(col)];
}

std::wstring columnNameAt(AppState* st, int col) {
    if (col >= 0 && static_cast<size_t>(col) < st->result.columns.size())
        return st->result.columns[static_cast<size_t>(col)];
    return L"col" + std::to_wstring(col);
}

std::vector<std::vector<std::wstring>> displayRows(AppState* st) {
    std::vector<std::vector<std::wstring>> out;
    out.reserve(st->rowOrder.size());
    for (size_t d : st->rowOrder)
        if (d < st->result.rows.size()) out.push_back(st->result.rows[d]);
    return out;
}

void updateSortArrow(AppState* st) {
    HWND header = ListView_GetHeader(st->hList);
    const int n = Header_GetItemCount(header);
    for (int i = 0; i < n; ++i) {
        HDITEMW hi{};
        hi.mask = HDI_FORMAT;
        Header_GetItem(header, i, &hi);
        hi.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (i == st->sortColumn) hi.fmt |= (st->sortAscending ? HDF_SORTUP : HDF_SORTDOWN);
        Header_SetItem(header, i, &hi);
    }
}

void setGridResult(AppState* st, const QueryResult& r) {
    ListView_SetItemCount(st->hList, 0);
    while (ListView_DeleteColumn(st->hList, 0)) {
    }
    st->result = r;
    st->sortColumn = -1;
    st->sortAscending = true;
    st->rowOrder.resize(r.rows.size());
    for (size_t i = 0; i < r.rows.size(); ++i) st->rowOrder[i] = i;

    for (size_t i = 0; i < r.columns.size(); ++i) {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        col.cx = 140;
        col.iSubItem = static_cast<int>(i);
        col.pszText = const_cast<LPWSTR>(r.columns[i].c_str());
        ListView_InsertColumn(st->hList, static_cast<int>(i), &col);
    }
    ListView_SetItemCountEx(st->hList, static_cast<int>(r.rows.size()), LVSICF_NOINVALIDATEALL);
    for (size_t i = 0; i < r.columns.size(); ++i)
        ListView_SetColumnWidth(st->hList, static_cast<int>(i), LVSCW_AUTOSIZE_USEHEADER);
    InvalidateRect(st->hList, nullptr, TRUE);
}

void showResult(AppState* st, const QueryResult& r) {
    wchar_t buf[160];
    if (!r.columns.empty()) {
        setGridResult(st, r);
        std::swprintf(buf, 160, L"%llu rows · %.0f ms",
                      static_cast<unsigned long long>(r.rows.size()), r.executionTimeSec * 1000.0);
    } else {
        clearGrid(st);
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
        if (MessageBoxW(st->hwnd, d.message.c_str(), L"Confirm destructive statement",
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
                clearGrid(st);
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
        wc.hbrBackground = themeBrush(currentTheme().panelBg);
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
    std::wstring m;
    if (!st->session.isConnected()) {
        m = L"Not connected.";
    } else if (st->currentConnection.engine == DatabaseEngine::Postgres) {
        const auto& c = st->currentConnection;
        m = L"Engine:  PostgreSQL\nHost:  " + c.host + L"\nPort:  " + c.port + L"\nDatabase:  " +
            c.databaseName + L"\nUser:  " + c.username + L"\nEncryption:  " +
            (st->sslActive ? L"SSL/TLS" : L"none");
    } else {
        m = L"Engine:  SQLite\nFile:  " + st->currentConnection.filePath;
    }
    showInfoDialog(st->hwnd, L"Connection Details", m);
}

LRESULT CALLBACK ListSubclassProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
LRESULT CALLBACK StatusSubclassProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

void createChildren(AppState* st, HINSTANCE hInst) {
    const Theme& th = currentTheme();
    createFonts(st);

    st->hList = CreateWindowExW(
        0, WC_LISTVIEW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_OWNERDATA, 0, 0, 0, 0,
        st->hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LIST)), hInst, nullptr);
    ListView_SetExtendedListViewStyle(st->hList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    ListView_SetBkColor(st->hList, th.panelBg);
    ListView_SetTextBkColor(st->hList, th.panelBg);
    ListView_SetTextColor(st->hList, th.textPrimary);
    if (th.dark) SetWindowTheme(st->hList, L"DarkMode_Explorer", nullptr);
    SendMessageW(st->hList, WM_SETFONT, reinterpret_cast<WPARAM>(st->hMono), TRUE);
    SetWindowSubclass(st->hList, ListSubclassProc, 1, reinterpret_cast<DWORD_PTR>(st));

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
        0, MSFTEDIT_CLASS, L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN |
            ES_NOHIDESEL,
        0, 0, 0, 0, st->hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EDIT)), hInst, nullptr);
    SendMessageW(st->hEdit, EM_SETBKGNDCOLOR, 0, static_cast<LPARAM>(th.windowBg));
    if (th.dark) SetWindowTheme(st->hEdit, L"DarkMode_Explorer", nullptr);
    CHARFORMAT2W cf{};
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_FACE | CFM_SIZE | CFM_COLOR;
    cf.yHeight = 220;
    cf.crTextColor = th.textPrimary;
    lstrcpynW(cf.szFaceName, L"Consolas", LF_FACESIZE);
    SendMessageW(st->hEdit, EM_SETCHARFORMAT, SCF_ALL, reinterpret_cast<LPARAM>(&cf));
    SetWindowTextW(st->hEdit, L"SELECT * FROM users;");
    SendMessageW(st->hEdit, EM_SETEVENTMASK, 0, ENM_CHANGE);
    applyHighlight(st);

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

// Subclass the results ListView so we can dark-custom-draw its header (the
// header notifies its parent, the ListView, so we intercept it here).
LRESULT CALLBACK ListSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR,
                                  DWORD_PTR ref) {
    auto* st = reinterpret_cast<AppState*>(ref);
    if (msg == WM_NOTIFY) {
        auto* nh = reinterpret_cast<NMHDR*>(lParam);
        if (nh->code == NM_CUSTOMDRAW && nh->hwndFrom == ListView_GetHeader(hwnd)) {
            auto* cd = reinterpret_cast<NMCUSTOMDRAW*>(lParam);
            if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
                const Theme& th = currentTheme();
                HDC dc = cd->hdc;
                RECT rc = cd->rc;
                HBRUSH bg = CreateSolidBrush(th.panelElevBg);
                FillRect(dc, &rc, bg);
                DeleteObject(bg);
                HPEN pen = CreatePen(PS_SOLID, 1, th.border);
                HGDIOBJ op = SelectObject(dc, pen);
                MoveToEx(dc, rc.left, rc.bottom - 1, nullptr);
                LineTo(dc, rc.right, rc.bottom - 1);
                SelectObject(dc, op);
                DeleteObject(pen);

                const int col = static_cast<int>(cd->dwItemSpec);
                wchar_t buf[256] = L"";
                HDITEMW hi{};
                hi.mask = HDI_TEXT;
                hi.pszText = buf;
                hi.cchTextMax = 256;
                Header_GetItem(nh->hwndFrom, col, &hi);
                const bool sorted = (col == st->sortColumn);
                SetBkMode(dc, TRANSPARENT);
                SelectObject(dc, st->hUi);
                SetTextColor(dc, sorted ? th.accent : th.textSecondary);
                RECT tr = rc;
                tr.left += dp(10, st->dpi);
                tr.right -= dp(18, st->dpi);
                DrawTextW(dc, buf, -1, &tr,
                          DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
                if (sorted) {
                    const int ax = rc.right - dp(12, st->dpi);
                    const int ay = (rc.top + rc.bottom) / 2;
                    const int aw = dp(4, st->dpi), ah = dp(3, st->dpi);
                    POINT tri[3];
                    if (st->sortAscending) {
                        tri[0] = {ax, ay - ah};
                        tri[1] = {ax - aw, ay + ah};
                        tri[2] = {ax + aw, ay + ah};
                    } else {
                        tri[0] = {ax - aw, ay - ah};
                        tri[1] = {ax + aw, ay - ah};
                        tri[2] = {ax, ay + ah};
                    }
                    HBRUSH ab = CreateSolidBrush(th.accent);
                    HGDIOBJ oab = SelectObject(dc, ab);
                    HGDIOBJ oap = SelectObject(dc, GetStockObject(NULL_PEN));
                    Polygon(dc, tri, 3);
                    SelectObject(dc, oab);
                    SelectObject(dc, oap);
                    DeleteObject(ab);
                }
                return CDRF_SKIPDEFAULT;
            }
            return CDRF_DODEFAULT;
        }
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, ListSubclassProc, 1);
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
            const Theme& th = currentTheme();
            HBRUSH bg = CreateSolidBrush(th.windowBg);
            FillRect(dc, &rc, bg);
            DeleteObject(bg);
            HPEN pen = CreatePen(PS_SOLID, 1, th.border);
            HGDIOBJ old = SelectObject(dc, pen);
            const int y = (rc.bottom - rc.top) / 2;
            MoveToEx(dc, rc.left, y, nullptr);
            LineTo(dc, rc.right, y);
            SelectObject(dc, old);
            DeleteObject(pen);
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
            const Theme& th = currentTheme();
            HBRUSH bg = CreateSolidBrush(th.windowBg);
            FillRect(dc, &rc, bg);
            DeleteObject(bg);
            HPEN pen = CreatePen(PS_SOLID, 1, th.border);
            HGDIOBJ old = SelectObject(dc, pen);
            const int x = (rc.right - rc.left) / 2;
            MoveToEx(dc, x, rc.top, nullptr);
            LineTo(dc, x, rc.bottom);
            SelectObject(dc, old);
            DeleteObject(pen);
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
            DeleteObject(title);
            DeleteObject(body);
            EndPaint(hwnd, &ps);
            return 0;
        }
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
    const int W = 440, H = 196;
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

    ListView_SetBkColor(st->hList, th.panelBg);
    ListView_SetTextBkColor(st->hList, th.panelBg);
    ListView_SetTextColor(st->hList, th.textPrimary);
    SetWindowTheme(st->hList, explorerTheme, nullptr);
    InvalidateRect(st->hList, nullptr, TRUE);
    if (HWND header = ListView_GetHeader(st->hList)) InvalidateRect(header, nullptr, TRUE);

    TreeView_SetBkColor(st->hTree, th.panelBg);
    TreeView_SetTextColor(st->hTree, th.textPrimary);
    SetWindowTheme(st->hTree, explorerTheme, nullptr);
    InvalidateRect(st->hTree, nullptr, TRUE);

    SendMessageW(st->hEdit, EM_SETBKGNDCOLOR, 0, static_cast<LPARAM>(th.windowBg));
    SetWindowTheme(st->hEdit, explorerTheme, nullptr);
    applyHighlight(st);  // re-colors the syntax + base text

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
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect(reinterpret_cast<HDC>(wParam), &rc, themeBrush(currentTheme().windowBg));
            return 1;
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
                SendMessageW(st->hList, WM_SETFONT, reinterpret_cast<WPARAM>(st->hMono), TRUE);
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
            const int code = HIWORD(wParam);
            if (reinterpret_cast<HWND>(lParam) == st->hEdit && code == EN_CHANGE)
                applyHighlight(st);
            else if (id == ID_MENU) showMainMenu(st);
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
            } else if (id == ID_GRID_VIEW) {
                showCellDetail(hwnd, columnNameAt(st, st->ctxCol), cellAt(st, st->ctxRow, st->ctxCol));
            } else if (id == ID_GRID_COPYVAL) {
                copyToClipboard(hwnd, cellAt(st, st->ctxRow, st->ctxCol));
            } else if (id == ID_GRID_COPYROW) {
                std::wstring line;
                for (int c = 0; c < static_cast<int>(st->result.columns.size()); ++c) {
                    if (c) line += L'\t';
                    line += cellAt(st, st->ctxRow, c);
                }
                copyToClipboard(hwnd, line);
            } else if (id == ID_GRID_TSV) {
                copyToClipboard(hwnd, buildTsv(st->result.columns, displayRows(st)));
            } else if (id == ID_GRID_CSV) {
                copyToClipboard(hwnd, buildCsv(st->result.columns, displayRows(st)));
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
        case WM_DRAWITEM: {
            auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (st && dis->hwndItem == st->hStatus) {
                const Theme& th = currentTheme();
                HBRUSH bg = CreateSolidBrush(th.panelElevBg);
                FillRect(dis->hDC, &dis->rcItem, bg);
                DeleteObject(bg);
                SetBkMode(dis->hDC, TRANSPARENT);
                HGDIOBJ oldFont = SelectObject(dis->hDC, st->hUi);
                RECT tr = dis->rcItem;
                if (dis->itemID == 0) {
                    tr.left += dp(12, st->dpi);
                    SetTextColor(dis->hDC, th.textPrimary);
                    DrawTextW(dis->hDC, st->statusMsg.c_str(), -1, &tr,
                              DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
                } else {
                    // Right part: active status flags as subtle pills, right-to-left.
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
                    SelectObject(dis->hDC, st->hUi);
                    int right = tr.right - dp(12, st->dpi);
                    for (int i = static_cast<int>(sizeof(flags) / sizeof(flags[0])) - 1; i >= 0; --i) {
                        if (!flags[i].on) continue;
                        SIZE sz{};
                        GetTextExtentPoint32W(dis->hDC, flags[i].text, lstrlenW(flags[i].text), &sz);
                        const int w = sz.cx + dp(18, st->dpi);
                        const int h = dp(18, st->dpi);
                        const int midY = (tr.top + tr.bottom) / 2;
                        RECT pr{right - w, midY - h / 2, right, midY + h / 2};
                        HBRUSH pb = CreateSolidBrush(th.panelBg);
                        HGDIOBJ ob = SelectObject(dis->hDC, pb);
                        HGDIOBJ op = SelectObject(dis->hDC, GetStockObject(NULL_PEN));
                        RoundRect(dis->hDC, pr.left, pr.top, pr.right, pr.bottom, dp(9, st->dpi),
                                  dp(9, st->dpi));
                        SelectObject(dis->hDC, ob);
                        SelectObject(dis->hDC, op);
                        DeleteObject(pb);
                        SetTextColor(dis->hDC, flags[i].fg);
                        DrawTextW(dis->hDC, flags[i].text, -1, &pr,
                                  DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
                        right -= w + dp(6, st->dpi);
                    }
                }
                SelectObject(dis->hDC, oldFont);
                return TRUE;
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
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
            if (st && nh->idFrom == IDC_LIST) {
                if (nh->code == LVN_GETDISPINFOW) {
                    auto* di = reinterpret_cast<NMLVDISPINFOW*>(lParam);
                    if (di->item.mask & LVIF_TEXT) {
                        const std::wstring& v = cellAt(st, di->item.iItem, di->item.iSubItem);
                        lstrcpynW(di->item.pszText, v.c_str(), di->item.cchTextMax);
                    }
                    return 0;
                }
                if (nh->code == LVN_COLUMNCLICK) {
                    auto* nlv = reinterpret_cast<NMLISTVIEW*>(lParam);
                    const int col = nlv->iSubItem;
                    if (st->sortColumn == col)
                        st->sortAscending = !st->sortAscending;
                    else {
                        st->sortColumn = col;
                        st->sortAscending = true;
                    }
                    st->rowOrder = sortedRowOrder(st->result.rows, static_cast<size_t>(col),
                                                  st->sortAscending);
                    updateSortArrow(st);
                    InvalidateRect(st->hList, nullptr, TRUE);
                    InvalidateRect(ListView_GetHeader(st->hList), nullptr, TRUE);
                    return 0;
                }
                if (nh->code == NM_CUSTOMDRAW) {
                    auto* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(lParam);
                    if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                    if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                        const Theme& th = currentTheme();
                        const int row = static_cast<int>(cd->nmcd.dwItemSpec);
                        const bool sel =
                            (ListView_GetItemState(st->hList, row, LVIS_SELECTED) & LVIS_SELECTED) != 0;
                        if (sel) {
                            cd->clrText = th.selectionText;
                            cd->clrTextBk = th.selectionBg;
                        } else {
                            cd->clrText = th.textPrimary;
                            cd->clrTextBk = (row % 2) ? th.altRowBg : th.panelBg;
                        }
                        return CDRF_DODEFAULT;
                    }
                    return CDRF_DODEFAULT;
                }
                if (nh->code == NM_RCLICK) {
                    auto* ia = reinterpret_cast<NMITEMACTIVATE*>(lParam);
                    if (ia->iItem >= 0) {
                        st->ctxRow = ia->iItem;
                        st->ctxCol = ia->iSubItem >= 0 ? ia->iSubItem : 0;
                        POINT pt;
                        GetCursorPos(&pt);
                        HMENU pm = CreatePopupMenu();
                        AppendMenuW(pm, MF_STRING, ID_GRID_VIEW, L"View value…");
                        AppendMenuW(pm, MF_STRING, ID_GRID_COPYVAL, L"Copy value");
                        AppendMenuW(pm, MF_STRING, ID_GRID_COPYROW, L"Copy row");
                        AppendMenuW(pm, MF_SEPARATOR, 0, nullptr);
                        AppendMenuW(pm, MF_STRING, ID_GRID_TSV, L"Copy all as TSV");
                        AppendMenuW(pm, MF_STRING, ID_GRID_CSV, L"Copy all as CSV");
                        TrackPopupMenu(pm, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
                        DestroyMenu(pm);
                    }
                    return TRUE;
                }
            }
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
                clearGrid(st);
                setTitle(st);
                setStatus(st, st->session.statusMessage());
                updateFlags(st);
                fetchTablesAsync(st);
            } else {
                st->sslActive = false;
                updateFlags(st);
                MessageBoxW(hwnd, m->error.c_str(), L"Connection failed", MB_ICONERROR | MB_OK);
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
            if (st && st->hMono) DeleteObject(st->hMono);
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
    icc.dwICC = ICC_WIN95_CLASSES | ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES | ICC_BAR_CLASSES |
                ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);
    enableDarkMenus();

    if (!LoadLibraryW(L"Msftedit.dll")) {
        MessageBoxW(nullptr, L"Failed to load Msftedit.dll (RichEdit).", L"SQLTerminal",
                    MB_ICONERROR | MB_OK);
        return 1;
    }

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
