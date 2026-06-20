// SPDX-License-Identifier: GPL-3.0-or-later
// SqlSyntaxHighlighter — computes the colored spans for the SQL editor: numbers,
// keywords, and string/comment literals. Pure and testable; the UI layer renders
// the returned spans onto a RichEdit control.
//
// Mirrors SQLSyntaxHighlighter in SQLTerminal/Views/SQLEditorView.swift: literal
// and comment spans (from SqlLiteralScanner) win over keyword/number coloring
// inside them. Offsets are UTF-16 code units (aligned with the editor buffer).
#pragma once

#include <string>
#include <vector>

namespace sqlcore {

enum class SyntaxToken {
    Keyword,
    Number,
    StringLiteral,
    Comment,
};

struct HighlightSpan {
    size_t location;
    size_t length;
    SyntaxToken type;
};

inline bool operator==(const HighlightSpan& a, const HighlightSpan& b) {
    return a.location == b.location && a.length == b.length && a.type == b.type;
}
inline bool operator!=(const HighlightSpan& a, const HighlightSpan& b) {
    return !(a == b);
}

class SqlSyntaxHighlighter {
public:
    // Non-overlapping colored spans, sorted by location. Regions with no span
    // render in the editor's default text color.
    static std::vector<HighlightSpan> computeSpans(const std::wstring& text);
};

}  // namespace sqlcore
