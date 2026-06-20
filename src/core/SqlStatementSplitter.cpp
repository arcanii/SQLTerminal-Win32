// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/SqlStatementSplitter.h"

#include "core/SqlText.h"

namespace sqlcore {

using text::isWordChar;
using text::trimWhitespace;

std::vector<Segment> SqlStatementSplitter::segments(const std::wstring& sql) {
    std::vector<Segment> segs;
    std::wstring current;
    size_t segmentStart = 0;
    bool inDollarQuote = false;
    std::wstring dollarTag;
    bool inSingleQuote = false;
    bool inLineComment = false;
    bool inBlockComment = false;
    const size_t count = sql.size();
    size_t i = 0;

    auto emit = [&](size_t end) {
        segs.push_back(Segment{trimWhitespace(current), segmentStart, end});
        current.clear();
        segmentStart = end;
    };

    while (i < count) {
        const wchar_t c = sql[i];
        const bool hasNext = (i + 1 < count);
        const wchar_t next = hasNext ? sql[i + 1] : 0;

        // Line comment
        if (!inSingleQuote && !inDollarQuote && !inBlockComment && c == L'-' &&
            hasNext && next == L'-') {
            inLineComment = true;
            current.push_back(c);
            i += 1;
            continue;
        }
        if (inLineComment) {
            current.push_back(c);
            if (c == L'\n') inLineComment = false;
            i += 1;
            continue;
        }

        // Block comment
        if (!inSingleQuote && !inDollarQuote && !inBlockComment && c == L'/' &&
            hasNext && next == L'*') {
            inBlockComment = true;
            current.push_back(c);
            i += 1;
            continue;
        }
        if (inBlockComment) {
            current.push_back(c);
            if (c == L'*' && hasNext && next == L'/') {
                current.push_back(next);
                inBlockComment = false;
                i += 2;
                continue;
            }
            i += 1;
            continue;
        }

        // Dollar quoting: $tag$ ... $tag$
        if (!inSingleQuote && c == L'$') {
            std::wstring tag = L"$";
            size_t j = i + 1;
            while (j < count && isWordChar(sql[j])) {
                tag.push_back(sql[j]);
                j += 1;
            }
            if (j < count && sql[j] == L'$') {
                tag.push_back(L'$');
                if (inDollarQuote && tag == dollarTag) {
                    current.append(tag);
                    inDollarQuote = false;
                    dollarTag.clear();
                    i = j + 1;
                    continue;
                } else if (!inDollarQuote) {
                    inDollarQuote = true;
                    dollarTag = tag;
                    current.append(tag);
                    i = j + 1;
                    continue;
                }
            }
        }

        if (inDollarQuote) {
            current.push_back(c);
            i += 1;
            continue;
        }

        // Single quotes (with '' escape)
        if (c == L'\'' && !inDollarQuote) {
            inSingleQuote = !inSingleQuote;
            if (inSingleQuote == false && hasNext && next == L'\'') {
                current.push_back(c);
                current.push_back(next);
                inSingleQuote = true;
                i += 2;
                continue;
            }
            current.push_back(c);
            i += 1;
            continue;
        }

        if (inSingleQuote) {
            current.push_back(c);
            i += 1;
            continue;
        }

        // Semicolon — statement boundary
        if (c == L';') {
            current.push_back(c);
            emit(i + 1);
            i += 1;
            continue;
        }

        current.push_back(c);
        i += 1;
    }

    // Trailing run (no terminating semicolon)
    emit(count);
    return segs;
}

std::vector<std::wstring> SqlStatementSplitter::split(const std::wstring& sql) {
    std::vector<std::wstring> out;
    for (const auto& seg : segments(sql)) {
        if (!seg.text.empty()) out.push_back(seg.text);
    }
    return out;
}

std::optional<std::wstring> SqlStatementSplitter::statementAtOffset(
    long long cursorOffset, const std::wstring& sql) {
    const auto segs = segments(sql);
    if (segs.empty()) return std::nullopt;

    // Prefer the segment whose range strictly contains the cursor; on an exact
    // boundary, favour the segment ending there (so a cursor just past a ';'
    // still targets the statement you just finished typing).
    const Segment* hit = nullptr;
    for (const auto& s : segs) {
        if (cursorOffset >= static_cast<long long>(s.start) &&
            cursorOffset < static_cast<long long>(s.end)) {
            hit = &s;
            break;
        }
    }
    if (!hit) {
        for (auto it = segs.rbegin(); it != segs.rend(); ++it) {
            if (cursorOffset >= static_cast<long long>(it->start) &&
                cursorOffset <= static_cast<long long>(it->end)) {
                hit = &(*it);
                break;
            }
        }
    }
    if (!hit) hit = &segs.back();
    if (hit->text.empty()) return std::nullopt;
    return hit->text;
}

}  // namespace sqlcore
