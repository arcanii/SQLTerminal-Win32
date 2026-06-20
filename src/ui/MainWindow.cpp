// SPDX-License-Identifier: GPL-3.0-or-later
// P2 main window: a results ListView (top), a draggable splitter, and a RichEdit
// SQL editor (bottom) with live syntax highlighting, plus a status bar. Run the
// whole editor with Ctrl+E or the statement under the cursor with Ctrl+Enter.
// Execution is still synchronous on the UI thread (P3 moves it off-thread).
#include "ui/MainWindow.h"

#include <commctrl.h>
#include <commdlg.h>

#define _RICHEDIT_VER 0x0500
#include <richedit.h>

#include <algorithm>
#include <cstdio>
#include <optional>
#include <string>

#include "core/SqlStatementSplitter.h"
#include "core/SqlSyntaxHighlighter.h"
#include "db/DatabaseSession.h"
#include "models/DatabaseConnection.h"
#include "models/QueryResult.h"

namespace sqlterm {
namespace {

constexpr wchar_t kClassName[] = L"SQLTerminalMainWindow";
constexpr wchar_t kSplitterClass[] = L"SQLTerminalSplitter";

enum : int {
    ID_OPEN = 1001,
    ID_EXIT = 1002,
    ID_RUN = 1003,
    ID_RUN_STMT = 1004,
    ID_CANCEL = 1005,
    IDC_LIST = 2001,
    IDC_EDIT = 2002,
    IDC_SPLIT = 2004,
    IDC_STATUS = 2005,
};

// Worker-thread results are marshalled to the UI thread via these messages; the
// LPARAM is a heap payload owned by (and freed in) the handler.
constexpr UINT WM_APP_RESULT = WM_APP + 1;     // lParam = QueryResult*
constexpr UINT WM_APP_CONNECTED = WM_APP + 2;  // lParam = ConnectMsg*

struct ConnectMsg {
    bool ok;
    std::wstring error;
};

constexpr int kSplitterHeight = 6;
constexpr int kMinEditor = 80;
constexpr int kMinList = 100;

struct AppState {
    HWND hwnd = nullptr;
    HWND hList = nullptr;
    HWND hEdit = nullptr;     // RichEdit
    HWND hSplitter = nullptr;
    HWND hStatus = nullptr;
    int editorHeight = 150;   // user-draggable
    bool suppressHighlight = false;
    bool busy = false;            // a query is in flight
    bool cancelRequested = false; // user hit Ctrl+. for the in-flight query
    DatabaseSession session;
    std::wstring dbName;
    std::wstring pendingPath;     // path being connected to
};

COLORREF colorFor(sqlcore::SyntaxToken t) {
    switch (t) {
        case sqlcore::SyntaxToken::Keyword: return RGB(199, 37, 108);   // pink
        case sqlcore::SyntaxToken::Number: return RGB(128, 0, 128);     // purple
        case sqlcore::SyntaxToken::StringLiteral: return RGB(196, 26, 22);  // red
        case sqlcore::SyntaxToken::Comment: return RGB(34, 139, 34);    // green
    }
    return GetSysColor(COLOR_WINDOWTEXT);
}

// Editor text with RichEdit's single-CR line breaks normalized to '\n' (1:1, so
// character offsets still line up with EM_EXSETSEL). SqlCore expects '\n'.
std::wstring editorText(HWND edit) {
    GETTEXTLENGTHEX gtl{};
    gtl.flags = GTL_NUMCHARS;
    gtl.codepage = 1200;  // UTF-16
    const LONG n = static_cast<LONG>(SendMessageW(edit, EM_GETTEXTLENGTHEX,
                                                  reinterpret_cast<WPARAM>(&gtl), 0));
    if (n <= 0) return std::wstring();
    std::wstring buf(static_cast<size_t>(n), L'\0');
    GETTEXTEX gt{};
    gt.cb = static_cast<DWORD>((n + 1) * sizeof(wchar_t));
    gt.flags = GT_DEFAULT;  // single '\r' line breaks
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
        CHARRANGE cr{static_cast<LONG>(s.location),
                     static_cast<LONG>(s.location + s.length)};
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

    int editH = st->editorHeight;
    int maxEdit = ch - kMinList - kSplitterHeight;
    if (maxEdit < kMinEditor) maxEdit = (std::max)(0, ch - kSplitterHeight);
    if (editH > maxEdit) editH = maxEdit;
    if (editH < 0) editH = 0;
    int listH = ch - editH - kSplitterHeight;
    if (listH < 0) listH = 0;

    MoveWindow(st->hList, 0, 0, cw, listH, TRUE);
    MoveWindow(st->hSplitter, 0, listH, cw, kSplitterHeight, TRUE);
    MoveWindow(st->hEdit, 0, listH + kSplitterHeight, cw, editH, TRUE);
}

void clearGrid(HWND list) {
    ListView_DeleteAllItems(list);
    while (ListView_DeleteColumn(list, 0)) {
    }
}

void populateGrid(HWND list, const QueryResult& r) {
    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    clearGrid(list);
    for (size_t i = 0; i < r.columns.size(); ++i) {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        col.cx = 140;
        col.iSubItem = static_cast<int>(i);
        col.pszText = const_cast<LPWSTR>(r.columns[i].c_str());
        ListView_InsertColumn(list, static_cast<int>(i), &col);
    }
    for (size_t row = 0; row < r.rows.size(); ++row) {
        const auto& cells = r.rows[row];
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(row);
        item.pszText = cells.empty() ? const_cast<LPWSTR>(L"")
                                     : const_cast<LPWSTR>(cells[0].c_str());
        ListView_InsertItem(list, &item);
        for (size_t c = 1; c < cells.size(); ++c)
            ListView_SetItemText(list, static_cast<int>(row), static_cast<int>(c),
                                 const_cast<LPWSTR>(cells[c].c_str()));
    }
    for (size_t i = 0; i < r.columns.size(); ++i)
        ListView_SetColumnWidth(list, static_cast<int>(i), LVSCW_AUTOSIZE_USEHEADER);
    SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list, nullptr, TRUE);
}

void showResult(AppState* st, const QueryResult& r) {
    wchar_t buf[160];
    if (!r.columns.empty()) {
        populateGrid(st->hList, r);
        std::swprintf(buf, 160, L"%llu rows · %.0f ms",
                      static_cast<unsigned long long>(r.rows.size()),
                      r.executionTimeSec * 1000.0);
    } else {
        clearGrid(st->hList);
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

void doRunWhole(AppState* st) {
    if (!st->session.isConnected()) {
        setStatus(st, L"No database — File ▸ Open Database… (Ctrl+O)");
        return;
    }
    if (st->busy) {
        setStatus(st, L"A query is already running — Ctrl+. to cancel.");
        return;
    }
    startExecute(st, editorText(st->hEdit));
}

void doRunStatement(AppState* st) {
    if (!st->session.isConnected()) {
        setStatus(st, L"No database — File ▸ Open Database… (Ctrl+O)");
        return;
    }
    if (st->busy) {
        setStatus(st, L"A query is already running — Ctrl+. to cancel.");
        return;
    }
    const std::wstring text = editorText(st->hEdit);
    const auto stmt =
        sqlcore::SqlStatementSplitter::statementAtOffset(caretOffset(st->hEdit), text);
    if (stmt) {
        startExecute(st, *stmt);
    } else {
        setStatus(st, L"No statement under the cursor.");
    }
}

void doCancel(AppState* st) {
    if (!st->busy) return;
    st->cancelRequested = true;
    st->session.cancel();
    setStatus(st, L"Cancelling…");
}

void doOpen(AppState* st) {
    if (st->busy) {
        setStatus(st, L"A query is running — Ctrl+. to cancel before opening.");
        return;
    }
    wchar_t file[1024] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = st->hwnd;
    ofn.lpstrFilter = L"SQLite Database\0*.db;*.sqlite;*.sqlite3\0All Files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = 1024;
    ofn.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST;
    ofn.lpstrTitle = L"Open or Create SQLite Database";
    if (!GetOpenFileNameW(&ofn)) return;

    st->pendingPath = file;
    DatabaseConnection cfg;
    cfg.engine = DatabaseEngine::Sqlite;
    cfg.filePath = file;
    setStatus(st, L"Connecting…");
    HWND hwnd = st->hwnd;
    st->session.connectAsync(cfg, [hwnd](bool ok, std::wstring err) {
        auto* m = new ConnectMsg{ok, std::move(err)};
        if (!PostMessageW(hwnd, WM_APP_CONNECTED, 0, reinterpret_cast<LPARAM>(m))) delete m;
    });
}

void createChildren(AppState* st, HINSTANCE hInst) {
    st->hList = CreateWindowExW(0, WC_LISTVIEW, L"",
                                WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS,
                                0, 0, 0, 0, st->hwnd,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LIST)),
                                hInst, nullptr);
    ListView_SetExtendedListViewStyle(
        st->hList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

    st->hSplitter = CreateWindowExW(0, kSplitterClass, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0,
                                    0, st->hwnd,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SPLIT)),
                                    hInst, nullptr);
    SetWindowLongPtrW(st->hSplitter, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));

    st->hEdit = CreateWindowExW(
        WS_EX_CLIENTEDGE, MSFTEDIT_CLASS, L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL |
            ES_WANTRETURN | ES_NOHIDESEL,
        0, 0, 0, 0, st->hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EDIT)),
        hInst, nullptr);
    SendMessageW(st->hEdit, EM_SETBKGNDCOLOR, 0,
                 static_cast<LPARAM>(GetSysColor(COLOR_WINDOW)));
    CHARFORMAT2W cf{};
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_FACE | CFM_SIZE | CFM_COLOR;
    cf.yHeight = 220;  // ~11pt (twips)
    cf.crTextColor = GetSysColor(COLOR_WINDOWTEXT);
    lstrcpynW(cf.szFaceName, L"Consolas", LF_FACESIZE);
    SendMessageW(st->hEdit, EM_SETCHARFORMAT, SCF_ALL, reinterpret_cast<LPARAM>(&cf));
    SetWindowTextW(st->hEdit, L"SELECT * FROM users;");
    SendMessageW(st->hEdit, EM_SETEVENTMASK, 0, ENM_CHANGE);
    applyHighlight(st);

    st->hStatus = CreateWindowExW(0, STATUSCLASSNAMEW, L"Ready", WS_CHILD | WS_VISIBLE,
                                  0, 0, 0, 0, st->hwnd,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STATUS)),
                                  hInst, nullptr);
    setStatus(st, L"No database — File ▸ Open Database… (Ctrl+O)");
}

