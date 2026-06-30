// SPDX-License-Identifier: GPL-3.0-or-later
// Pure result-grid helpers ported from ResultsTableView.swift: the sort
// comparator (numeric-aware, NULL-last, natural), a stable row-order permutation
// for the virtual ListView, CSV/TSV export, and JSON pretty-printing.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace sqlterm {

// -1 / 0 / +1. Equal strings are 0; "NULL" sorts after everything; numeric when
// both parse as numbers; otherwise a natural (digit-aware) compare.
int smartCompare(const std::wstring& a, const std::wstring& b);

// Stable display order of row indices sorted by `column` (smartCompare); ties keep
// original order. Used to drive the virtual ListView without reordering the data.
std::vector<size_t> sortedRowOrder(const std::vector<std::vector<std::wstring>>& rows,
                                   size_t column, bool ascending);

// True if any cell of `row` contains `needleLower` (case-insensitive substring).
// `needleLower` must already be lower-cased; an empty needle matches every row.
// Drives the results-grid full-text row filter.
bool rowMatchesFilter(const std::vector<std::wstring>& row, const std::wstring& needleLower);

std::wstring csvEscape(const std::wstring& value);
std::wstring buildTsv(const std::vector<std::wstring>& columns,
                      const std::vector<std::vector<std::wstring>>& rows);
std::wstring buildCsv(const std::vector<std::wstring>& columns,
                      const std::vector<std::vector<std::wstring>>& rows);

// Pretty-printed (sorted-keys, 2-space) JSON if `value` is valid JSON, else nullopt.
std::optional<std::wstring> prettyPrintJson(const std::wstring& value);

}  // namespace sqlterm
