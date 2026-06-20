// SPDX-License-Identifier: GPL-3.0-or-later
// SqlText.h — shared character helpers for the SqlCore algorithms.
//
// Ported from the macOS SQLTerminal `SQLCore` package. The whole of SqlCore
// works in UTF-16 code units (std::wstring / wchar_t on Windows), which is the
// single index space the editor (RichEdit) and the splitter/scanner share. For
// the all-ASCII inputs the original Swift tests exercise, UTF-16 code-unit
// offsets coincide with Swift's grapheme offsets.
//
// Deliberate simplification vs. the Swift original: Swift used Unicode-aware
// `Character.isLetter/isNumber` for word/dollar-tag characters in the splitter
// and classifier (and ASCII-only in the scanner). Here all three use the same
// ASCII [A-Za-z0-9_] definition. This matches every existing golden test; full
// Unicode identifier fidelity would require ICU and is out of scope.
#pragma once

#include <string>

namespace sqlcore::text {

// ASCII word character: [A-Za-z0-9_].
inline bool isWordChar(wchar_t c) {
    return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') ||
           (c >= L'0' && c <= L'9') || c == L'_';
}

// Locale-independent ASCII uppercase (keywords are all ASCII).
inline wchar_t asciiUpper(wchar_t c) {
    return (c >= L'a' && c <= L'z') ? static_cast<wchar_t>(c - 32) : c;
}

// Matches Swift's `.whitespacesAndNewlines` for the ASCII subset the splitter
// trims (space, tab, newline, carriage return, vertical tab, form feed).
inline bool isWhitespace(wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'\n' || c == L'\r' ||
           c == L'\v' || c == L'\f';
}

inline std::wstring trimWhitespace(const std::wstring& s) {
    size_t begin = 0, end = s.size();
    while (begin < end && isWhitespace(s[begin])) ++begin;
    while (end > begin && isWhitespace(s[end - 1])) --end;
    return s.substr(begin, end - begin);
}

}  // namespace sqlcore::text