void buildMenu(HWND hwnd) {
    HMENU bar = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, ID_OPEN, L"&Open Database…\tCtrl+O");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, ID_EXIT, L"E&xit");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");
    HMENU query = CreatePopupMenu();
    AppendMenuW(query, MF_STRING, ID_RUN, L"&Run\tCtrl+E");
    AppendMenuW(query, MF_STRING, ID_RUN_STMT, L"Run &Statement at Cursor\tCtrl+Enter");
    AppendMenuW(query, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(query, MF_STRING, ID_CANCEL, L"&Cancel Running Query\tCtrl+.");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(query), L"&Query");
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

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* st = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            auto* state = reinterpret_cast<AppState*>(cs->lpCreateParams);
            state->hwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        case WM_CREATE:
            createChildren(st, reinterpret_cast<LPCREATESTRUCTW>(lParam)->hInstance);
            buildMenu(hwnd);
            layout(st);
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
            if (reinterpret_cast<HWND>(lParam) == st->hEdit && code == EN_CHANGE) {
                applyHighlight(st);
            } else if (id == ID_OPEN) {
                doOpen(st);
            } else if (id == ID_RUN) {
                doRunWhole(st);
            } else if (id == ID_RUN_STMT) {
                doRunStatement(st);
            } else if (id == ID_CANCEL) {
                doCancel(st);
            } else if (id == ID_EXIT) {
                DestroyWindow(hwnd);
            }
            return 0;
        }
        case WM_APP_RESULT: {
            auto* r = reinterpret_cast<QueryResult*>(lParam);
            st->busy = false;
            if (st->cancelRequested) {
                setStatus(st, L"Query cancelled.");
            } else if (r->error) {
                setStatus(st, L"Error: " + *r->error);
            } else {
                showResult(st, *r);
            }
            st->cancelRequested = false;
            delete r;
            return 0;
        }
        case WM_APP_CONNECTED: {
            auto* m = reinterpret_cast<ConnectMsg*>(lParam);
            if (m->ok) {
                const std::wstring& path = st->pendingPath;
                const size_t slash = path.find_last_of(L"\\/");
                st->dbName = (slash == std::wstring::npos) ? path : path.substr(slash + 1);
                clearGrid(st->hList);
                setTitle(st);
                setStatus(st, st->session.statusMessage());
            } else {
                MessageBoxW(hwnd, m->error.c_str(), L"Connection failed",
                            MB_ICONERROR | MB_OK);
                setStatus(st, L"Connection failed.");
            }
            delete m;
            return 0;
        }
        case WM_SETFOCUS:
            if (st && st->hEdit) SetFocus(st->hEdit);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

