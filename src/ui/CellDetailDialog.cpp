// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/CellDetailDialog.h"

#include <cstring>
#include <optional>
#include <string>

#include "app/ResultFormat.h"

namespace sqlterm {
namespace {

constexpr wchar_t kClass[] = L"SQLTerminalCellDetail";
enum : int { IDC_VALUE = 100, IDC_JSON = 101, IDC_COUNT = 102, IDC_COPY = 103 };
constexpr int W = 540;
constexpr int H = 440;

struct State {
    HWND hwnd = nullptr;
    HFONT font = nullptr;
    std::wstring raw;
    std::optional<std::wstring> pretty;
    bool done = false;
};

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

std::wstring shownValue(State* st) {
    const bool formatted = st->pretty && SendMessageW(GetDlgItem(st->hwnd, IDC_JSON), BM_GETCHECK, 0, 0) == BST_CHECKED;
    return formatted ? *st->pretty : st->raw;
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
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            if (id == IDC_JSON) {
                SetWindowTextW(GetDlgItem(hwnd, IDC_VALUE), shownValue(st).c_str());
            } else if (id == IDC_COPY) {
                copyToClipboard(hwnd, shownValue(st));
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

void showCellDetail(HWND owner, const std::wstring& column, const std::wstring& value) {
    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = Proc;
        wc.hInstance = hInst;
        wc.lpszClassName = kClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        RegisterClassExW(&wc);
        registered = true;
    }

    State st;
    st.font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    st.raw = value;
    st.pretty = prettyPrintJson(value);

    RECT orc{};
    GetWindowRect(owner, &orc);
    const int fullW = W + 16, fullH = H + 39;
    const int x = orc.left + ((orc.right - orc.left) - fullW) / 2;
    const int y = orc.top + ((orc.bottom - orc.top) - fullH) / 2;

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kClass, column.c_str(),
                                WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, fullW, fullH, owner,
                                nullptr, hInst, &st);
    if (!hwnd) return;

    auto mk = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int cx, int cy, int cw,
                  int chh, int id) {
        HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, cx, cy, cw, chh, hwnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(st.font), TRUE);
        return c;
    };

    HWND value_edit = mk(L"EDIT", value.c_str(),
                         WS_BORDER | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY |
                             ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                         12, 12, W - 24, H - 84, IDC_VALUE);
    HFONT mono = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             FIXED_PITCH | FF_MODERN, L"Consolas");
    if (mono) SendMessageW(value_edit, WM_SETFONT, reinterpret_cast<WPARAM>(mono), TRUE);

    wchar_t count[64];
    swprintf(count, 64, L"%llu characters", static_cast<unsigned long long>(value.size()));
    mk(L"STATIC", count, SS_LEFT, 12, H - 64, 200, 18, IDC_COUNT);
    if (st.pretty) mk(L"BUTTON", L"Format JSON", BS_AUTOCHECKBOX, 220, H - 65, 120, 20, IDC_JSON);
    mk(L"BUTTON", L"Copy", BS_PUSHBUTTON | WS_TABSTOP, W - 200, H - 40, 90, 26, IDC_COPY);
    mk(L"BUTTON", L"Close", BS_DEFPUSHBUTTON | WS_TABSTOP, W - 104, H - 40, 90, 26, IDCANCEL);

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
    if (mono) DeleteObject(mono);
    DestroyWindow(hwnd);
}

}  // namespace sqlterm
