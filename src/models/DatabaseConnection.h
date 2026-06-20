// SPDX-License-Identifier: GPL-3.0-or-later
// DatabaseConnection — in-memory live connection config. Ported from
// SQLTerminal/Models/DatabaseConnection.swift (the macOS security-scoped-URL
// field is dropped; Windows has no per-file sandbox). host/port are strings.
#pragma once

#include <string>

#include "models/DatabaseEngine.h"

namespace sqlterm {

enum class SslMode { Off, Prefer, Require };

struct DatabaseConnection {
    DatabaseEngine engine = DatabaseEngine::Sqlite;
    std::wstring filePath;                 // SQLite
    std::wstring host = L"localhost";      // Postgres
    std::wstring port = L"5432";           // Postgres (string, may be empty)
    std::wstring databaseName;
    std::wstring username;
    std::wstring password;                 // in-memory only; never persisted
    SslMode sslMode = SslMode::Prefer;
};

}  // namespace sqlterm
