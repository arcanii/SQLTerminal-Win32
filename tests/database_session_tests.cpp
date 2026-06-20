// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for DatabaseSession: off-thread execute, result delivery, and — most
// importantly — interrupting a long-running query via cancel() with the
// connection surviving afterward (the P3 de-risking gate).
#include <chrono>
#include <condition_variable>
#include <filesystem>
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

static wstring freshTempDb() {
    const auto p = std::filesystem::temp_directory_path() / L"sqlterminal_p3_test.db";
    std::error_code ec;
    std::filesystem::remove(p, ec);
    return p.wstring();
}

static bool connectSync(DatabaseSession& s, const DatabaseConnection& cfg, wstring& errOut) {
    std::mutex m;
    std::condition_variable cv;
    bool done = false, ok = false;
    wstring err;
    s.connectAsync(cfg, [&](bool success, wstring error) {
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

TEST(session_connect_and_execute_off_thread) {
    DatabaseSession s;
    DatabaseConnection cfg;
    cfg.engine = DatabaseEngine::Sqlite;
    cfg.filePath = freshTempDb();
    wstring err;
    CHECK(connectSync(s, cfg, err));
    CHECK(err.empty());
    CHECK(s.isConnected());

    const auto r = runSync(s, L"SELECT 7 AS x;");
    CHECK(!r.error.has_value());
    CHECK_EQ(r.columns, (vector<wstring>{L"x"}));
    CHECK_EQ(r.rows, (vector<vector<wstring>>{{L"7"}}));
}

TEST(session_cancel_interrupts_long_query_and_connection_survives) {
    DatabaseSession s;
    DatabaseConnection cfg;
    cfg.engine = DatabaseEngine::Sqlite;
    cfg.filePath = freshTempDb();
    wstring err;
    CHECK(connectSync(s, cfg, err));

    // A query that runs effectively forever until interrupted.
    const wstring longQuery =
        L"WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x < 2000000000) "
        L"SELECT count(*) FROM c;";

    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    QueryResult out;
    s.executeAsync(longQuery, [&](QueryResult r) {
        std::lock_guard<std::mutex> lk(m);
        out = std::move(r);
        done = true;
        cv.notify_one();
    });

    // Let the worker get into sqlite3_step, then interrupt from this thread.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    s.cancel();

    {
        std::unique_lock<std::mutex> lk(m);
        // Must finish promptly after cancel (not run to completion).
        const bool finished =
            cv.wait_for(lk, std::chrono::seconds(10), [&] { return done; });
        CHECK(finished);
        CHECK(out.error.has_value());  // interrupted -> error result
    }

    // The connection must still be usable (sqlite3_interrupt leaves it open).
    CHECK(s.isConnected());
    const auto r2 = runSync(s, L"SELECT 1 AS x;");
    CHECK(!r2.error.has_value());
    CHECK_EQ(r2.columns, (vector<wstring>{L"x"}));
}

TEST(session_execute_without_connection_is_error) {
    DatabaseSession s;
    const auto r = runSync(s, L"SELECT 1;");
    CHECK(r.error.has_value());
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
