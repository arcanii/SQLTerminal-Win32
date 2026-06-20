// SPDX-License-Identifier: GPL-3.0-or-later
#include "db/PostgresProvider.h"

#include <libpq-fe.h>

#include <chrono>
#include <cstdlib>
#include <vector>

#include "core/PostgresHba.h"
#include "core/SqlStatementSplitter.h"
#include "core/SqlText.h"
#include "db/PostgresConnInfo.h"
#include "platform/Encoding.h"

namespace sqlterm {

namespace {

double secondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

// True if every line of `stmt` is blank or a `--` line comment (nothing to run).
bool isCommentOnly(const std::wstring& stmt) {
    size_t pos = 0;
    const size_t n = stmt.size();
    while (pos < n) {
        size_t end = stmt.find(L'\n', pos);
        if (end == std::wstring::npos) end = n;
        const std::wstring line = sqlcore::text::trimWhitespace(stmt.substr(pos, end - pos));
        if (!line.empty() && line.rfind(L"--", 0) != 0) return false;
        pos = end + 1;
    }
    return true;
}

std::wstring resultError(PGresult* res, PGconn* conn) {
    auto field = [&](int code) -> std::wstring {
        const char* f = PQresultErrorField(res, code);
        return f ? wideFromUtf8(f) : std::wstring();
    };
    const std::wstring severity = field(PG_DIAG_SEVERITY);
    std::wstring primary = field(PG_DIAG_MESSAGE_PRIMARY);
    if (primary.empty()) primary = wideFromUtf8(PQerrorMessage(conn));
    const std::wstring detail = field(PG_DIAG_MESSAGE_DETAIL);
    const std::wstring hint = field(PG_DIAG_MESSAGE_HINT);
    const std::wstring sqlstate = field(PG_DIAG_SQLSTATE);

    std::wstring out = severity.empty() ? primary : (severity + L": " + primary);
    if (!detail.empty()) out += L"\nDetail: " + detail;
    if (!hint.empty()) out += L"\nHint: " + hint;
    if (auto hba = sqlcore::PostgresHba::suggestedLine(primary)) {
        out += L"\n\nThe server has no pg_hba.conf rule that permits this login. Add a "
               L"line like this and reload (SELECT pg_reload_conf();):\n\n    " + *hba;
    }
    if (!sqlstate.empty()) out += L"\n(SQLSTATE " + sqlstate + L")";
    return out;
}

}  // namespace

PostgresProvider::~PostgresProvider() { disconnect(); }

bool PostgresProvider::connect(const DatabaseConnection& config, std::wstring& error) {
    disconnect();

    const std::string conninfo = buildConnInfo(config, config.sslMode);
    PGconn* c = PQconnectdb(conninfo.c_str());
    if (!c) {
        error = L"PostgreSQL connection failed: out of memory.";
        return false;
    }
    if (PQstatus(c) != CONNECTION_OK) {
        std::wstring msg = wideFromUtf8(PQerrorMessage(c));
        PQfinish(c);
        std::wstring full = L"PostgreSQL connection failed: " + msg;
        if (auto hba = sqlcore::PostgresHba::suggestedLine(msg)) {
            full += L"\nThe server has no pg_hba.conf rule that permits this login. Add a "
                    L"line like this and reload (SELECT pg_reload_conf();):\n\n    " + *hba;
        }
        error = full;
        return false;
    }

    conn_ = c;
    cancel_ = PQgetCancel(conn_);  // snapshot for thread-safe interruption
    isSSLActive_ = (PQsslInUse(conn_) != 0);
    isConnected_ = true;
    status_ = L"Connected to " + config.username + L"@" + config.host + L":" + config.port +
              L"/" + config.databaseName + (isSSLActive_ ? L" (SSL)" : L"");
    return true;
}

void PostgresProvider::disconnect() {
    if (cancel_) {
        PQfreeCancel(cancel_);
        cancel_ = nullptr;
    }
    if (conn_) {
        PQfinish(conn_);
        conn_ = nullptr;
    }
    isConnected_ = false;
    isSSLActive_ = false;
    status_ = L"Disconnected";
}

void PostgresProvider::cancel() {
    if (cancel_) {
        char errbuf[256];
        PQcancel(cancel_, errbuf, static_cast<int>(sizeof errbuf));
    }
}

QueryResult PostgresProvider::execute(const std::wstring& sql) {
    if (!conn_) return QueryResult::failure(L"No database connection.");
    const std::wstring trimmed = sqlcore::text::trimWhitespace(sql);
    if (trimmed.empty()) return QueryResult::failure(L"Empty query.");

    const auto start = std::chrono::steady_clock::now();
    std::vector<std::wstring> lastColumns;
    std::vector<std::vector<std::wstring>> lastRows;
    long long totalRowsAffected = 0;
    bool lastWasQuery = false;

    auto errorResult = [&](const std::wstring& msg) {
        QueryResult r;
        r.columns = lastColumns;
        r.rows = lastRows;
        r.rowsAffected = totalRowsAffected;
        r.executionTimeSec = secondsSince(start);
        r.error = msg;
        r.statementType = StatementType::Error;
        return r;
    };

    for (const auto& raw : sqlcore::SqlStatementSplitter::split(trimmed)) {
        const std::wstring stmt = sqlcore::text::trimWhitespace(raw);
        if (stmt.empty()) continue;
        if (stmt[0] == L'\\') continue;       // skip psql meta-commands
        if (isCommentOnly(stmt)) continue;    // skip comment-only statements

        const std::string utf8 = utf8FromWide(stmt);
        PGresult* res = PQexec(conn_, utf8.c_str());
        const ExecStatusType status = PQresultStatus(res);

        if (status == PGRES_TUPLES_OK) {
            const int nf = PQnfields(res);
            const int nt = PQntuples(res);
            lastColumns.clear();
            lastColumns.reserve(static_cast<size_t>(nf));
            for (int i = 0; i < nf; ++i) {
                const char* name = PQfname(res, i);
                lastColumns.push_back(name ? wideFromUtf8(name) : std::wstring());
            }
            lastRows.clear();
            lastRows.reserve(static_cast<size_t>(nt));
            for (int r = 0; r < nt; ++r) {
                std::vector<std::wstring> row;
                row.reserve(static_cast<size_t>(nf));
                for (int i = 0; i < nf; ++i) {
                    if (PQgetisnull(res, r, i))
                        row.push_back(L"NULL");
                    else
                        row.push_back(wideFromUtf8(PQgetvalue(res, r, i)));
                }
                lastRows.push_back(std::move(row));
            }
            totalRowsAffected += nt;
            lastWasQuery = true;
        } else if (status == PGRES_COMMAND_OK || status == PGRES_EMPTY_QUERY) {
            const char* tuples = PQcmdTuples(res);
            if (tuples && *tuples) totalRowsAffected += std::atoll(tuples);
            lastWasQuery = false;
        } else {
            const std::wstring msg = resultError(res, conn_);
            PQclear(res);
            return errorResult(msg);
        }
        PQclear(res);
    }

    QueryResult out;
    out.columns = lastColumns;
    out.rows = lastRows;
    out.rowsAffected = totalRowsAffected;
    out.executionTimeSec = secondsSince(start);
    out.error = std::nullopt;
    out.statementType = lastWasQuery ? StatementType::Query : StatementType::Mutation;
    return out;
}

}  // namespace sqlterm
