// SPDX-License-Identifier: GPL-3.0-or-later
// Pure tests for the libpq conninfo builder (no libpq / no server needed).
#include <string>

#include "db/PostgresConnInfo.h"
#include "models/DatabaseConnection.h"
#include "test_harness.h"

using namespace sqlterm;

static std::string dbg(const std::string& s) { return "\"" + s + "\""; }

TEST(conninfo_basic) {
    DatabaseConnection c;
    c.engine = DatabaseEngine::Postgres;
    c.host = L"localhost";
    c.port = L"5432";
    c.databaseName = L"demo";
    c.username = L"bob";
    c.password = L"secret";
    c.sslMode = SslMode::Prefer;
    CHECK_EQ(buildConnInfo(c, c.sslMode),
             std::string("host='localhost' port='5432' dbname='demo' user='bob' "
                         "password='secret' sslmode='prefer' connect_timeout='10'"));
}

TEST(conninfo_sslmodes) {
    DatabaseConnection c;
    c.host = L"h";
    c.databaseName = L"d";
    c.username = L"u";
    c.port.clear();
    c.password.clear();
    CHECK_EQ(buildConnInfo(c, SslMode::Off),
             std::string("host='h' dbname='d' user='u' sslmode='disable' connect_timeout='10'"));
    CHECK_EQ(buildConnInfo(c, SslMode::Require),
             std::string("host='h' dbname='d' user='u' sslmode='require' connect_timeout='10'"));
}

TEST(conninfo_escapes_quotes_and_backslashes) {
    DatabaseConnection c;
    c.host = L"h";
    c.port.clear();
    c.databaseName.clear();
    c.username.clear();
    c.password = L"a'b\\c";  // a'b\c
    CHECK_EQ(buildConnInfo(c, SslMode::Off),
             std::string("host='h' password='a\\'b\\\\c' sslmode='disable' connect_timeout='10'"));
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
