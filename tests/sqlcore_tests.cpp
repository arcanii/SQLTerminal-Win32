// SPDX-License-Identifier: GPL-3.0-or-later
// Golden tests for SqlCore — a direct transcription of the macOS SQLTerminal
// `Tests/SQLCoreTests/*.swift` XCTest assertion tables. These tables pin the
// observable behavior of the byte-for-byte port; keep them in lock-step with the
// Swift originals.
#include <optional>
#include <string>
#include <vector>

#include "core/PostgresHba.h"
#include "core/SqlLiteralScanner.h"
#include "core/SqlStatementClassifier.h"
#include "core/SqlStatementSplitter.h"
#include "core/SqlSyntaxHighlighter.h"
#include "test_harness.h"

using namespace sqlcore;
using std::optional;
using std::vector;
using std::wstring;

// ---- dbg() overloads (used by CHECK_EQ to print mismatches) -----------------

static std::string dbg(bool b) { return b ? "true" : "false"; }
static std::string dbg(const wstring& w) { return "\"" + th::narrow(w) + "\""; }

static std::string dbg(SqlStatementKind k) {
    switch (k) {
        case SqlStatementKind::Read: return "Read";
        case SqlStatementKind::Write: return "Write";
        case SqlStatementKind::Neutral: return "Neutral";
    }
    return "?";
}

static std::string dbg(const SqlLiteralRange& r) {
    return "(" + std::to_string(r.location) + ", " + std::to_string(r.length) +
           ", " + dbg(r.isComment) + ")";
}

[[maybe_unused]] static std::string dbg(const SqlStatementInfo& i) {
    return "{" + dbg(i.leadingKeyword) + ", " + dbg(i.kind) + ", " +
           dbg(i.isDestructive) + "}";
}

static std::string dbg(const optional<wstring>& o) {
    return o ? ("Some(" + dbg(*o) + ")") : std::string("None");
}

static std::string dbg(const vector<wstring>& v) {
    std::string s = "[";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += ", ";
        s += dbg(v[i]);
    }
    return s + "]";
}
static std::string dbg(const vector<SqlLiteralRange>& v) {
    std::string s = "[";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += ", ";
        s += dbg(v[i]);
    }
    return s + "]";
}
static std::string dbg(const vector<SqlStatementKind>& v) {
    std::string s = "[";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += ", ";
        s += dbg(v[i]);
    }
    return s + "]";
}

static std::string dbg(SyntaxToken t) {
    switch (t) {
        case SyntaxToken::Keyword: return "Keyword";
        case SyntaxToken::Number: return "Number";
        case SyntaxToken::StringLiteral: return "String";
        case SyntaxToken::Comment: return "Comment";
    }
    return "?";
}
static std::string dbg(const HighlightSpan& s) {
    return "(" + std::to_string(s.location) + ", " + std::to_string(s.length) + ", " +
           dbg(s.type) + ")";
}
static std::string dbg(const vector<HighlightSpan>& v) {
    std::string s = "[";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += ", ";
        s += dbg(v[i]);
    }
    return s + "]";
}

// ---- helpers ----------------------------------------------------------------

static SqlStatementInfo classify(const wstring& s) {
    return SqlStatementClassifier::classify(s);
}
static vector<SqlStatementKind> kinds(const vector<SqlStatementInfo>& infos) {
    vector<SqlStatementKind> out;
    for (const auto& i : infos) out.push_back(i.kind);
    return out;
}
static vector<SqlLiteralRange> scan(const wstring& s) {
    return SqlLiteralScanner::literalAndCommentRanges(s);
}

// ===== SQLStatementSplitterTests =============================================

TEST(splitter_SimpleMultiStatement) {
    CHECK_EQ(SqlStatementSplitter::split(L"SELECT 1; SELECT 2;"),
             (vector<wstring>{L"SELECT 1;", L"SELECT 2;"}));
}

TEST(splitter_TrailingStatementWithoutSemicolon) {
    CHECK_EQ(SqlStatementSplitter::split(L"SELECT 1; SELECT 2"),
             (vector<wstring>{L"SELECT 1;", L"SELECT 2"}));
}

TEST(splitter_EmptyAndWhitespaceOnly) {
    CHECK_EQ(SqlStatementSplitter::split(L"   \n  "), (vector<wstring>{}));
    CHECK_EQ(SqlStatementSplitter::split(L";;;"),
             (vector<wstring>{L";", L";", L";"}));
}

