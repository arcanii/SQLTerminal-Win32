// SPDX-License-Identifier: GPL-3.0-or-later
#include "db/SqliteProvider.h"

#include <chrono>
#include <filesystem>
#include <string>

#include "platform/Encoding.h"
#include "sqlite3.h"

namespace sqlterm {

namespace {
double secondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}
bool isBlank(const std::wstring& s) {
    for (const wchar_t c : s) {
        if (c != L' ' && c != L'\n' && c != L'\r' && c != L'\t' && c != L'\v' && c != L'\f')
            return false;
    }
    return true;
}
}  // namespace

SqliteProvider::~SqliteProvider() { disconnect(); }

bool SqliteProvider::connect(const DatabaseConnection& config, std::wstring& error) {
    disconnect();

    const std::wstring path = config.filePath;
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path p(path);
    const fs::path dir = p.parent_path();
    if (!dir.empty() && !fs::exists(dir, ec)) {
        error = L"SQLite connection failed: Directory does not exist: " + dir.wstring();
        return false;
    }

    const std::string upath = utf8FromWide(path);
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    const int rc = sqlite3_open_v2(upath.c_str(), &db_, flags, nullptr);
    if (rc != SQLITE_OK || db_ == nullptr) {
        std::wstring msg = db_ ? wideFromUtf8(sqlite3_errmsg(db_))
                               : (L"Unknown error (code " + std::to_wstring(rc) + L")");
        disconnect();
        error = L"SQLite connection failed: " + msg;
        return false;
    }

    isConnected_ = true;
    status_ = L"Connected to " + p.filename().wstring();
    return true;
}

void SqliteProvider::disconnect() {
    if (db_) sqlite3_close_v2(db_);
    db_ = nullptr;
    isConnected_ = false;
    status_ = L"Disconnected";
}

void SqliteProvider::cancel() {
    if (db_) sqlite3_interrupt(db_);
}

QueryResult SqliteProvider::execute(const std::wstring& sql) {
    if (db_ == nullptr) return QueryResult::failure(L"No database connection.");
    if (isBlank(sql)) return QueryResult::failure(L"Empty query.");

    const auto start = std::chrono::steady_clock::now();

    std::vector<std::wstring> allColumns;
    std::vector<std::vector<std::wstring>> allRows;
    long long totalRowsAffected = 0;
    bool lastStatementWasQuery = false;

    // sqlite3_prepare_v2 wants a contiguous, null-terminated UTF-8 buffer; walk
    // it via the tail pointer to run each statement in turn.
    const std::string utf8 = utf8FromWide(sql);
    const char* current = utf8.c_str();
    const char* const endp = current + utf8.size();

    auto errorResult = [&](const std::wstring& msg) {
        QueryResult r;
        r.columns = allColumns;
        r.rows = allRows;
        r.rowsAffected = totalRowsAffected;
        r.executionTimeSec = secondsSince(start);
        r.error = msg;
        r.statementType = StatementType::Error;
        return r;
    };

    while (true) {
        // Skip whitespace and semicolons.
        while (current < endp && (*current == ' ' || *current == '\n' ||
                                  *current == '\r' || *current == '\t' || *current == ';')) {
            ++current;
        }
        if (current >= endp || *current == '\0') break;

        sqlite3_stmt* stmt = nullptr;
        const char* tail = nullptr;
        const int prepareRC = sqlite3_prepare_v2(db_, current, -1, &stmt, &tail);
        if (prepareRC != SQLITE_OK) {
            const std::wstring msg = wideFromUtf8(sqlite3_errmsg(db_));
            sqlite3_finalize(stmt);
            return errorResult(msg);
        }
        if (stmt == nullptr) break;  // nothing to prepare (trailing comment/ws)

        const int columnCount = sqlite3_column_count(stmt);
        if (columnCount > 0) {
            lastStatementWasQuery = true;
            allColumns.clear();
            allRows.clear();
            for (int i = 0; i < columnCount; ++i) {
                const char* name = sqlite3_column_name(stmt, i);
                allColumns.push_back(name ? wideFromUtf8(name)
                                          : (L"col" + std::to_wstring(i)));
            }
        } else {
            lastStatementWasQuery = false;
        }

        int stepRC = sqlite3_step(stmt);
        while (stepRC == SQLITE_ROW) {
            std::vector<std::wstring> row;
            row.reserve(static_cast<size_t>(columnCount));
            for (int i = 0; i < columnCount; ++i) {
                const unsigned char* text = sqlite3_column_text(stmt, i);
                row.push_back(text ? wideFromUtf8(reinterpret_cast<const char*>(text))
                                   : std::wstring(L"NULL"));
            }
            allRows.push_back(std::move(row));
            stepRC = sqlite3_step(stmt);
        }

        totalRowsAffected += sqlite3_changes(db_);
        sqlite3_finalize(stmt);

        if (stepRC != SQLITE_DONE) {
            return errorResult(wideFromUtf8(sqlite3_errmsg(db_)));
        }

        current = tail;
    }

    QueryResult r;
    r.columns = allColumns;
    r.rows = allRows;
    r.rowsAffected = totalRowsAffected;
    r.executionTimeSec = secondsSince(start);
    r.error = std::nullopt;
    r.statementType = lastStatementWasQuery ? StatementType::Query : StatementType::Mutation;
    return r;
}

}  // namespace sqlterm
