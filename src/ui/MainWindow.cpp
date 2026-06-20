// SPDX-License-Identifier: GPL-3.0-or-later
// P1 main window: a results ListView (top), a SQL edit control (bottom), and a
// Run button. Connect via File > Open Database, run with Ctrl+E. Execution is
// synchronous on the UI thread here; P3 moves it to a worker thread.
#include "ui/MainWindow.h"

#include <commctrl.h>
#include <commdlg.h>  // GetOpenFileNameW / OPENFILENAMEW

#include <cstdio>
#include <string>

#include "db/SqliteProvider.h"
#include "models/DatabaseConnection.h"
#include "models/QueryResult.h"

namespace sqlterm {
namespace {

constexpr wchar_t kClassName[] = L"SQLTerminalMainWindow";

enum : int {
    ID_OPEN = 1001,
    ID_EXIT = 1002,
    ID_RUN = 1003,
    IDC_LIST = 2001,
    IDC_EDIT = 2002,
    IDC_RUNBTN = 2003,
};

constexpr int kEditHeight = 150;
constexpr int kStripHeight = 36;

struct AppState {
    HWND hwnd = nullptr;
    HWND hList = nullptr;
    HWND hEdit = nullptr;
    HWND hButton = nullptr;
    HFONT hMono = nullptr;
    SqliteProvider provider;
    std::wstring dbName;
};

std::wstring getEditText(HWND edit) {
    const int len = GetWindowTextLengthW(edit);
    std::wstring s;
    if (len <= 0) return s;
    s.resize(static_cast<size_t>(len));
    GetWindowTextW(edit, &s[0], len + 1);  // writes len chars + terminator
    return s;
}

void setTitle(AppState* st, const std::wstring& extra) {
    std::wstring title = L"SQLTerminal";
    if (!st->dbName.empty()) title += L" — " + st->dbName;
    if (!extra.empty()) title += L"   [" + extra + L"]";
    SetWindowTextW(st->hwnd, title.c_str());
}

void layout(AppState* st) {
    RECT rc;
    GetClientRect(st->hwnd, &rc);
    const int cw = rc.right;
    const int ch = rc.bottom;
    int listH = ch - kEditHeight - kStripHeight;
    if (listH < 0) listH = 0;
    MoveWindow(st->hList, 0, 0, cw, listH, TRUE);
    MoveWindow(st->hEdit, 0, listH, cw, kEditHeight, TRUE);
    MoveWindow(st->hButton, cw - 130, listH + kEditHeight + 5, 120, 26, TRUE);
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
        item.iSubItem = 0;
        item.pszText = cells.empty() ? const_cast<LPWSTR>(L"")
                                     : const_cast<LPWSTR>(cells[0].c_str());
        ListView_InsertItem(list, &item);
        for (size_t c = 1; c < cells.size(); ++c) {
            ListView_SetItemText(list, static_cast<int>(row), static_cast<int>(c),
                                 const_cast<LPWSTR>(cells[c].c_str()));
        }
    }
    for (size_t i = 0; i < r.columns.size(); ++i) {
        ListView_SetColumnWidth(list, static_cast<int>(i), LVSCW_AUTOSIZE_USEHEADER);
    }
    SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list, nullptr, TRUE);
}

void doOpen(AppState* st) {
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

    DatabaseConnection cfg;
    cfg.engine = DatabaseEngine::Sqlite;
    cfg.filePath = file;
    std::wstring err;
    if (!st->provider.connect(cfg, err)) {
        MessageBoxW(st->hwnd, err.c_str(), L"Connection failed", MB_ICONERROR | MB_OK);
        return;
    }

    const std::wstring path = file;
    const size_t slash = path.find_last_of(L"\\/");
    st->dbName = (slash == std::wstring::npos) ? path : path.substr(slash + 1);
    clearGrid(st->hList);
    setTitle(st, L"connected");
}

void doRun(AppState* st) {
    if (!st->provider.isConnected()) {
        MessageBoxW(st->hwnd, L"Open a database first (File ▸ Open Database…).",
                    L"No connection", MB_ICONINFORMATION | MB_OK);
        return;
    }
    const QueryResult r = st->provider.execute(getEditText(st->hEdit));
    if (r.error) {
        MessageBoxW(st->hwnd, r.error->c_str(), L"Query error", MB_ICONERROR | MB_OK);
        setTitle(st, L"error");
        return;
    }
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
    setTitle(st, buf);
}

void createChildren(AppState* st, HINSTANCE hInst) {
    st->hList = CreateWindowExW(0, WC_LISTVIEW, L"",
                                WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS,
                                0, 0, 0, 0, st->hwnd,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LIST)),
                                hInst, nullptr);
    ListView_SetExtendedListViewStyle(
        st->hList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

    st->hEdit = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"SELECT 1;",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE |
            ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_WANTRETURN,
        0, 0, 0, 0, st->hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EDIT)),
        hInst, nullptr);

    st->hButton = CreateWindowExW(0, L"BUTTON", L"Run (Ctrl+E)",
                                  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0,
                                  st->hwnd,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_RUNBTN)),
                                  hInst, nullptr);

    st->hMono = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            FIXED_PITCH | FF_MODERN, L"Consolas");
    if (st->hMono) SendMessageW(st->hEdit, WM_SETFONT, reinterpret_cast<WPARAM>(st->hMono), TRUE);
    SendMessageW(st->hButton, WM_SETFONT,
                 reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
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
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(query), L"&Query");
    SetMenu(hwnd, bar);
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
            if (id == ID_OPEN) {
                doOpen(st);
            } else if (id == ID_RUN || id == IDC_RUNBTN) {
                doRun(st);
            } else if (id == ID_EXIT) {
                DestroyWindow(hwnd);
            }
            return 0;
        }
        case WM_DESTROY:
            if (st && st->hMono) DeleteObject(st->hMono);
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
    };
    HACCEL hAccel = CreateAcceleratorTableW(
        accels, static_cast<int>(sizeof(accels) / sizeof(accels[0])));

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
