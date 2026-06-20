// SPDX-License-Identifier: GPL-3.0-or-later
// PostgresProvider — PostgreSQL engine over libpq. Port of PostgresProvider.swift
// (PostgresClientKit). libpq collapses the Swift app's 4-credential probe loop and
// manual SSL fallback into one PQconnectdb. cancel() uses an out-of-band PGcancel
// (PQgetCancel snapshot + PQcancel), so the connection survives a cancel.
#pragma once

#include <string>

#include "db/DatabaseProvider.h"

struct pg_conn;    // libpq PGconn
struct pg_cancel;  // libpq PGcancel

namespace sqlterm {

class PostgresProvider : public DatabaseProvider {
public:
    PostgresProvider() = default;
    ~PostgresProvider() override;

    PostgresProvider(const PostgresProvider&) = delete;
    PostgresProvider& operator=(const PostgresProvider&) = delete;

    DatabaseEngine engine() const override { return DatabaseEngine::Postgres; }
    bool isConnected() const override { return isConnected_; }
    bool isSSLActive() const override { return isSSLActive_; }
    std::wstring statusMessage() const override { return status_; }

    bool connect(const DatabaseConnection& config, std::wstring& error) override;
    QueryResult execute(const std::wstring& sql) override;
    void cancel() override;
    void disconnect() override;

private:
    pg_conn* conn_ = nullptr;
    pg_cancel* cancel_ = nullptr;  // snapshot taken right after connect
    bool isConnected_ = false;
    bool isSSLActive_ = false;
    std::wstring status_ = L"Disconnected";
};

}  // namespace sqlterm
