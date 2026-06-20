// SPDX-License-Identifier: GPL-3.0-or-later
// SqlLiteralScanner — finds the string-literal and comment spans in SQL, with
// UTF-16 ranges so they map directly onto the RichEdit editor buffer. Honours
// '' escapes and dollar-quoting, and the rule that a `--` inside a string isn't
// a comment (and a `'` inside a comment isn't a string).
//
// Byte-for-byte port of SQLTerminal/Core/SQLLiteralScanner.swift. Note: the
// dollar-tag charset here is ASCII-only [A-Za-z0-9_], matching the Swift scanner.
#pragma once

#include <string>
#include <vector>

namespace sqlcore {

// A single-quoted string / dollar-quoted block, or a comment, with its UTF-16
// range (location + length) in the source.
struct SqlLiteralRange {
    size_t location;
    size_t length;
    bool isComment;  // true = -- or /* */ comment; false = '...' or $tag$...$tag$
};

inline bool operator==(const SqlLiteralRange& a, const SqlLiteralRange& b) {
    return a.location == b.location && a.length == b.length &&
           a.isComment == b.isComment;
}
inline bool operator!=(const SqlLiteralRange& a, const SqlLiteralRange& b) {
    return !(a == b);
}

class SqlLiteralScanner {
public:
    static std::vector<SqlLiteralRange> literalAndCommentRanges(const std::wstring& text);
};

}  // namespace sqlcore
