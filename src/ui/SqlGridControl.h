// SPDX-License-Identifier: GPL-3.0-or-later
// SqlGridControl — a GPU-accelerated (Direct2D + DirectWrite) results grid that
// replaces the virtual ListView. It is a real child window (class "SqlD2DGrid")
// stored in AppState::hList, so layout()/SetFocus/DeferWindowPos are unchanged.
//
// The control owns everything the old ListView + its handlers did: the result
// data (a copy of QueryResult), the sort state + row-order permutation (via
// sqlapp ResultFormat::sortedRowOrder/smartCompare), per-column widths,
// selection, two-axis scrolling, the dark header with click-sort + sort arrows,
// alternating rows, the per-cell right-click menu (View / Copy value / Copy row /
// Copy all as TSV / CSV — using ResultFormat + CellDetailDialog), and column
// resize. Colors come from Theme.h; it shares the D2D factories in D2DSupport.h.
#pragma once

#include <windows.h>

#include <string>

#include "models/QueryResult.h"

namespace sqlterm {

// Register the "SqlD2DGrid" window class once.
bool registerSqlGridClass(HINSTANCE hInst);

// Populate the grid from a query result (replaces the old setGridResult): copies
// the data, resets sort/scroll/selection, and auto-sizes columns to content.
void gridSetResult(HWND grid, const QueryResult& result);

// Clear the grid to the empty state (replaces clearGrid).
void gridClear(HWND grid);

// Re-read currentTheme(): rebuild brushes + scrollbar theme and repaint.
void gridApplyTheme(HWND grid);

// Rebuild text formats/metrics at the new DPI (scales column widths) and repaint.
void gridUpdateDpi(HWND grid, UINT dpi);

// Set the full-text row filter (case-insensitive; matches any column). Empty text
// clears it. Rebuilds the displayed row order = filter applied over the current sort.
void gridSetFilter(HWND grid, const std::wstring& text);

// Filtered (shown) and total data-row counts, for a "N of M" readout.
void gridGetCounts(HWND grid, int& shown, int& total);

}  // namespace sqlterm