TEST(splitter_SemicolonInsideStringLiteralIsNotASplit) {
    CHECK_EQ(SqlStatementSplitter::split(L"SELECT ';' AS x;"),
             (vector<wstring>{L"SELECT ';' AS x;"}));
}

TEST(splitter_EscapedQuoteInsideString) {
    CHECK_EQ(SqlStatementSplitter::split(L"SELECT 'it''s; fine' AS x; SELECT 2;"),
             (vector<wstring>{L"SELECT 'it''s; fine' AS x;", L"SELECT 2;"}));
}

TEST(splitter_SemicolonInLineCommentIsNotASplit) {
    CHECK_EQ(SqlStatementSplitter::split(L"SELECT 1 -- one; two\n; SELECT 2;"),
             (vector<wstring>{L"SELECT 1 -- one; two\n;", L"SELECT 2;"}));
}

TEST(splitter_SemicolonInBlockCommentIsNotASplit) {
    CHECK_EQ(SqlStatementSplitter::split(L"SELECT 1 /* a; b */; SELECT 2;"),
             (vector<wstring>{L"SELECT 1 /* a; b */;", L"SELECT 2;"}));
}

TEST(splitter_DollarQuotedBodyWithSemicolonsIsOneStatement) {
    const wstring fn =
        L"CREATE FUNCTION f() RETURNS int AS $$\n"
        L"BEGIN\n"
        L"  PERFORM 1; PERFORM 2;\n"
        L"  RETURN 3;\n"
        L"END;\n"
        L"$$ LANGUAGE plpgsql;";
    const auto parts = SqlStatementSplitter::split(fn);
    CHECK_EQ(parts.size(), static_cast<size_t>(1));
    CHECK(!parts.empty() && parts[0].find(L"RETURN 3;") != wstring::npos);
}

TEST(splitter_TaggedDollarQuote) {
    CHECK_EQ(SqlStatementSplitter::split(L"SELECT $tag$ a; b; $tag$; SELECT 2;"),
             (vector<wstring>{L"SELECT $tag$ a; b; $tag$;", L"SELECT 2;"}));
}

TEST(splitter_StatementAtCursor) {
    const wstring sql = L"SELECT 1; SELECT 2; SELECT 3;";
    CHECK_EQ(SqlStatementSplitter::statementAtOffset(3, sql),
             optional<wstring>(L"SELECT 1;"));
    CHECK_EQ(SqlStatementSplitter::statementAtOffset(14, sql),
             optional<wstring>(L"SELECT 2;"));
    CHECK_EQ(SqlStatementSplitter::statementAtOffset(25, sql),
             optional<wstring>(L"SELECT 3;"));
}

TEST(splitter_StatementAtCursorAtEnd) {
    const wstring sql = L"SELECT 1; SELECT 2";
    CHECK_EQ(SqlStatementSplitter::statementAtOffset(
                 static_cast<long long>(sql.size()), sql),
             optional<wstring>(L"SELECT 2"));
}

// ===== SQLStatementClassifierTests ===========================================

TEST(classifier_Reads) {
    CHECK_EQ(classify(L"SELECT * FROM t").kind, SqlStatementKind::Read);
    CHECK_EQ(classify(L"  select 1").kind, SqlStatementKind::Read);
    CHECK_EQ(classify(L"SHOW server_encoding").kind, SqlStatementKind::Read);
    CHECK_EQ(classify(L"VALUES (1),(2)").kind, SqlStatementKind::Read);
    CHECK_EQ(classify(L"PRAGMA table_info('t')").kind, SqlStatementKind::Read);
}

TEST(classifier_Writes) {
    CHECK_EQ(classify(L"INSERT INTO t VALUES (1)").kind, SqlStatementKind::Write);
    CHECK_EQ(classify(L"update t set x = 1 where id = 2").kind, SqlStatementKind::Write);
    CHECK_EQ(classify(L"CREATE TABLE t (id int)").kind, SqlStatementKind::Write);
    CHECK_EQ(classify(L"ALTER TABLE t ADD COLUMN c int").kind, SqlStatementKind::Write);
    CHECK_EQ(classify(L"GRANT SELECT ON t TO bob").kind, SqlStatementKind::Write);
}

TEST(classifier_Neutral) {
    CHECK_EQ(classify(L"BEGIN").kind, SqlStatementKind::Neutral);
    CHECK_EQ(classify(L"COMMIT").kind, SqlStatementKind::Neutral);
    CHECK_EQ(classify(L"ROLLBACK").kind, SqlStatementKind::Neutral);
    CHECK_EQ(classify(L"SET search_path TO public").kind, SqlStatementKind::Neutral);
    CHECK_EQ(classify(L"FROBNICATE foo").kind, SqlStatementKind::Neutral);
}

