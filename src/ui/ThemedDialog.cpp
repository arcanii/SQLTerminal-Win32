// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/ThemedDialog.h"

#include <algorithm>

#include "ui/Theme.h"

namespace sqlterm {

namespace {

struct MsgState {
    bool done = false;
    int result = IDCANCEL;
    HICON icon = nullptr;
    int iconX = 0, iconY = 0, iconSize = 0;
};

LRESULT CALLBACK MsgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* st = reinterpret_cast<MsgState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect(reinterpret_cast<HDC>(wParam), &rc, themeBrush(currentTheme().panelBg));
            return 1;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            if (st && st->icon)
                DrawIconEx(hdc, st->iconX, st->iconY, st->icon, st->iconSize, st->iconSize, 0, nullptr,
                           DI_NORMAL);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            return dialogCtlColor(msg, wParam);
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            if (id == IDOK || id == IDYES || id == IDNO || id == IDCANCEL) {
                if (st) {
                    st->result = id;
                    st->done = true;
                }
            }
            return 0;
        }
        case WM_CLOSE:
            if (st) st->done = true;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

HICON iconFor(UINT type) {
    switch (type & 0xF0) {
        case MB_ICONERROR: return LoadIconW(nullptr, IDI_ERROR);
        case MB_ICONWARNING: return LoadIconW(nullptr, IDI_WARNING);
        case MB_ICONINFORMATION: return LoadIconW(nullptr, IDI_INFORMATION);
        case MB_ICONQUESTION: return LoadIconW(nullptr, IDI_QUESTION);
        default: return nullptr;
    }
}

}  // namespace

int themedMessageBox(HWND owner, const std::wstring& text, const std::wstring& caption, UINT type) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = MsgProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"SqlThemedMsg";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;  // painted per current theme in WM_ERASEBKGND
        RegisterClassExW(&wc);
        registered = true;
    }

    const UINT dpi = owner ? GetDpiForWindow(owner) : GetDpiForSystem();
    auto S = [dpi](int v) { return dpiScale(v, dpi); };

    MsgState st;
    st.icon = iconFor(type);
    st.iconSize = st.icon ? S(32) : 0;

    HFONT ui = CreateFontW(-S(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           VARIABLE_PITCH | FF_SWISS, L"Segoe UI");

    const int margin = S(18);
    const int iconGap = st.icon ? S(14) : 0;
    const int textW = S(300);

    // Measure the wrapped message height.
    int textH = S(20);
    if (HDC mdc = GetDC(nullptr)) {
        HGDIOBJ of = SelectObject(mdc, ui);
        RECT mr{0, 0, textW, 0};
        DrawTextW(mdc, text.c_str(), -1, &mr, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        textH = mr.bottom;
        SelectObject(mdc, of);
        ReleaseDC(nullptr, mdc);
    }

    const int btnW = S(90), btnH = S(28);
    const int textX = margin + st.iconSize + iconGap;
    const int contentH = (std::max)(textH, st.iconSize);
    const int btnY = margin + contentH + S(18);
    const int clientW = textX + textW + margin;
    const int clientH = btnY + btnH + margin;
    st.iconX = margin;
    st.iconY = margin;

    RECT wr{0, 0, clientW, clientH};
    AdjustWindowRectExForDpi(&wr, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME, dpi);
    const int fullW = wr.right - wr.left, fullH = wr.bottom - wr.top;

    RECT orc{};
    if (owner)
        GetWindowRect(owner, &orc);
    else
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &orc, 0);
    const int x = orc.left + ((orc.right - orc.left) - fullW) / 2;
    const int y = orc.top + ((orc.bottom - orc.top) - fullH) / 2;

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"SqlThemedMsg", caption.c_str(),
                                WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, fullW, fullH, owner, nullptr,
                                GetModuleHandleW(nullptr), &st);
    if (!hwnd) {
        DeleteObject(ui);
        return IDCANCEL;
    }

    HWND label = CreateWindowExW(0, L"STATIC", text.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT, textX,
                                 margin, textW, textH, hwnd, nullptr, GetModuleHandleW(nullptr),
                                 nullptr);
    SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(ui), TRUE);

    // Buttons, right-aligned; the affirmative button is the rightmost default.
    const int rightX = clientW - margin;
    auto addButton = [&](const wchar_t* lbl, int id, int slotFromRight, bool def) {
        const int bx = rightX - (slotFromRight + 1) * btnW - slotFromRight * S(8);
        HWND b = CreateWindowExW(
            0, L"BUTTON", lbl,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | (def ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON), bx, btnY,
            btnW, btnH, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            GetModuleHandleW(nullptr), nullptr);
        SendMessageW(b, WM_SETFONT, reinterpret_cast<WPARAM>(ui), TRUE);
        return b;
    };

    HWND defBtn = nullptr;
    const UINT buttons = type & 0xF;
    if (buttons == MB_YESNO) {
        addButton(L"No", IDNO, 1, false);
        defBtn = addButton(L"Yes", IDYES, 0, true);
    } else if (buttons == MB_OKCANCEL) {
        addButton(L"Cancel", IDCANCEL, 1, false);
        defBtn = addButton(L"OK", IDOK, 0, true);
    } else {
        defBtn = addButton(L"OK", IDOK, 0, true);
    }

    applyDialogDarkMode(hwnd);
    EnableWindow(owner, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    if (defBtn) SetFocus(defBtn);

    MSG msg;
    while (!st.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    EnableWindow(owner, TRUE);
    if (owner) SetForegroundWindow(owner);
    DestroyWindow(hwnd);
    DeleteObject(ui);
    return st.result;
}

}  // namespace sqlterm
