// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for the JSON persistence stores (redirected to a temp dir via
// SQLT_DATA_DIR) and the Windows Credential Manager round-trip.
#define _CRT_SECURE_NO_WARNINGS  // _wputenv_s
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "models/ConnectionProfile.h"
#include "persistence/Stores.h"
#include "security/CredentialStore.h"
#include "test_harness.h"

using namespace sqlterm;
using std::optional;
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
static std::string dbg(const optional<wstring>& o) {
    return o ? ("Some(" + dbg(*o) + ")") : std::string("None");
}

static ConnectionProfile pg(const wstring& user, optional<wstring> name = std::nullopt) {
    ConnectionProfile p;
    p.engine = DatabaseEngine::Postgres;
    p.host = L"h";
    p.port = L"5432";
    p.databaseName = L"d";
    p.username = user;
    p.name = std::move(name);
    p.sslMode = SslMode::Prefer;
    return p;
}
static ConnectionProfile named(const wstring& nm) { return pg(L"u", nm); }

static vector<wstring> usernames(const vector<ConnectionProfile>& v) {
    vector<wstring> out;
    for (const auto& p : v) out.push_back(p.username);
    return out;
}
static vector<wstring> displayNames(const vector<ConnectionProfile>& v) {
    vector<wstring> out;
    for (const auto& p : v) out.push_back(p.displayName());
    return out;
}

TEST(recents_mru_dedup_and_cap) {
    RecentConnectionsStore::clear();
    RecentConnectionsStore::add(pg(L"a"));
    RecentConnectionsStore::add(pg(L"b"));
    RecentConnectionsStore::add(pg(L"c"));
    CHECK_EQ(usernames(RecentConnectionsStore::load()), (vector<wstring>{L"c", L"b", L"a"}));

    RecentConnectionsStore::add(pg(L"a"));  // existing -> moves to front, no dup
    CHECK_EQ(usernames(RecentConnectionsStore::load()), (vector<wstring>{L"a", L"c", L"b"}));

    RecentConnectionsStore::clear();
    for (int i = 0; i < 12; ++i) RecentConnectionsStore::add(pg(L"u" + std::to_wstring(i)));
    const auto list = RecentConnectionsStore::load();
    CHECK(list.size() == 10);                 // capped
    CHECK_EQ(list.front().username, wstring(L"u11"));  // most-recent first

    RecentConnectionsStore::clear();
    CHECK(RecentConnectionsStore::load().empty());
}

TEST(saved_profiles_sorted_dedup_remove) {
    for (auto& p : SavedProfilesStore::load()) SavedProfilesStore::remove(p);  // start clean

    SavedProfilesStore::save(named(L"Beta"));
    SavedProfilesStore::save(named(L"alpha"));
    CHECK_EQ(displayNames(SavedProfilesStore::load()), (vector<wstring>{L"alpha", L"Beta"}));

    ConnectionProfile a2 = named(L"alpha");
    a2.host = L"otherhost";  // same id (named:alpha) -> replace, not add
    SavedProfilesStore::save(a2);
    CHECK(SavedProfilesStore::load().size() == 2);

    SavedProfilesStore::remove(named(L"Beta"));
    CHECK_EQ(displayNames(SavedProfilesStore::load()), (vector<wstring>{L"alpha"}));
}

TEST(history_dedup_bump_and_cap) {
    QueryHistoryStore::clear();
    QueryHistoryStore::record(L"SELECT 1", 1.0);
    QueryHistoryStore::record(L"SELECT 2", 2.0);
    QueryHistoryStore::record(L"SELECT 1", 3.0);  // dup -> bump to front, runCount 2
    auto list = QueryHistoryStore::load();
    CHECK(list.size() == 2);
    CHECK_EQ(list.front().sql, wstring(L"SELECT 1"));
    CHECK(list.front().runCount == 2);
    CHECK(list.front().lastRunEpoch == 3.0);

    QueryHistoryStore::record(L"   \n  ", 4.0);  // blank ignored
    CHECK(QueryHistoryStore::load().size() == 2);

    QueryHistoryStore::clear();
    for (int i = 0; i < 205; ++i) QueryHistoryStore::record(L"q" + std::to_wstring(i), i);
    CHECK(QueryHistoryStore::load().size() == 200);
}

TEST(snippets_upsert_ci_and_sorted) {
    for (auto& s : SnippetStore::load()) SnippetStore::remove(s);  // start clean

    SnippetStore::save(L"Foo", L"sqlA");
    SnippetStore::save(L"foo", L"sqlB");  // ci-equal -> replace
    auto list = SnippetStore::load();
    CHECK(list.size() == 1);
    CHECK_EQ(list.front().sql, wstring(L"sqlB"));

    SnippetStore::save(L"Bar", L"sqlC");
    list = SnippetStore::load();
    CHECK(list.size() == 2);
    CHECK_EQ(list.front().name, wstring(L"Bar"));  // sorted before "foo"
}

TEST(credential_manager_roundtrip) {
    const wstring acct = L"sqlt-selftest@localhost:5432/testdb";
    CredentialStore::deletePassword(acct);  // clean slate

    CHECK(CredentialStore::savePassword(acct, L"p@ss w0rd →"));  // includes non-ASCII
    CHECK_EQ(CredentialStore::password(acct), optional<wstring>(L"p@ss w0rd →"));

    CHECK(CredentialStore::deletePassword(acct));
    CHECK(!CredentialStore::password(acct).has_value());
    CHECK(CredentialStore::deletePassword(acct));  // idempotent
}

int main() {
    // Redirect persistence to a fresh temp dir so we never touch real %APPDATA%.
    const auto dir = std::filesystem::temp_directory_path() / L"sqlterminal_store_test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    _wputenv_s(L"SQLT_DATA_DIR", dir.wstring().c_str());

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
