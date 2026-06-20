// SPDX-License-Identifier: GPL-3.0-or-later
// SqlStatementSplitter — splits SQL into statements at top-level semicolons,
// ignoring semicolons inside single-quoted strings (with '' escape), line/block
// comments, and $tag$...$tag$ dollar-quoted blocks (PL/pgSQL bodies).
//
// Byte-for-byte port of SQLTerminal/Core/SQLStatementSplitter.swift. This is the
// single source of truth for statement boundaries: the Postgres provider uses it
// to run multi-statement input, and the editor uses it for "run the statement
// under the cursor" and to classify statements for the safety guards.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace sqlcore {

// A statement and the half-open offset range [start, end) it occupies in the
// original input (offsets index UTF-16 code units of the source).
struct Segment {
    std::wstring text;  // trimmed; may be empty (whitespace/comment only)
    size_t start;
    size_t end;
};

class SqlStatementSplitter {
public:
    // The trimmed, non-empty statements, in order (each keeps its trailing ';').
    static std::vector<std::wstring> split(const std::wstring& sql);

    // The trimmed statement whose range contains `cursorOffset`, or nullopt if
    // that position falls only in whitespace/comments between statements.
    static std::optional<std::wstring> statementAtOffset(long long cursorOffset,
                                                         const std::wstring& sql);

    // Walks the input once, emitting a Segment per top-level ';'-delimited run
    // (plus a trailing run).
    static std::vector<Segment> segments(const std::wstring& sql);
};

}  // namespace sqlcore
