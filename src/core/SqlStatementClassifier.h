// SPDX-License-Identifier: GPL-3.0-or-later
// SqlStatementClassifier — heuristic, keyword-based classification used by the
// read-only guard and the destructive-statement confirmation. A safety net, not
// a security boundary: only recognised writes are flagged.
//
// Byte-for-byte port of SQLTerminal/Core/SQLStatementClassifier.swift.
#pragma once

#include <string>
#include <vector>

namespace sqlcore {

// Whether a statement reads, mutates, or just controls the session/transaction.
enum class SqlStatementKind {
    Read,     // SELECT, EXPLAIN, SHOW, ... — returns/inspects data
    Write,    // INSERT/UPDATE/DELETE/DDL/GRANT/... — mutates data or schema
    Neutral,  // BEGIN/COMMIT/SET/... or unrecognised — neither blocked nor confirmed
};

struct SqlStatementInfo {
    std::wstring leadingKeyword;  // upper-cased leading keyword, or L"" if empty
    SqlStatementKind kind;
    bool isDestructive;  // DROP, TRUNCATE, or DELETE/UPDATE with no WHERE
};

inline bool operator==(const SqlStatementInfo& a, const SqlStatementInfo& b) {
    return a.leadingKeyword == b.leadingKeyword && a.kind == b.kind &&
           a.isDestructive == b.isDestructive;
}
inline bool operator!=(const SqlStatementInfo& a, const SqlStatementInfo& b) {
    return !(a == b);
}

class SqlStatementClassifier {
public:
    static SqlStatementInfo classify(const std::wstring& statement);

    // Split `sql` into statements and classify each non-empty one.
    static std::vector<SqlStatementInfo> classifyAll(const std::wstring& sql);
};

}  // namespace sqlcore
