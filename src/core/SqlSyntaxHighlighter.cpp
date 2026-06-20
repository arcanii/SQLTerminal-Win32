// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/SqlSyntaxHighlighter.h"

#include <algorithm>
#include <unordered_set>

#include "core/SqlLiteralScanner.h"
#include "core/SqlText.h"

namespace sqlcore {

namespace {

using text::asciiUpper;
using text::isWordChar;

// Keyword set transcribed verbatim from SQLEditorView.swift (upper-case).
const std::unordered_set<std::wstring>& keywords() {
    static const std::unordered_set<std::wstring> k = {
        L"ABORT", L"ADD", L"ALL", L"ALTER", L"AND", L"AS", L"ASC", L"ATTACH",
        L"AUTOINCREMENT", L"BEGIN", L"BETWEEN", L"BY", L"CALL", L"CASCADE", L"CASE",
        L"CAST", L"CHECK", L"CLUSTER", L"COLLATE", L"COLUMN", L"COMMENT", L"COMMIT",
        L"CONSTRAINT", L"COPY", L"CREATE", L"CROSS", L"CURRENT", L"DATABASE",
        L"DEFAULT", L"DEFERRABLE", L"DELETE", L"DESC", L"DETACH", L"DISTINCT", L"DO",
        L"DROP", L"ELSE", L"END", L"ESCAPE", L"EXCEPT", L"EXISTS", L"EXPLAIN",
        L"FALSE", L"FOREIGN", L"FROM", L"FULL", L"GRANT", L"GROUP", L"HAVING", L"IF",
        L"ILIKE", L"IN", L"INDEX", L"INNER", L"INSERT", L"INTO", L"IS", L"JOIN",
        L"KEY", L"LEFT", L"LIKE", L"LIMIT", L"MERGE", L"NATURAL", L"NOT", L"NULL",
        L"OFFSET", L"ON", L"OR", L"ORDER", L"OUTER", L"PRAGMA", L"PRIMARY",
        L"REFERENCES", L"REINDEX", L"RENAME", L"REPLACE", L"RETURNING", L"REVOKE",
        L"RIGHT", L"ROLLBACK", L"SAVEPOINT", L"SELECT", L"SET", L"SHOW", L"TABLE",
        L"TEMP", L"TEMPORARY", L"THEN", L"TRIGGER", L"TRUE", L"TRUNCATE", L"UNION",
        L"UNIQUE", L"UPDATE", L"USING", L"VACUUM", L"VALUES", L"VIEW", L"WHEN",
        L"WHERE", L"WITH",
    };
    return k;
}

bool isDigit(wchar_t c) { return c >= L'0' && c <= L'9'; }

}  // namespace

std::vector<HighlightSpan> SqlSyntaxHighlighter::computeSpans(const std::wstring& text) {
    const size_t n = text.size();
    const auto literals = SqlLiteralScanner::literalAndCommentRanges(text);

    auto overlapsLiteral = [&](size_t start, size_t end) {
        for (const auto& r : literals) {
            const size_t rs = r.location, re = r.location + r.length;
            if (start < re && rs < end) return true;  // half-open intervals overlap
        }
        return false;
    };

    std::vector<HighlightSpan> spans;

    // Literals/comments are authoritative.
    for (const auto& r : literals) {
        spans.push_back({r.location, r.length,
                         r.isComment ? SyntaxToken::Comment : SyntaxToken::StringLiteral});
    }

    // Keywords: maximal word runs whose upper-case form is a keyword.
    for (size_t i = 0; i < n;) {
        if (!isWordChar(text[i])) {
            ++i;
            continue;
        }
        const size_t start = i;
        std::wstring upper;
        while (i < n && isWordChar(text[i])) {
            upper.push_back(asciiUpper(text[i]));
            ++i;
        }
        if (keywords().count(upper) && !overlapsLiteral(start, i)) {
            spans.push_back({start, i - start, SyntaxToken::Keyword});
        }
    }

    // Numbers: \b\d+(\.\d+)?\b
    for (size_t i = 0; i < n;) {
        const bool prevIsWord = (i > 0) && isWordChar(text[i - 1]);
        if (isDigit(text[i]) && !prevIsWord) {
            const size_t start = i;
            size_t j = i + 1;
            while (j < n && isDigit(text[j])) ++j;
            if (j + 1 < n && text[j] == L'.' && isDigit(text[j + 1])) {
                j += 1;
                while (j < n && isDigit(text[j])) ++j;
            }
            const bool endBoundary = (j >= n) || !isWordChar(text[j]);
            if (endBoundary) {
                if (!overlapsLiteral(start, j))
                    spans.push_back({start, j - start, SyntaxToken::Number});
                i = j;
            } else {
                // e.g. "123abc" — not a number; skip the whole word run.
                i = j;
                while (i < n && isWordChar(text[i])) ++i;
            }
        } else {
            ++i;
        }
    }

    std::sort(spans.begin(), spans.end(),
              [](const HighlightSpan& a, const HighlightSpan& b) {
                  return a.location < b.location;
              });
    return spans;
}

}  // namespace sqlcore
