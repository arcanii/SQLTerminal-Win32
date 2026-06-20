// SPDX-License-Identifier: GPL-3.0-or-later
// P0 skeleton: a single empty main window with Common Controls v6 enabled.
// Real UI (editor, results grid, schema tree, status bar) arrives in later
// phases; this just proves the GUI target builds, links, and runs.
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <commctrl.h>

#pragma comment(lib, "comctl32.lib")
// Pull in the Common Controls v6 (themed) side-by-side assembly.
#pragma comment(                                                          \
    linker,                                                               \
    "\"/manifestdependency:type='win32' "                                \
    "name='Microsoft.Windows.Common-Controls' version='6.0.0.0' "        \
    "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' "        \
    "language='*'\"")

namespace {

constexpr wchar_t kWindowClass[] = L"SQLTerminalMainWindow";

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            SetBkMode(dc, TRANSPARENT);
            const wchar_t* text = L"SQLTerminal (Win32) — P0 skeleton";
            DrawTextW(dc, text, -1, &rc,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_WIN95_CLASSES | ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES |
                ICC_TREEVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kWindowClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (!RegisterClassExW(&wc)) return 1;

    HWND hwnd = CreateWindowExW(0, kWindowClass, L"SQLTerminal",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                1100, 750, nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) return 1;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}
