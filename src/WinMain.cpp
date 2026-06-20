// SPDX-License-Identifier: GPL-3.0-or-later
// Entry point. Enables Common Controls v6 (themed ListView/TreeView/etc.) and
// hands off to the UI layer.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "ui/MainWindow.h"

#pragma comment(lib, "comctl32.lib")
// Common Controls v6 + DPI awareness come from the embedded app manifest
// (packaging/app.manifest via SQLTerminal.rc).

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    return sqlterm::runApp(hInstance, nCmdShow);
}
