// SPDX-License-Identifier: GPL-3.0-or-later
// DatabaseProvider — the engine-agnostic interface implemented by
// SqliteProvider (P1) and PostgresProvider (P4). Mirrors the Swift
// DatabaseProvider protocol; `connect` reports failure via an out-param instead
// of throwing.
#pragma once

#include <string>

#include "models/DatabaseConnection.h"
#include "models/DatabaseEngine.h"
#include "models/QueryResult.h"

namespace sqlterm {

class DatabaseProvider {
public:
    virtual ~DatabaseProvider() = default;

    virtual DatabaseEngine engine() const = 0;
    virtual bool isConnected() const = 0;
    virtual bool isSSLActive() const = 0;
    virtual std::wstring statusMessage() const = 0;

    // Returns true on success; on failure returns false and sets `error`.
    virtual bool connect(const DatabaseConnection& config, std::wstring& error) = 0;
    virtual QueryResult execute(const std::wstring& sql) = 0;
    // Interrupt a running query; safe to call from another thread (P3).
    virtual void cancel() = 0;
    virtual void disconnect() = 0;
};

}  // namespace sqlterm
