// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace sqlterm {

// Registers the window class, opens the main window, and runs the message loop.
// Returns the WM_QUIT exit code.
int runApp(HINSTANCE hInstance, int nCmdShow);

}  // namespace sqlterm
