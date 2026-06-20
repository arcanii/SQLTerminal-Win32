// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for the pure terminal-controller logic: dot-commands, smart-quote
// normalization, the read-only/destructive guard, transaction tracking,
// command-history navigation, and schema SQL.
#include <optional>
#include <string>
#include <vector>

#include "app/DotCommandHandler.h"
#include "app/TerminalLogic.h"
#include "test_harness.h"

using namespace sqlterm;
using std::optional;
using std::vector;
using std::wstring;

static std::string dbg(const wstring& w) { return "\"" + th::narrow(w) + "\""; }
static std::string dbg(const optional<wstring>& o) {
    return o ? ("Some(" + dbg(*o) + ")") : std::string("None");
}
static bool has(const wstring& s, const wstring& sub) { return s.find(sub) != wstring::npos; }

// ===== DotCommandHandler =====================================================

TEST(dot_not_a_command) {
    CHECK(!handleDotCommand(L"SELECT 1", DatabaseEngine::Sqlite).has_value());
}
TEST(dot_tables_per_engine) {
    auto s = handleDotCommand(L".tables", DatabaseEngine::Sqlite);
    CHECK(s && s->kind == DotKind::Sql);
    CHECK_EQ(s->statements.front(),
             wstring(L"SELECT name FROM sqlite_master WHERE type='table' ORDER BY name;"));
    auto p = handleDotCommand(L".tables", DatabaseEngine::Postgres);
    CHECK(p && p->kind == DotKind::Sql);
    CHECK(has(p->statements.front(), L"pg_tables"));
}
TEST(dot_count_quotes_table) {
    auto r = handleDotCommand(L".count users", DatabaseEngine::Sqlite);
    CHECK(r && r->kind == DotKind::Sql);
    CHECK_EQ(r->statements.front(), wstring(L"SELECT COUNT(*) AS row_count FROM \"users\";"));
}
TEST(dot_command_lowercased_arg_case_kept) {
    auto r = handleDotCommand(L".COLUMNS Users", DatabaseEngine::Sqlite);
    CHECK(r && r->kind == DotKind::Sql);
    CHECK_EQ(r->statements.front(), wstring(L"PRAGMA table_info('Users');"));
}
TEST(dot_usage_messages) {
    auto r = handleDotCommand(L".columns", DatabaseEngine::Sqlite);
    CHECK(r && r->kind == DotKind::Message);
    CHECK_EQ(r->text, wstring(L"Usage: .columns <table_name>"));
}
TEST(dot_clear_and_reconnect) {
    CHECK(handleDotCommand(L".clear", DatabaseEngine::Sqlite)->kind == DotKind::Clear);
    auto r = handleDotCommand(L".connect mydb", DatabaseEngine::Postgres);
    CHECK(r && r->kind == DotKind::Reconnect);
    CHECK_EQ(r->text, wstring(L"mydb"));
    CHECK(handleDotCommand(L".connect db", DatabaseEngine::Sqlite)->kind == DotKind::Message);
}
TEST(dot_dbinfo_multi_and_help_and_unknown) {
    CHECK(handleDotCommand(L".dbinfo", DatabaseEngine::Sqlite)->statements.size() == 4);
    CHECK(has(handleDotCommand(L".help", DatabaseEngine::Sqlite)->text,
              L"Available dot-commands (SQLite)"));
    CHECK(has(handleDotCommand(L".bogus", DatabaseEngine::Sqlite)->text, L"Unknown command: .bogus"));
}

// ===== smart characters ======================================================

TEST(smart_characters_normalized) {
    const wstring in = L"  \x201C" L"hi" L"\x201D \x2018x\x2019 \x2013 \x2014  ";
    CHECK_EQ(normalizeSmartCharacters(in), wstring(L"\"hi\" 'x' - -"));
}

// ===== command history =======================================================

TEST(command_history_navigation) {
    CommandHistory h;
    CHECK(!h.up(L"cur").has_value());  // empty
    h.add(L"a");
    h.add(L"b");
    CHECK_EQ(h.up(L"typed"), optional<wstring>(L"b"));
    CHECK_EQ(h.up(L""), optional<wstring>(L"a"));
    CHECK_EQ(h.up(L""), optional<wstring>(L"a"));      // stays at oldest
    CHECK_EQ(h.down(), optional<wstring>(L"b"));
    CHECK_EQ(h.down(), optional<wstring>(L"typed"));   // restores saved input
    CHECK(!h.down().has_value());
}

// ===== guards ================================================================

TEST(guard_run_block_confirm) {
    CHECK(evaluateGuard({L"SELECT 1"}, false).action == GuardAction::Run);

    auto blocked = evaluateGuard({L"INSERT INTO t VALUES(1)"}, true);
    CHECK(blocked.action == GuardAction::Block);
    CHECK(has(blocked.message, L"Read-only mode is on — INSERT"));

    auto drop = evaluateGuard({L"DROP TABLE t"}, false);
    CHECK(drop.action == GuardAction::Confirm);
    CHECK(has(drop.message, L"DROP"));
    CHECK(has(drop.message, L"can't be undone"));

    auto del = evaluateGuard({L"DELETE FROM t"}, false);
    CHECK(del.action == GuardAction::Confirm);
    CHECK(has(del.message, L"DELETE without WHERE"));

    CHECK(evaluateGuard({L"DELETE FROM t WHERE id=1"}, false).action == GuardAction::Run);
    // Read-only takes precedence over destructive-confirm.
    CHECK(evaluateGuard({L"DROP TABLE t"}, true).action == GuardAction::Block);
}

// ===== transactions ==========================================================

TEST(transaction_tracking) {
    CHECK(updateInTransaction(false, {L"BEGIN"}) == true);
    CHECK(updateInTransaction(true, {L"COMMIT"}) == false);
    CHECK(updateInTransaction(false, {L"BEGIN; SELECT 1; COMMIT;"}) == false);  // last wins
    CHECK(updateInTransaction(true, {L"SELECT 1"}) == true);                    // unchanged
}

// ===== schema SQL ============================================================

TEST(schema_sql_and_identifiers) {
    CHECK_EQ(quotedIdentifier(L"my\"tbl"), wstring(L"\"my\"\"tbl\""));
    CHECK_EQ(selectStatementFor(L"users"), wstring(L"SELECT * FROM \"users\" LIMIT 100;"));
    CHECK(has(tableNamesSql(DatabaseEngine::Sqlite), L"NOT LIKE 'sqlite_%'"));
    CHECK_EQ(columnsSql(DatabaseEngine::Sqlite, L"t"), wstring(L"PRAGMA table_info('t');"));
    CHECK(has(columnsSql(DatabaseEngine::Postgres, L"o'brien"), L"'o''brien'"));
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
