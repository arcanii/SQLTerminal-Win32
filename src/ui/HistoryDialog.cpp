// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/HistoryDialog.h"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "persistence/Stores.h"
#include "ui/ThemedDialog.h"
#include "ui/Theme.h"

namespace sqlterm {
namespace {

constexpr wchar_t kClass[] = L"SQLTerminalHistoryDialog";
enum : int {
    IDC_MODE_HIST = 100,
    IDC_MODE_SNIP = 101,
    IDC_SEARCH = 102,
    IDC_LIST = 103,
    IDC_SNIPNAME = 104,
    IDC_SAVESNIP = 105,
    IDC_LOAD = 106,
    IDC_DELETE = 107,
};
constexpr int W = 560;
constexpr int H = 480;

struct State {
    HWND hwnd = nullptr;
    UINT dpi = 96;
    HFONT font = nullptr;
    std::wstring currentSql;
    bool snippetsMode = false;
    std::vector<std::wstring> shownSql;  // parallel to list items: SQL to load
    std::vector<int> shownIndex;         // index into the underlying store list
    std::optional<std::wstring> result;
    bool done = false;
};

std::wstring lower(std::wstring s) {
    for (auto& c : s)
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c + 32);
    return s;
}
std::wstring getText(HWND c) {
    const int n = GetWindowTextLengthW(c);
    std::wstring s;
    if (n <= 0) return s;
    s.resize(static_cast<size_t>(n));
    GetWindowTextW(c, &s[0], n + 1);
    return s;
}
std::wstring oneLine(const std::wstring& sql, size_t maxLen = 120) {
    std::wstring s;
    for (wchar_t c : sql) s.push_back((c == L'\n' || c == L'\r' || c == L'\t') ? L' ' : c);
    if (s.size() > maxLen) s = s.substr(0, maxLen) + L"…";
    return s;
}

void repopulate(State* st) {
    HWND list = GetDlgItem(st->hwnd, IDC_LIST);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    st->shownSql.clear();
    st->shownIndex.clear();
    const std::wstring needle = lower(getText(GetDlgItem(st->hwnd, IDC_SEARCH)));

    if (st->snippetsMode) {
        const auto snips = SnippetStore::load();
        for (size_t i = 0; i < snips.size(); ++i) {
            if (!needle.empty() && lower(snips[i].name).find(needle) == std::wstring::npos &&
                lower(snips[i].sql).find(needle) == std::wstring::npos)
                continue;
            SendMessageW(list, LB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>((snips[i].name + L"  —  " + oneLine(snips[i].sql, 80)).c_str()));
            st->shownSql.push_back(snips[i].sql);
            st->shownIndex.push_back(static_cast<int>(i));
        }
    } else {
        const auto hist = QueryHistoryStore::load();
        for (size_t i = 0; i < hist.size(); ++i) {
            if (!needle.empty() && lower(hist[i].sql).find(needle) == std::wstring::npos) continue;
            SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(oneLine(hist[i].sql).c_str()));
            st->shownSql.push_back(hist[i].sql);
            st->shownIndex.push_back(static_cast<int>(i));
        }
    }

    // Snippet-save controls + the Delete button label depend on the mode.
    const int show = st->snippetsMode ? SW_SHOW : SW_HIDE;
    ShowWindow(GetDlgItem(st->hwnd, IDC_SNIPNAME), show);
    ShowWindow(GetDlgItem(st->hwnd, IDC_SAVESNIP), show);
    SetWindowTextW(GetDlgItem(st->hwnd, IDC_DELETE),
                   st->snippetsMode ? L"Delete" : L"Clear All");
}

