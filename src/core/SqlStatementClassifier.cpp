// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/SqlStatementClassifier.h"

#include <optional>
#include <unordered_set>

#include "core/SqlStatementSplitter.h"
#include "core/SqlText.h"

namespace sqlcore {

using text::asciiUpper;
using text::isWordChar;

namespace {

const std::unordered_set<std::wstring>& readKeywords() {
    static const std::unordered_set<std::wstring> k = {
        L"SELECT", L"TABLE", L"VALUES", L"SHOW", L"DESCRIBE", L"DESC", L"PRAGMA",
    };
    return k;
}
const std::unordered_set<std::wstring>& writeKeywords() {
    static const std::unordered_set<std::wstring> k = {
        L"INSERT", L"UPDATE", L"DELETE", L"MERGE", L"REPLACE", L"UPSERT",
        L"CREATE", L"DROP", L"ALTER", L"TRUNCATE", L"RENAME",
        L"GRANT", L"REVOKE", L"COMMENT", L"SECURITY",
        L"REINDEX", L"VACUUM", L"CLUSTER", L"COPY", L"CALL", L"DO",
        L"REFRESH", L"IMPORT", L"LOAD", L"ATTACH", L"DETACH",
    };
    return k;
}
const std::unordered_set<std::wstring>& neutralKeywords() {
    static const std::unordered_set<std::wstring> k = {
        L"BEGIN", L"START", L"COMMIT", L"ROLLBACK", L"END", L"ABORT",
        L"SAVEPOINT", L"RELEASE", L"SET", L"RESET", L"USE", L"DISCARD",
        L"LISTEN", L"UNLISTEN", L"NOTIFY", L"CHECKPOINT", L"ANALYZE",
        L"DEALLOCATE", L"PREPARE", L"EXECUTE", L"FETCH", L"MOVE", L"CLOSE", L"DECLARE",
    };
    return k;
}
const std::unordered_set<std::wstring>& dataModifyingKeywords() {
    static const std::unordered_set<std::wstring> k = {
        L"INSERT", L"UPDATE", L"DELETE", L"MERGE",
    };
    return k;
}

// If a `$tag$` opener starts at `i`, returns it (e.g. L"$$", L"$body$").
std::optional<std::wstring> dollarTagAt(const std::wstring& chars, size_t i) {
    if (i >= chars.size() || chars[i] != L'$') return std::nullopt;
    std::wstring tag = L"$";
    size_t j = i + 1;
    while (j < chars.size() && isWordChar(chars[j])) {
        tag.push_back(chars[j]);
        j += 1;
    }
    if (j >= chars.size() || chars[j] != L'$') return std::nullopt;
    tag.push_back(L'$');
    return tag;
}

// `sql` upper-cased with comments and string/dollar-quoted literal *contents*
// replaced by spaces, so keyword scanning never trips over a value or a
// commented-out keyword.
std::wstring strippedUppercasedCode(const std::wstring& sql) {
    std::wstring out;
    bool inSingle = false, inLine = false, inBlock = false, inDollar = false;
    std::wstring dollarTag;
    const size_t count = sql.size();
    size_t i = 0;

    while (i < count) {
        const wchar_t c = sql[i];
        const bool hasNext = (i + 1 < count);
        const wchar_t next = hasNext ? sql[i + 1] : 0;

        if (inLine) {
            if (c == L'\n') {
                inLine = false;
                out.push_back(L' ');
            }
            i += 1;
            continue;
        }
        if (inBlock) {
            if (c == L'*' && hasNext && next == L'/') {
                inBlock = false;
                i += 2;
                out.push_back(L' ');
                continue;
            }
            i += 1;
            continue;
        }
        if (inDollar) {
            if (c == L'$') {
                auto tag = dollarTagAt(sql, i);
                if (tag && *tag == dollarTag) {
                    inDollar = false;
                    dollarTag.clear();
                    i += tag->size();
                    out.push_back(L' ');
                    continue;
                }
            }
            i += 1;
            continue;
        }
        if (inSingle) {
            if (c == L'\'') {
                if (hasNext && next == L'\'') {
                    i += 2;
                    continue;  // escaped quote
                }
                inSingle = false;
                out.push_back(L' ');
            }
            i += 1;
            continue;
        }

        // Not currently inside a literal/comment.
        if (c == L'-' && hasNext && next == L'-') {
            inLine = true;
            i += 1;
            continue;
        }
        if (c == L'/' && hasNext && next == L'*') {
            inBlock = true;
            i += 1;
            continue;
        }
        if (c == L'\'') {
            inSingle = true;
            i += 1;
            continue;
        }
        if (c == L'$') {
            auto tag = dollarTagAt(sql, i);
            if (tag) {
                inDollar = true;
                dollarTag = *tag;
                i += tag->size();
                continue;
            }
        }

        out.push_back(asciiUpper(c));
        i += 1;
    }
    return out;
}

// Split on any run of non-word characters, dropping empty tokens.
std::vector<std::wstring> splitWords(const std::wstring& s) {
    std::vector<std::wstring> out;
    std::wstring cur;
    for (const wchar_t c : s) {
        if (isWordChar(c)) {
            cur.push_back(c);
        } else if (!cur.empty()) {
            out.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

bool contains(const std::vector<std::wstring>& words, const std::wstring& w) {
    for (const auto& x : words)
        if (x == w) return true;
    return false;
}

bool anyDataModifying(const std::vector<std::wstring>& words) {
    const auto& set = dataModifyingKeywords();
    for (const auto& x : words)
        if (set.count(x)) return true;
    return false;
}

}  // namespace

SqlStatementInfo SqlStatementClassifier::classify(const std::wstring& statement) {
    const std::wstring code = strippedUppercasedCode(statement);
    const std::vector<std::wstring> words = splitWords(code);
    if (words.empty()) {
        return SqlStatementInfo{L"", SqlStatementKind::Neutral, false};
    }

    const std::wstring& first = words.front();

    SqlStatementKind kind;
    if (writeKeywords().count(first)) {
        kind = SqlStatementKind::Write;
    } else if (readKeywords().count(first)) {
        kind = SqlStatementKind::Read;
    } else if (neutralKeywords().count(first)) {
        kind = SqlStatementKind::Neutral;
    } else if (first == L"WITH") {
        // A CTE is a write iff it ultimately runs a data-modifying statement.
        kind = anyDataModifying(words) ? SqlStatementKind::Write : SqlStatementKind::Read;
    } else if (first == L"EXPLAIN") {
        // EXPLAIN ANALYZE actually executes the plan (writes included).
        kind = contains(words, L"ANALYZE") ? SqlStatementKind::Write : SqlStatementKind::Read;
    } else {
        kind = SqlStatementKind::Neutral;
    }

    bool isDestructive = (first == L"DROP" || first == L"TRUNCATE");
    if (first == L"DELETE" || first == L"UPDATE") {
        isDestructive = !contains(words, L"WHERE");
    }

    return SqlStatementInfo{first, kind, isDestructive};
}

std::vector<SqlStatementInfo> SqlStatementClassifier::classifyAll(const std::wstring& sql) {
    std::vector<SqlStatementInfo> out;
    for (const auto& stmt : SqlStatementSplitter::split(sql)) {
        out.push_back(classify(stmt));
    }
    return out;
}

}  // namespace sqlcore
