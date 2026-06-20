// SPDX-License-Identifier: GPL-3.0-or-later
// QueryResult — the unified result of executing SQL. Plain data, safe to build
// on a worker thread and hand to the UI thread (P3). Ported from
// SQLTerminal/Models/QueryResult.swift. All cell values are pre-stringified.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace sqlterm {

enum class StatementType {
    Query,     // SELECT/PRAGMA — returns rows
    Mutation,  // INSERT/UPDATE/DELETE/CREATE/DROP — no rows
    Error,
};

struct QueryResult {
    std::vector<std::wstring> columns;
    std::vector<std::vector<std::wstring>> rows;
    long long rowsAffected = 0;
    double executionTimeSec = 0.0;
    std::optional<std::wstring> error;
    StatementType statementType = StatementType::Query;

    static QueryResult failure(const std::wstring& message) {
        QueryResult r;
        r.error = message;
        r.statementType = StatementType::Error;
        return r;
    }
};

}  // namespace sqlterm