int runApp(HINSTANCE hInstance, int nCmdShow) {
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_WIN95_CLASSES | ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES |
                ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    // RichEdit 4.1 (MSFTEDIT_CLASS / "RICHEDIT50W") lives in Msftedit.dll.
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

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (!RegisterClassExW(&wc)) return 1;

    AppState state;
    HWND hwnd = CreateWindowExW(0, kClassName, L"SQLTerminal", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 1100, 750, nullptr, nullptr,
                                hInstance, &state);
    if (!hwnd) return 1;
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    ACCEL accels[] = {
        {FCONTROL | FVIRTKEY, static_cast<WORD>('E'), ID_RUN},
        {FCONTROL | FVIRTKEY, static_cast<WORD>('O'), ID_OPEN},
        {FCONTROL | FVIRTKEY, VK_RETURN, ID_RUN_STMT},
        {FCONTROL | FVIRTKEY, VK_OEM_PERIOD, ID_CANCEL},
    };
    HACCEL hAccel = CreateAcceleratorTableW(accels, static_cast<int>(sizeof(accels) /
                                                                     sizeof(accels[0])));

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!TranslateAcceleratorW(hwnd, hAccel, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (hAccel) DestroyAcceleratorTable(hAccel);
    return static_cast<int>(msg.wParam);
}

}  // namespace sqlterm
