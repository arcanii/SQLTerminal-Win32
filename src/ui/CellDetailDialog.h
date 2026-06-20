// SPDX-License-Identifier: GPL-3.0-or-later
// Modal cell-detail viewer (port of CellDetailView): expands one cell value, with
// a "Format JSON" toggle when the value is valid JSON, a character count, and Copy.
#pragma once

#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace sqlterm {

void showCellDetail(HWND owner, const std::wstring& column, const std::wstring& value);

}  // namespace sqlterm
