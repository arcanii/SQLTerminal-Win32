// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/DotCommandHandler.h"

namespace sqlterm {
namespace {

std::wstring trimWs(const std::wstring& s) {
    auto ws = [](wchar_t c) {
        return c == L' ' || c == L'\t' || c == L'\n' || c == L'\r' || c == L'\v' || c == L'\f';
    };
    size_t b = 0, e = s.size();
    while (b < e && ws(s[b])) ++b;
    while (e > b && ws(s[e - 1])) --e;
    return s.substr(b, e - b);
}
std::wstring lower(const std::wstring& s) {
    std::wstring o = s;
    for (auto& c : o)
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c + 32);
    return o;
}

DotCommandResult sql(const std::wstring& s) { return {DotKind::Sql, {s}, L""}; }
DotCommandResult multi(std::vector<std::wstring> v) { return {DotKind::MultiSql, std::move(v), L""}; }
DotCommandResult msg(const std::wstring& t) { return {DotKind::Message, {}, t}; }
DotCommandResult clearResult() { return {DotKind::Clear, {}, L""}; }
DotCommandResult reconnect(const std::wstring& db) { return {DotKind::Reconnect, {}, db}; }

const wchar_t* kSqliteHelp =
    L"Available dot-commands (SQLite):\n\n"
    L"Tables & Schema\n"
    L"  .tables                 List all tables\n"
    L"  .views                  List all views\n"
    L"  .indexes [table]        List indexes (optionally for a table)\n"
    L"  .schema [table]         Show CREATE statements\n"
    L"  .columns <table>        Show column info for a table\n\n"
    L"Database Info\n"
    L"  .dbinfo                 Show database properties\n"
    L"  .size                   Show database size in bytes\n"
    L"  .journal                Show journal mode\n"
    L"  .encoding               Show database encoding\n\n"
    L"Data Inspection\n"
    L"  .count <table>          Count rows in a table\n"
    L"  .first <table>          Show first 10 rows\n"
    L"  .last <table>           Show last 10 rows\n\n"
    L"Foreign Keys\n"
    L"  .fk <table>             Show foreign keys for a table\n\n"
    L"Terminal\n"
    L"  .clear                  Clear the terminal output\n\n"
    L"Other\n"
    L"  .help                   Show this help";

const wchar_t* kPostgresHelp =
    L"Available dot-commands (PostgreSQL):\n\n"
    L"Tables & Schema\n"
    L"  .tables                 List all tables in public schema\n"
    L"  .views                  List all views in public schema\n"
    L"  .indexes [table]        List indexes (optionally for a table)\n"
    L"  .schema [table]         Show column definitions\n"
    L"  .columns <table>        Show column info for a table\n\n"
    L"Database Info\n"
    L"  .dbinfo                 Show database properties\n"
    L"  .size                   Show database size\n"
    L"  .encoding               Show server encoding\n"
    L"  .schemas                List all schemas\n"
    L"  .databases              List all databases\n\n"
    L"Data Inspection\n"
    L"  .count <table>          Count rows in a table\n"
    L"  .first <table>          Show first 10 rows\n"
    L"  .last <table>           Show last 10 rows\n\n"
    L"Foreign Keys\n"
    L"  .fk <table>             Show foreign keys for a table\n\n"
    L"Connection\n"
    L"  .connect <database>     Switch to a different database\n"
    L"  .use <database>         Same as .connect\n\n"
    L"Terminal\n"
    L"  .clear                  Clear the terminal output\n\n"
    L"Other\n"
    L"  .help                   Show this help";

bool isPg(DatabaseEngine e) { return e == DatabaseEngine::Postgres; }

}  // namespace

