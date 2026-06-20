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
// Pull in the Common Controls v6 (themed) side-by-side assembly.
#pragma comment(                                                          \
    linker,                                                               \
    "\"/manifestdependency:type='win32' "                                \
    "name='Microsoft.Windows.Common-Controls' version='6.0.0.0' "        \
    "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' "        \
    "language='*'\"")

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    return sqlterm::runApp(hInstance, nCmdShow);
}
