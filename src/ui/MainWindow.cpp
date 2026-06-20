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
#include "ui/CellDetailDialog.h"
#include "ui/ConnectionDialog.h"
#include "ui/HistoryDialog.h"

namespace sqlterm {
namespace {

constexpr wchar_t kClassName[] = L"SQLTerminalMainWindow";
constexpr wchar_t kSplitterClass[] = L"SQLTerminalSplitter";
constexpr wchar_t kVSplitterClass[] = L"SQLTerminalVSplitter";

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

double epochNow() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// Follow the system light/dark setting for the window title bar (Win10 2004+).
bool systemUsesDarkMode() {
    DWORD value = 1, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return false;
    return value == 0;
}
void applyDarkTitleBar(HWND hwnd) {
    BOOL dark = systemUsesDarkMode() ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
}

COLORREF colorFor(sqlcore::SyntaxToken t) {
    switch (t) {
        case sqlcore::SyntaxToken::Keyword: return RGB(199, 37, 108);
        case sqlcore::SyntaxToken::Number: return RGB(128, 0, 128);
        case sqlcore::SyntaxToken::StringLiteral: return RGB(196, 26, 22);
        case sqlcore::SyntaxToken::Comment: return RGB(34, 139, 34);
    }
    return GetSysColor(COLOR_WINDOWTEXT);
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
    base.crTextColor = GetSysColor(COLOR_WINDOWTEXT);
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

void setStatus(AppState* st, const std::wstring& text) {
    SendMessageW(st->hStatus, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(text.c_str()));
}

void updateFlags(AppState* st) {
    std::wstring f;
    auto add = [&](const wchar_t* s) {
        if (!f.empty()) f += L"  ·  ";
        f += s;
    };
    if (st->sslActive) add(L"SSL");
    if (st->readOnly) add(L"read-only");
    if (st->inTransaction) add(L"in transaction");
    SendMessageW(st->hStatus, SB_SETTEXTW, 1, reinterpret_cast<LPARAM>(f.c_str()));
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
    const int cw = rc.right;
    const int ch = (std::max)(0L, static_cast<long>(rc.bottom - statusHeight(st)));

    // Left: schema sidebar (tree) + vertical splitter.
    const int vsW = kSplitterHeight;
    int sw = st->sidebarWidth;
    int maxSidebar = cw - 200 - vsW;
    if (maxSidebar < 120) maxSidebar = (std::max)(0, cw - vsW);
    if (sw > maxSidebar) sw = maxSidebar;
    if (sw < 0) sw = 0;
    const int rx = sw + vsW;
    const int rw = (std::max)(0, cw - rx);
    MoveWindow(st->hTree, 0, 0, sw, ch, TRUE);
    MoveWindow(st->hVSplitter, sw, 0, vsW, ch, TRUE);

    // Right: results / horizontal splitter / editor.
    int editH = st->editorHeight;
    int maxEdit = ch - kMinList - kSplitterHeight;
    if (maxEdit < kMinEditor) maxEdit = (std::max)(0, ch - kSplitterHeight);
    if (editH > maxEdit) editH = maxEdit;
    if (editH < 0) editH = 0;
    int listH = ch - editH - kSplitterHeight;
    if (listH < 0) listH = 0;

    MoveWindow(st->hList, rx, 0, rw, listH, TRUE);
    MoveWindow(st->hSplitter, rx, listH, rw, kSplitterHeight, TRUE);
    MoveWindow(st->hEdit, rx, listH + kSplitterHeight, rw, editH, TRUE);

    if (st->hStatus) {
        int parts[2] = {cw - 180, -1};
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
        setStatus(st, L"No database — File ▸ Open Database… (Ctrl+O)");
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
                MessageBoxW(st->hwnd, dot->text.c_str(), L"SQLTerminal", MB_OK | MB_ICONINFORMATION);
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
    CheckMenuItem(GetMenu(st->hwnd), ID_READONLY, st->readOnly ? MF_CHECKED : MF_UNCHECKED);
    updateFlags(st);
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
    MessageBoxW(st->hwnd, m.c_str(), L"Connection Details", MB_OK | MB_ICONINFORMATION);
}

void createChildren(AppState* st, HINSTANCE hInst) {
    st->hList = CreateWindowExW(
        0, WC_LISTVIEW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_OWNERDATA, 0, 0, 0, 0,
        st->hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LIST)), hInst, nullptr);
    ListView_SetExtendedListViewStyle(
        st->hList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

    st->hSplitter = CreateWindowExW(0, kSplitterClass, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0,
                                    st->hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SPLIT)),
                                    hInst, nullptr);
    SetWindowLongPtrW(st->hSplitter, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));

