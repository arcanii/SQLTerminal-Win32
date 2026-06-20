// SPDX-License-Identifier: GPL-3.0-or-later
// SqliteProvider — SQLite engine over the vendored sqlite3 amalgamation. Ported
// from SQLTerminal/Providers/SQLiteProvider.swift (the macOS security-scoped
// resource dance is dropped). cancel() uses sqlite3_interrupt; the connection
// stays usable afterwards.
#pragma once

#include <string>

#include "db/DatabaseProvider.h"

struct sqlite3;  // forward declaration (avoids pulling sqlite3.h into headers)

namespace sqlterm {

class SqliteProvider : public DatabaseProvider {
public:
    SqliteProvider() = default;
    ~SqliteProvider() override;

    SqliteProvider(const SqliteProvider&) = delete;
    SqliteProvider& operator=(const SqliteProvider&) = delete;

    DatabaseEngine engine() const override { return DatabaseEngine::Sqlite; }
    bool isConnected() const override { return isConnected_; }
    bool isSSLActive() const override { return false; }  // local file, no network
    std::wstring statusMessage() const override { return status_; }

    bool connect(const DatabaseConnection& config, std::wstring& error) override;
    QueryResult execute(const std::wstring& sql) override;
    void cancel() override;
    void disconnect() override;

private:
    sqlite3* db_ = nullptr;
    bool isConnected_ = false;
    std::wstring status_ = L"Disconnected";
};

}  // namespace sqlterm