TEST(classifier_LeadingKeywordUppercased) {
    CHECK_EQ(classify(L"  -- c\n select 1").leadingKeyword, wstring(L"SELECT"));
    CHECK_EQ(classify(L"/* x */ DrOp TABLE t").leadingKeyword, wstring(L"DROP"));
}

TEST(classifier_DestructiveDropAndTruncate) {
    CHECK(classify(L"DROP TABLE t").isDestructive);
    CHECK(classify(L"truncate t").isDestructive);
    CHECK(classify(L"DROP TABLE t").kind == SqlStatementKind::Write);
}

TEST(classifier_DeleteUpdateWithoutWhereIsDestructive) {
    CHECK(classify(L"DELETE FROM t").isDestructive);
    CHECK(classify(L"UPDATE t SET x = 1").isDestructive);
}

TEST(classifier_DeleteUpdateWithWhereIsNotDestructive) {
    CHECK(!classify(L"DELETE FROM t WHERE id = 1").isDestructive);
    CHECK(!classify(L"UPDATE t SET x = 1 WHERE id = 2").isDestructive);
}

TEST(classifier_WhereInsideStringDoesNotCount) {
    CHECK(classify(L"UPDATE t SET note = 'no where here'").isDestructive);
}

TEST(classifier_CommentedDropIsNotClassifiedAsDrop) {
    const auto info = classify(L"-- DROP TABLE t\nSELECT 1");
    CHECK_EQ(info.leadingKeyword, wstring(L"SELECT"));
    CHECK(!info.isDestructive);
}

TEST(classifier_WithCTE) {
    CHECK_EQ(classify(L"WITH x AS (SELECT 1) SELECT * FROM x").kind,
             SqlStatementKind::Read);
    CHECK_EQ(classify(L"WITH x AS (SELECT 1) DELETE FROM t WHERE id IN (SELECT 1)").kind,
             SqlStatementKind::Write);
}

TEST(classifier_Explain) {
    CHECK_EQ(classify(L"EXPLAIN SELECT 1").kind, SqlStatementKind::Read);
    CHECK_EQ(classify(L"EXPLAIN ANALYZE UPDATE t SET x = 1").kind,
             SqlStatementKind::Write);
}

TEST(classifier_ClassifyAllDropsWhitespaceOnlySegments) {
    CHECK_EQ(kinds(SqlStatementClassifier::classifyAll(L"SELECT 1;   \n  ")),
             (vector<SqlStatementKind>{SqlStatementKind::Read}));
    CHECK_EQ(kinds(SqlStatementClassifier::classifyAll(L"SELECT 1; DELETE FROM t;")),
             (vector<SqlStatementKind>{SqlStatementKind::Read, SqlStatementKind::Write}));
}

TEST(classifier_BareSemicolonIsNeutral) {
    CHECK_EQ(kinds(SqlStatementClassifier::classifyAll(
                 L"SELECT 1; ; DELETE FROM t WHERE id=1;")),
             (vector<SqlStatementKind>{SqlStatementKind::Read,
                                       SqlStatementKind::Neutral,
                                       SqlStatementKind::Write}));
}

// ===== SQLLiteralScannerTests ================================================

TEST(scanner_SingleQuotedString) {
    CHECK_EQ(scan(L"SELECT 'a;b'"),
             (vector<SqlLiteralRange>{{7, 5, false}}));
}
TEST(scanner_EscapedQuote) {
    CHECK_EQ(scan(L"'it''s'"), (vector<SqlLiteralRange>{{0, 7, false}}));
}
TEST(scanner_TwoStrings) {
    CHECK_EQ(scan(L"'a' 'b'"),
             (vector<SqlLiteralRange>{{0, 3, false}, {4, 3, false}}));
}
TEST(scanner_LineComment) {
    CHECK_EQ(scan(L"x -- c\ny"), (vector<SqlLiteralRange>{{2, 4, true}}));
}
TEST(scanner_BlockComment) {
    CHECK_EQ(scan(L"a /* b */ c"), (vector<SqlLiteralRange>{{2, 7, true}}));
}
TEST(scanner_DollarQuote) {
    CHECK_EQ(scan(L"$$ a; b $$"), (vector<SqlLiteralRange>{{0, 10, false}}));
}
TEST(scanner_TaggedDollarQuote) {
    CHECK_EQ(scan(L"$x$ a $x$"), (vector<SqlLiteralRange>{{0, 9, false}}));
}
TEST(scanner_DashDashInsideStringIsNotAComment) {
    CHECK_EQ(scan(L"'-- not'"), (vector<SqlLiteralRange>{{0, 8, false}}));
}
TEST(scanner_QuoteInsideCommentIsNotAString) {
    CHECK_EQ(scan(L"-- it's"), (vector<SqlLiteralRange>{{0, 7, true}}));
}
TEST(scanner_Mixed) {
    CHECK_EQ(scan(L"WHERE x = 'a' -- note"),
             (vector<SqlLiteralRange>{{10, 3, false}, {14, 7, true}}));
}
TEST(scanner_NoLiterals) {
    CHECK_EQ(scan(L"SELECT * FROM t WHERE id = 5"),
             (vector<SqlLiteralRange>{}));
}
TEST(scanner_UnterminatedStringRunsToEnd) {
    CHECK_EQ(scan(L"SELECT 'abc"), (vector<SqlLiteralRange>{{7, 4, false}}));
}

