// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/SqlLiteralScanner.h"

#include <algorithm>
#include <optional>

namespace sqlcore {

namespace {

// The `$tag$` opener starting at `i` (e.g. L"$$", L"$body$"), or nullopt.
// ASCII-only tag charset, exactly as the Swift scanner.
std::optional<std::wstring> dollarTag(const std::wstring& s, size_t i) {
    const size_t n = s.size();
    if (i >= n || s[i] != 0x24 /* $ */) return std::nullopt;
    size_t j = i + 1;
    while (j < n) {
        const wchar_t ch = s[j];
        const bool isWord = (ch >= 0x41 && ch <= 0x5A) || (ch >= 0x61 && ch <= 0x7A) ||
                            (ch >= 0x30 && ch <= 0x39) || ch == 0x5F;
        if (isWord) {
            j += 1;
        } else {
            break;
        }
    }
    if (j >= n || s[j] != 0x24) return std::nullopt;
    return s.substr(i, j - i + 1);
}

}  // namespace

std::vector<SqlLiteralRange> SqlLiteralScanner::literalAndCommentRanges(
    const std::wstring& text) {
    const size_t n = text.size();
    std::vector<SqlLiteralRange> ranges;
    size_t i = 0;

    while (i < n) {
        const wchar_t c = text[i];

        // Line comment: -- ... to end of line
        if (c == 0x2D && i + 1 < n && text[i + 1] == 0x2D) {
            const size_t start = i;
            i += 2;
            while (i < n && text[i] != 0x0A) i += 1;
            ranges.push_back(SqlLiteralRange{start, i - start, true});
            continue;
        }

        // Block comment: /* ... */
        if (c == 0x2F && i + 1 < n && text[i + 1] == 0x2A) {
            const size_t start = i;
            i += 2;
            while (i + 1 < n && !(text[i] == 0x2A && text[i + 1] == 0x2F)) i += 1;
            i = std::min(i + 2, n);
            ranges.push_back(SqlLiteralRange{start, i - start, true});
            continue;
        }

        // Single-quoted string with '' escape
        if (c == 0x27) {
            const size_t start = i;
            i += 1;
            while (i < n) {
                if (text[i] == 0x27) {
                    if (i + 1 < n && text[i + 1] == 0x27) {
                        i += 2;
                        continue;  // escaped quote
                    }
                    i += 1;
                    break;
                }
                i += 1;
            }
            ranges.push_back(SqlLiteralRange{start, i - start, false});
            continue;
        }

        // Dollar-quoted block: $tag$ ... $tag$
        if (c == 0x24) {
            if (auto tag = dollarTag(text, i)) {
                const size_t start = i;
                i += tag->size();
                while (i < n) {
                    if (text[i] == 0x24) {
                        if (auto close = dollarTag(text, i); close && *close == *tag) {
                            i += close->size();
                            break;
                        }
                    }
                    i += 1;
                }
                ranges.push_back(SqlLiteralRange{start, i - start, false});
                continue;
            }
        }

        i += 1;
    }
    return ranges;
}

}  // namespace sqlcore