std::optional<DotCommandResult> handleDotCommand(const std::wstring& input, DatabaseEngine engine) {
    const std::wstring trimmed = trimWs(input);
    if (trimmed.empty() || trimmed[0] != L'.') return std::nullopt;

    size_t cmdEnd = 0;
    auto ws = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\n' || c == L'\r'; };
    while (cmdEnd < trimmed.size() && !ws(trimmed[cmdEnd])) ++cmdEnd;
    const std::wstring command = lower(trimmed.substr(0, cmdEnd));
    size_t argStart = cmdEnd;
    while (argStart < trimmed.size() && ws(trimmed[argStart])) ++argStart;
    const bool hasArg = argStart < trimmed.size();
    const std::wstring arg = hasArg ? trimmed.substr(argStart) : std::wstring();

    if (command == L".tables")
        return isPg(engine)
                   ? sql(L"SELECT tablename FROM pg_tables WHERE schemaname = 'public' ORDER BY tablename;")
                   : sql(L"SELECT name FROM sqlite_master WHERE type='table' ORDER BY name;");

    if (command == L".views")
        return isPg(engine)
                   ? sql(L"SELECT viewname FROM pg_views WHERE schemaname = 'public' ORDER BY viewname;")
                   : sql(L"SELECT name FROM sqlite_master WHERE type='view' ORDER BY name;");

    if (command == L".indexes") {
        if (isPg(engine)) {
            if (hasArg)
                return sql(L"SELECT indexname, indexdef FROM pg_indexes WHERE tablename = '" + arg +
                           L"' ORDER BY indexname;");
            return sql(L"SELECT tablename, indexname FROM pg_indexes WHERE schemaname = 'public' "
                       L"ORDER BY tablename, indexname;");
        }
        if (hasArg)
            return sql(L"SELECT name FROM sqlite_master WHERE type='index' AND tbl_name='" + arg +
                       L"' ORDER BY name;");
        return sql(L"SELECT name, tbl_name FROM sqlite_master WHERE type='index' ORDER BY tbl_name, name;");
    }

    if (command == L".schema") {
        if (isPg(engine)) {
            if (hasArg)
                return sql(L"SELECT column_name, data_type, is_nullable, column_default\n"
                           L"FROM information_schema.columns\n"
                           L"WHERE table_schema = 'public' AND table_name = '" + arg + L"'\n"
                           L"ORDER BY ordinal_position;");
            return sql(L"SELECT table_name FROM information_schema.tables WHERE table_schema = "
                       L"'public' ORDER BY table_name;");
        }
        if (hasArg) return sql(L"SELECT sql FROM sqlite_master WHERE name='" + arg + L"';");
        return sql(L"SELECT sql FROM sqlite_master WHERE sql IS NOT NULL ORDER BY name;");
    }

    if (command == L".columns") {
        if (!hasArg) return msg(L"Usage: .columns <table_name>");
        if (isPg(engine))
            return sql(L"SELECT column_name, data_type, is_nullable, column_default\n"
                       L"FROM information_schema.columns\n"
                       L"WHERE table_schema = 'public' AND table_name = '" + arg + L"'\n"
                       L"ORDER BY ordinal_position;");
        return sql(L"PRAGMA table_info('" + arg + L"');");
    }

    if (command == L".dbinfo") {
        if (isPg(engine))
            return multi({L"SELECT version();",
                          L"SELECT current_database() AS database, current_user AS user, "
                          L"inet_server_addr() AS host, inet_server_port() AS port;",
                          L"SELECT pg_size_pretty(pg_database_size(current_database())) AS database_size;"});
        return multi({L"SELECT 'SQLite version' AS property, sqlite_version() AS value;",
                      L"PRAGMA page_size;", L"PRAGMA page_count;", L"PRAGMA journal_mode;"});
    }

    if (command == L".size")
        return isPg(engine)
                   ? sql(L"SELECT pg_size_pretty(pg_database_size(current_database())) AS database_size;")
                   : sql(L"SELECT page_count * page_size AS size_bytes FROM pragma_page_count(), "
                         L"pragma_page_size();");

    if (command == L".fk" || command == L".foreignkeys") {
        if (!hasArg) return msg(L"Usage: .fk <table_name>");
        if (isPg(engine))
            return sql(L"SELECT conname AS constraint_name,\n"
                       L"       conrelid::regclass AS table_name,\n"
                       L"       a.attname AS column_name,\n"
                       L"       confrelid::regclass AS foreign_table,\n"
                       L"       af.attname AS foreign_column\n"
                       L"FROM pg_constraint c\n"
                       L"JOIN pg_attribute a ON a.attnum = ANY(c.conkey) AND a.attrelid = c.conrelid\n"
                       L"JOIN pg_attribute af ON af.attnum = ANY(c.confkey) AND af.attrelid = c.confrelid\n"
                       L"WHERE c.contype = 'f' AND c.conrelid::regclass::text = '" + arg + L"';");
        return sql(L"PRAGMA foreign_key_list('" + arg + L"');");
    }

    if (command == L".count") {
        if (!hasArg) return msg(L"Usage: .count <table_name>");
        return sql(L"SELECT COUNT(*) AS row_count FROM \"" + arg + L"\";");
    }
    if (command == L".first") {
        if (!hasArg) return msg(L"Usage: .first <table_name>");
        return sql(L"SELECT * FROM \"" + arg + L"\" LIMIT 10;");
    }
    if (command == L".last") {
        if (!hasArg) return msg(L"Usage: .last <table_name>");
        return isPg(engine) ? sql(L"SELECT * FROM \"" + arg + L"\" ORDER BY ctid DESC LIMIT 10;")
                            : sql(L"SELECT * FROM \"" + arg + L"\" ORDER BY rowid DESC LIMIT 10;");
    }

    if (command == L".schemas")
        return isPg(engine) ? sql(L"SELECT schema_name FROM information_schema.schemata ORDER BY schema_name;")
                            : msg(L".schemas is only available for PostgreSQL.");
    if (command == L".databases")
        return isPg(engine)
                   ? sql(L"SELECT datname FROM pg_database WHERE datistemplate = false ORDER BY datname;")
                   : msg(L".databases is only available for PostgreSQL.");

    if (command == L".connect" || command == L".use") {
        if (!isPg(engine)) return msg(command + L" is only available for PostgreSQL.");
        if (hasArg) return reconnect(arg);
        return msg(L"Usage: " + command + L" <database_name>");
    }

    if (command == L".journal")
        return isPg(engine) ? msg(L".journal is only available for SQLite.")
                            : sql(L"PRAGMA journal_mode;");
    if (command == L".encoding")
        return isPg(engine) ? sql(L"SHOW server_encoding;") : sql(L"PRAGMA encoding;");

    if (command == L".clear") return clearResult();
    if (command == L".help") return msg(isPg(engine) ? kPostgresHelp : kSqliteHelp);

    return msg(L"Unknown command: " + command + L"\nType .help for available commands.");
}

}  // namespace sqlterm