    st->hTree = CreateWindowExW(
        WS_EX_CLIENTEDGE, WC_TREEVIEW, L"",
        WS_CHILD | WS_VISIBLE | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS,
        0, 0, 0, 0, st->hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TREE)), hInst, nullptr);
    st->hVSplitter = CreateWindowExW(0, kVSplitterClass, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0,
                                     st->hwnd,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_VSPLIT)),
                                     hInst, nullptr);
    SetWindowLongPtrW(st->hVSplitter, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));

    st->hEdit = CreateWindowExW(
        WS_EX_CLIENTEDGE, MSFTEDIT_CLASS, L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN |
            ES_NOHIDESEL,
        0, 0, 0, 0, st->hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EDIT)), hInst, nullptr);
    SendMessageW(st->hEdit, EM_SETBKGNDCOLOR, 0, static_cast<LPARAM>(GetSysColor(COLOR_WINDOW)));
    CHARFORMAT2W cf{};
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_FACE | CFM_SIZE | CFM_COLOR;
    cf.yHeight = 220;
    cf.crTextColor = GetSysColor(COLOR_WINDOWTEXT);
    lstrcpynW(cf.szFaceName, L"Consolas", LF_FACESIZE);
    SendMessageW(st->hEdit, EM_SETCHARFORMAT, SCF_ALL, reinterpret_cast<LPARAM>(&cf));
    SetWindowTextW(st->hEdit, L"SELECT * FROM users;");
    SendMessageW(st->hEdit, EM_SETEVENTMASK, 0, ENM_CHANGE);
    applyHighlight(st);

    st->hStatus = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0,
                                  st->hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STATUS)),
                                  hInst, nullptr);
    setStatus(st, L"No database — File ▸ Open Database… (Ctrl+O)");
}

void buildMenu(HWND hwnd) {
    HMENU bar = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, ID_NEW, L"&New Window\tCtrl+N");
    AppendMenuW(file, MF_STRING, ID_OPEN, L"&Open Database…\tCtrl+O");
    AppendMenuW(file, MF_STRING, ID_CONN_DETAILS, L"Connection &Details…");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, ID_CLOSE, L"&Close Window\tCtrl+W");
    AppendMenuW(file, MF_STRING, ID_EXIT, L"E&xit");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");

    HMENU query = CreatePopupMenu();
    AppendMenuW(query, MF_STRING, ID_RUN, L"&Run\tCtrl+E");
    AppendMenuW(query, MF_STRING, ID_RUN_STMT, L"Run &Statement at Cursor\tCtrl+Enter");
    AppendMenuW(query, MF_STRING, ID_CANCEL, L"&Cancel Running Query\tCtrl+.");
    AppendMenuW(query, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(query, MF_STRING, ID_READONLY, L"Read-&only mode");
    AppendMenuW(query, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(query, MF_STRING, ID_TX_BEGIN, L"&Begin Transaction");
    AppendMenuW(query, MF_STRING, ID_TX_COMMIT, L"Co&mmit Transaction");
    AppendMenuW(query, MF_STRING, ID_TX_ROLLBACK, L"Roll&back Transaction");
    AppendMenuW(query, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(query, MF_STRING, ID_REFRESH_SCHEMA, L"Re&fresh Schema");
    AppendMenuW(query, MF_STRING, ID_HISTORY, L"&History && Snippets…\tCtrl+R");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(query), L"&Query");

    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, ID_CHECK_UPDATES, L"Check for &Updates…");
    AppendMenuW(help, MF_STRING, ID_HELP, L"SQLTerminal &Help\tF1");
    AppendMenuW(help, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(help, MF_STRING, ID_ABOUT, L"&About SQLTerminal");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(help), L"&Help");
    SetMenu(hwnd, bar);
}

