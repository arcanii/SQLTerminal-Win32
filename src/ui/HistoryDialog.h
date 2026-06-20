// SPDX-License-Identifier: GPL-3.0-or-later
// Modal History & Snippets panel (port of HistorySnippetsView): searchable list
// of past queries and saved snippets; load one into the editor, delete, or save
// the current editor text as a named snippet.
#pragma once

#include <optional>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace sqlterm {

// Returns the SQL the user chose to load into the editor, or nullopt.
std::optional<std::wstring> showHistorySnippets(HWND owner, const std::wstring& currentSql);

}  // namespace sqlterm
