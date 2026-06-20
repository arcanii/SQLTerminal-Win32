// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for SqliteProvider against a real temp-file database. Verifies the
// connect/execute/disconnect behavior (multi-statement tail loop, NULL -> "NULL",
// last-result-set-wins, error reporting) without needing the GUI.
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "db/SqliteProvider.h"
#include "models/DatabaseConnection.h"
#include "models/QueryResult.h"
#include "test_harness.h"

using namespace sqlterm;
using std::vector;
using std::wstring;

static std::string dbg(const wstring& w) { return "\"" + th::narrow(w) + "\""; }
static std::string dbg(const vector<wstring>& v) {
    std::string s = "[";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += ", ";
        s += dbg(v[i]);
    }
    return s + "]";
}
static std::string dbg(const vector<vector<wstring>>& v) {
    std::string s = "[";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += ", ";
        s += dbg(v[i]);
    }
    return s + "]";
}

static wstring freshTempDb() {
    const auto path = std::filesystem::temp_directory_path() / L"sqlterminal_p1_test.db";
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return path.wstring();
}

static void connectFresh(SqliteProvider& db, wstring& pathOut) {
    pathOut = freshTempDb();
    DatabaseConnection cfg;
    cfg.engine = DatabaseEngine::Sqlite;
    cfg.filePath = pathOut;
    wstring err;
    CHECK(db.connect(cfg, err));
    CHECK(err.empty());
    CHECK(db.isConnected());
}

TEST(sqlite_create_insert_select) {
    wstring path;
    SqliteProvider db;
    connectFresh(db, path);

    const auto create = db.execute(L"CREATE TABLE t(id INTEGER, name TEXT);");
    CHECK(!create.error.has_value());
    CHECK(create.statementType == StatementType::Mutation);

    const auto insert = db.execute(L"INSERT INTO t(id,name) VALUES (1,'a'),(2,NULL);");
    CHECK(!insert.error.has_value());
    CHECK(insert.rowsAffected == 2);

    const auto select = db.execute(L"SELECT id, name FROM t ORDER BY id;");
    CHECK(!select.error.has_value());
    CHECK(select.statementType == StatementType::Query);
    CHECK_EQ(select.columns, (vector<wstring>{L"id", L"name"}));
    CHECK_EQ(select.rows, (vector<vector<wstring>>{{L"1", L"a"}, {L"2", L"NULL"}}));

    db.disconnect();
    CHECK(!db.isConnected());
}

TEST(sqlite_multi_statement_last_result_wins) {
    wstring path;
    SqliteProvider db;
    connectFresh(db, path);
    const auto r = db.execute(L"SELECT 1 AS a; SELECT 2 AS b;");
    CHECK(!r.error.has_value());
    CHECK_EQ(r.columns, (vector<wstring>{L"b"}));
    CHECK_EQ(r.rows, (vector<vector<wstring>>{{L"2"}}));
    db.disconnect();
}

TEST(sqlite_single_statement_without_semicolon) {
    wstring path;
    SqliteProvider db;
    connectFresh(db, path);
    const auto r = db.execute(L"SELECT 42 AS x");
    CHECK(!r.error.has_value());
    CHECK_EQ(r.columns, (vector<wstring>{L"x"}));
    CHECK_EQ(r.rows, (vector<vector<wstring>>{{L"42"}}));
    db.disconnect();
}

TEST(sqlite_error_is_reported) {
    wstring path;
    SqliteProvider db;
    connectFresh(db, path);
    const auto r = db.execute(L"SELECT * FROM does_not_exist;");
    CHECK(r.error.has_value());
    CHECK(r.statementType == StatementType::Error);
    db.disconnect();
}

TEST(sqlite_empty_query_is_error) {
    wstring path;
    SqliteProvider db;
    connectFresh(db, path);
    const auto r = db.execute(L"   \n  ");
    CHECK(r.error.has_value());
    db.disconnect();
}

TEST(sqlite_connect_missing_directory_fails) {
    SqliteProvider db;
    DatabaseConnection cfg;
    cfg.engine = DatabaseEngine::Sqlite;
    cfg.filePath = L"Z:\\no\\such\\dir\\nope.db";
    wstring err;
    CHECK(!db.connect(cfg, err));
    CHECK(!err.empty());
    CHECK(!db.isConnected());
}

int main() {
    for (const auto& tc : th::registry()) {
        th::g_currentTest = tc.name;
        tc.fn();
    }
    const int passed = th::g_checks - th::g_failures;
    std::cout << "\n"
              << (th::g_failures == 0 ? "PASSED" : "FAILED") << ": " << passed << "/"
              << th::g_checks << " checks across " << th::registry().size() << " tests";
    if (th::g_failures) std::cout << "  (" << th::g_failures << " failed)";
    std::cout << "\n";
    return th::g_failures == 0 ? 0 : 1;
}