// ===== PostgresHBATests ======================================================

TEST(hba_SuggestsHostLineFromRejection) {
    const wstring msg =
        LR"(no pg_hba.conf entry for host "192.168.0.50", user "alice", database "app", no encryption)";
    CHECK_EQ(PostgresHba::suggestedLine(msg),
             optional<wstring>(L"host    app    alice    192.168.0.50/32    scram-sha-256"));
}

TEST(hba_IPv6UsesSlash128AndStripsZoneIndex) {
    const wstring msg =
        LR"(no pg_hba.conf entry for host "fe80::1%en0", user "bob", database "db", no encryption)";
    CHECK_EQ(PostgresHba::suggestedLine(msg),
             optional<wstring>(L"host    db    bob    fe80::1/128    scram-sha-256"));
}

TEST(hba_NonRejectionMessageReturnsNil) {
    CHECK(!PostgresHba::suggestedLine(
              LR"(password authentication failed for user "bob")")
               .has_value());
}

TEST(hba_NonIPAddressReturnsNil) {
    const wstring msg =
        LR"(no pg_hba.conf entry for host "localhost", user "bob", database "db", no encryption)";
    CHECK(!PostgresHba::suggestedLine(msg).has_value());
}

TEST(hba_QuotedValues) {
    CHECK_EQ(PostgresHba::quotedValues(LR"(a "one" b "two" c)"),
             (vector<wstring>{L"one", L"two"}));
    CHECK_EQ(PostgresHba::quotedValues(L"no quotes here"), (vector<wstring>{}));
}

// ===== SqlSyntaxHighlighter ==================================================

static vector<HighlightSpan> spans(const wstring& s) {
    return SqlSyntaxHighlighter::computeSpans(s);
}

TEST(highlighter_keyword_number_comment) {
    CHECK_EQ(spans(L"SELECT 1 -- x"),
             (vector<HighlightSpan>{{0, 6, SyntaxToken::Keyword},
                                    {7, 1, SyntaxToken::Number},
                                    {9, 4, SyntaxToken::Comment}}));
}

TEST(highlighter_string_and_comment_win) {
    CHECK_EQ(spans(L"WHERE x = 'a' -- note"),
             (vector<HighlightSpan>{{0, 5, SyntaxToken::Keyword},
                                    {10, 3, SyntaxToken::StringLiteral},
                                    {14, 7, SyntaxToken::Comment}}));
}

TEST(highlighter_keyword_inside_comment_not_highlighted) {
    CHECK_EQ(spans(L"-- SELECT 1"),
             (vector<HighlightSpan>{{0, 11, SyntaxToken::Comment}}));
}

TEST(highlighter_decimal_number) {
    CHECK_EQ(spans(L"x = 3.14"),
             (vector<HighlightSpan>{{4, 4, SyntaxToken::Number}}));
}

TEST(highlighter_digits_in_identifier_are_not_a_number) {
    CHECK_EQ(spans(L"col123 FROM t"),
             (vector<HighlightSpan>{{7, 4, SyntaxToken::Keyword}}));
}

// ===== runner ================================================================

int main() {
    for (const auto& tc : th::registry()) {
        th::g_currentTest = tc.name;
        tc.fn();
    }
    const int passed = th::g_checks - th::g_failures;
    std::cout << "\n"
              << (th::g_failures == 0 ? "PASSED" : "FAILED") << ": " << passed
              << "/" << th::g_checks << " checks across " << th::registry().size()
              << " tests";
    if (th::g_failures) std::cout << "  (" << th::g_failures << " failed)";
    std::cout << "\n";
    return th::g_failures == 0 ? 0 : 1;
}
