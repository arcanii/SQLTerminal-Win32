// SPDX-License-Identifier: GPL-3.0-or-later
// A dark-themed modal message box — a drop-in replacement for MessageBoxW with
// the same argument order and IDOK/IDYES/IDNO/IDCANCEL return codes. Honors the
// MB_OK / MB_OKCANCEL / MB_YESNO button flags and the MB_ICON{ERROR,WARNING,
// INFORMATION,QUESTION} icon flags. Lives in its own TU so dialogs outside
// MainWindow (e.g. HistoryDialog) can use it.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>

namespace sqlterm {

int themedMessageBox(HWND owner, const std::wstring& text, const std::wstring& caption, UINT type);

}  // namespace sqlterm