void loadSelection(State* st) {
    const int sel = static_cast<int>(SendMessageW(GetDlgItem(st->hwnd, IDC_LIST), LB_GETCURSEL, 0, 0));
    if (sel >= 0 && sel < static_cast<int>(st->shownSql.size())) {
        st->result = st->shownSql[static_cast<size_t>(sel)];
        st->done = true;
    }
}

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* st = reinterpret_cast<State*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            auto* s = reinterpret_cast<State*>(cs->lpCreateParams);
            s->hwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLORBTN:
            return dialogCtlColor(msg, wParam);
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            const int code = HIWORD(wParam);
            if (id == IDC_MODE_HIST) {
                st->snippetsMode = false;
                repopulate(st);
            } else if (id == IDC_MODE_SNIP) {
                st->snippetsMode = true;
                repopulate(st);
            } else if (id == IDC_SEARCH && code == EN_CHANGE) {
                repopulate(st);
            } else if (id == IDC_LIST && code == LBN_DBLCLK) {
                loadSelection(st);
            } else if (id == IDC_LOAD) {
                loadSelection(st);
            } else if (id == IDC_SAVESNIP) {
                const std::wstring name = getText(GetDlgItem(hwnd, IDC_SNIPNAME));
                if (!name.empty() && !st->currentSql.empty()) {
                    SnippetStore::save(name, st->currentSql);
                    SetWindowTextW(GetDlgItem(hwnd, IDC_SNIPNAME), L"");
                    repopulate(st);
                }
            } else if (id == IDC_DELETE) {
                const int sel = static_cast<int>(SendMessageW(GetDlgItem(hwnd, IDC_LIST), LB_GETCURSEL, 0, 0));
                if (st->snippetsMode) {
                    if (sel >= 0 && sel < static_cast<int>(st->shownIndex.size())) {
                        const auto snips = SnippetStore::load();
                        const int idx = st->shownIndex[static_cast<size_t>(sel)];
                        if (idx >= 0 && idx < static_cast<int>(snips.size()))
                            SnippetStore::remove(snips[static_cast<size_t>(idx)]);
                        repopulate(st);
                    }
                } else if (themedMessageBox(hwnd, L"Clear all query history?", L"Confirm",
                                            MB_YESNO | MB_ICONWARNING) == IDYES) {
                    QueryHistoryStore::clear();
                    repopulate(st);
                }
            } else if (id == IDCANCEL) {
                st->done = true;
            }
            return 0;
        }
        case WM_CLOSE:
            st->done = true;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

std::optional<std::wstring> showHistorySnippets(HWND owner, const std::wstring& currentSql) {
    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = Proc;
        wc.hInstance = hInst;
        wc.lpszClassName = kClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = themeBrush(currentTheme().panelBg);
        RegisterClassExW(&wc);
        registered = true;
    }

    State st;
    st.currentSql = currentSql;

    const UINT odpi = GetDpiForWindow(owner);
    RECT orc{};
    GetWindowRect(owner, &orc);
    const int fullW = dpiScale(W + 16, odpi), fullH = dpiScale(H + 39, odpi);
    const int x = orc.left + ((orc.right - orc.left) - fullW) / 2;
    const int y = orc.top + ((orc.bottom - orc.top) - fullH) / 2;

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kClass, L"History & Snippets",
                                WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, fullW, fullH, owner,
                                nullptr, hInst, &st);
    if (!hwnd) return std::nullopt;
    st.dpi = GetDpiForWindow(hwnd);
    st.font = CreateFontW(-dpiScale(15, st.dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          VARIABLE_PITCH | FF_SWISS, L"Segoe UI");

    auto mk = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int cx, int cy, int cw,
                  int chh, int id) {
        HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, dpiScale(cx, st.dpi),
                                 dpiScale(cy, st.dpi), dpiScale(cw, st.dpi), dpiScale(chh, st.dpi),
                                 hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst,
                                 nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(st.font), TRUE);
        return c;
    };

    mk(L"BUTTON", L"History", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 12, 12, 90, 22, IDC_MODE_HIST);
    mk(L"BUTTON", L"Snippets", BS_AUTORADIOBUTTON | WS_TABSTOP, 106, 12, 90, 22, IDC_MODE_SNIP);
    SendMessageW(GetDlgItem(hwnd, IDC_MODE_HIST), BM_SETCHECK, BST_CHECKED, 0);
    mk(L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 210, 12, W - 222, 22, IDC_SEARCH);

    mk(L"LISTBOX", L"", LBS_NOTIFY | WS_VSCROLL | WS_BORDER | WS_TABSTOP, 12, 44, W - 24, H - 120,
       IDC_LIST);

    mk(L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 12, H - 64, 220, 22, IDC_SNIPNAME);
    mk(L"BUTTON", L"Save editor as snippet", BS_PUSHBUTTON | WS_TABSTOP, 238, H - 65, 180, 24,
       IDC_SAVESNIP);

    mk(L"BUTTON", L"Delete", BS_PUSHBUTTON | WS_TABSTOP, 12, H - 36, 90, 26, IDC_DELETE);
    mk(L"BUTTON", L"Load", BS_DEFPUSHBUTTON | WS_TABSTOP, W - 200, H - 36, 90, 26, IDC_LOAD);
    mk(L"BUTTON", L"Close", BS_PUSHBUTTON | WS_TABSTOP, W - 104, H - 36, 90, 26, IDCANCEL);

    repopulate(&st);

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
    if (st.font) DeleteObject(st.font);
    return st.result;
}

}  // namespace sqlterm