LRESULT CALLBACK SplitterProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* st = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
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
                const int ch = (std::max)(0L, static_cast<long>(rc.bottom - statusHeight(st)));
                int newEdit = ch - kSplitterHeight - pt.y;
                int maxEdit = ch - kMinList - kSplitterHeight;
                if (maxEdit < kMinEditor) maxEdit = kMinEditor;
                if (newEdit < kMinEditor) newEdit = kMinEditor;
                if (newEdit > maxEdit) newEdit = maxEdit;
                st->editorHeight = newEdit;
                layout(st);
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
                int newW = pt.x;
                int maxW = rc.right - 200 - kSplitterHeight;
                if (maxW < 120) maxW = 120;
                if (newW < 120) newW = 120;
                if (newW > maxW) newW = maxW;
                st->sidebarWidth = newW;
                layout(st);
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
    HWND hwnd = CreateWindowExW(0, kClassName, L"SQLTerminal", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                CW_USEDEFAULT, 1100, 750, nullptr, nullptr, g_appInstance, state);
    if (!hwnd) {
        delete state;
        return nullptr;
    }
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    return hwnd;
}

void doAbout(HWND hwnd) {
    MessageBoxW(hwnd,
                L"SQLTerminal (Win32)  0.1.0\n\n"
                L"A native Windows SQL terminal for SQLite and PostgreSQL.\n"
                L"Licensed under GPL-3.0.",
                L"About SQLTerminal", MB_OK | MB_ICONINFORMATION);
}

void doHelp(HWND hwnd) {
    MessageBoxW(hwnd,
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
                L"Type .help in the editor for SQL dot-commands (.tables, .schema, …).",
                L"SQLTerminal Help", MB_OK | MB_ICONINFORMATION);
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
        case WM_CREATE:
            createChildren(st, reinterpret_cast<LPCREATESTRUCTW>(lParam)->hInstance);
            buildMenu(hwnd);
            layout(st);
            applyDarkTitleBar(hwnd);
            return 0;
        case WM_SETTINGCHANGE:
            applyDarkTitleBar(hwnd);  // react to light/dark toggle
            return 0;
        case WM_SIZE:
            if (st) layout(st);
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = 900;
            mmi->ptMinTrackSize.y = 600;
            return 0;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            const int code = HIWORD(wParam);
            if (reinterpret_cast<HWND>(lParam) == st->hEdit && code == EN_CHANGE)
                applyHighlight(st);
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
                    return 0;
                }
                if (nh->code == NM_CUSTOMDRAW) {
                    auto* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(lParam);
                    if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                    if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                        cd->clrTextBk = (cd->nmcd.dwItemSpec % 2) ? RGB(245, 245, 245)
                                                                  : GetSysColor(COLOR_WINDOW);
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
            delete m;
            return 0;
        }
        case WM_SETFOCUS:
            if (st && st->hEdit) SetFocus(st->hEdit);
            return 0;
        case WM_DESTROY:
            if (st && st->hMono) DeleteObject(st->hMono);
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

    if (!LoadLibraryW(L"Msftedit.dll")) {
        MessageBoxW(nullptr, L"Failed to load Msftedit.dll (RichEdit).", L"SQLTerminal",
                    MB_ICONERROR | MB_OK);
        return 1;
    }

    WNDCLASSEXW splitter{};
    splitter.cbSize = sizeof(splitter);
    splitter.lpfnWndProc = SplitterProc;
    splitter.hInstance = hInstance;
    splitter.lpszClassName = kSplitterClass;
    splitter.hCursor = LoadCursorW(nullptr, IDC_SIZENS);
    splitter.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    RegisterClassExW(&splitter);

    WNDCLASSEXW vsplitter{};
    vsplitter.cbSize = sizeof(vsplitter);
    vsplitter.lpfnWndProc = VSplitterProc;
    vsplitter.hInstance = hInstance;
    vsplitter.lpszClassName = kVSplitterClass;
    vsplitter.hCursor = LoadCursorW(nullptr, IDC_SIZEWE);
    vsplitter.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    RegisterClassExW(&vsplitter);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
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
