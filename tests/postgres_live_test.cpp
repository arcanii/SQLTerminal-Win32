// SPDX-License-Identifier: GPL-3.0-or-later
// Live PostgreSQL integration test, driven by env vars so it can run against a
// throwaway Docker Postgres. If SQLT_PG_HOST is unset it SKIPS (exit 0), so the
// normal test run doesn't require a server.
//
//   SQLT_PG_HOST=localhost SQLT_PG_PORT=5433 SQLT_PG_DB=demo
//   SQLT_PG_USER=postgres SQLT_PG_PASS=postgres [SQLT_PG_SSL=off] PostgresLiveTest
#define _CRT_SECURE_NO_WARNINGS  // _wgetenv
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "db/DatabaseSession.h"
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

static DatabaseConnection gCfg;
static bool gHave = false;

static wstring env(const wchar_t* name) {
    const wchar_t* v = _wgetenv(name);
    return v ? wstring(v) : wstring();
}

static bool connectSync(DatabaseSession& s, wstring& errOut) {
    std::mutex m;
    std::condition_variable cv;
    bool done = false, ok = false;
    wstring err;
    s.connectAsync(gCfg, [&](bool success, wstring error) {
        std::lock_guard<std::mutex> lk(m);
        ok = success;
        err = std::move(error);
        done = true;
        cv.notify_one();
    });
    std::unique_lock<std::mutex> lk(m);
    cv.wait(lk, [&] { return done; });
    errOut = err;
    return ok;
}

static QueryResult runSync(DatabaseSession& s, const wstring& sql) {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    QueryResult out;
    s.executeAsync(sql, [&](QueryResult r) {
        std::lock_guard<std::mutex> lk(m);
        out = std::move(r);
        done = true;
        cv.notify_one();
    });
    std::unique_lock<std::mutex> lk(m);
    cv.wait(lk, [&] { return done; });
    return out;
}

TEST(pg_connect_crud_and_select) {
    if (!gHave) return;
    DatabaseSession s;
    wstring err;
    CHECK(connectSync(s, err));
    if (!err.empty()) std::cout << "    connect error: " << th::narrow(err) << "\n";
    CHECK(s.isConnected());

    const auto select = runSync(
        s,
        L"DROP TABLE IF EXISTS sqlt_t;"
        L"CREATE TABLE sqlt_t(id int, name text);"
        L"INSERT INTO sqlt_t VALUES (1,'a'),(2,NULL);"
        L"SELECT id, name FROM sqlt_t ORDER BY id;");
    CHECK(!select.error.has_value());
    CHECK_EQ(select.columns, (vector<wstring>{L"id", L"name"}));
    CHECK_EQ(select.rows, (vector<vector<wstring>>{{L"1", L"a"}, {L"2", L"NULL"}}));
}

TEST(pg_multi_statement_last_result_wins) {
    if (!gHave) return;
    DatabaseSession s;
    wstring err;
    CHECK(connectSync(s, err));
    const auto r = runSync(s, L"SELECT 1 AS a; SELECT 2 AS b;");
    CHECK(!r.error.has_value());
    CHECK_EQ(r.columns, (vector<wstring>{L"b"}));
    CHECK_EQ(r.rows, (vector<vector<wstring>>{{L"2"}}));
}

TEST(pg_error_is_reported) {
    if (!gHave) return;
    DatabaseSession s;
    wstring err;
    CHECK(connectSync(s, err));
    const auto r = runSync(s, L"SELECT * FROM does_not_exist_xyz;");
    CHECK(r.error.has_value());
}

TEST(pg_cancel_interrupts_and_connection_survives) {
    if (!gHave) return;
    DatabaseSession s;
    wstring err;
    CHECK(connectSync(s, err));

    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    QueryResult out;
    s.executeAsync(L"SELECT pg_sleep(30);", [&](QueryResult r) {
        std::lock_guard<std::mutex> lk(m);
        out = std::move(r);
        done = true;
        cv.notify_one();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    s.cancel();
    {
        std::unique_lock<std::mutex> lk(m);
        const bool finished = cv.wait_for(lk, std::chrono::seconds(10), [&] { return done; });
        CHECK(finished);
        CHECK(out.error.has_value());  // PQcancel -> error
    }
    // PQcancel uses an out-of-band request; the connection stays alive.
    CHECK(s.isConnected());
    const auto r2 = runSync(s, L"SELECT 1 AS x;");
    CHECK(!r2.error.has_value());
    CHECK_EQ(r2.columns, (vector<wstring>{L"x"}));
}

int main() {
    gCfg.engine = DatabaseEngine::Postgres;
    gCfg.host = env(L"SQLT_PG_HOST");
    gCfg.port = env(L"SQLT_PG_PORT");
    gCfg.databaseName = env(L"SQLT_PG_DB");
    gCfg.username = env(L"SQLT_PG_USER");
    gCfg.password = env(L"SQLT_PG_PASS");
    const wstring ssl = env(L"SQLT_PG_SSL");
    gCfg.sslMode = (ssl == L"require") ? SslMode::Require
                   : (ssl == L"prefer") ? SslMode::Prefer
                                        : SslMode::Off;
    gHave = !gCfg.host.empty();

    if (!gHave) {
        std::cout << "SKIPPED: set SQLT_PG_HOST/PORT/DB/USER/PASS to run the live "
                     "PostgreSQL test.\n";
        return 0;
    }

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
