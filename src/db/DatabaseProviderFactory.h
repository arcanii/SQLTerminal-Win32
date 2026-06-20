// SPDX-License-Identifier: GPL-3.0-or-later
// Maps a DatabaseEngine to a concrete provider. Mirrors DatabaseProviderFactory
// in the Swift app. PostgresProvider is added in P4.
#pragma once

#include <memory>

#include "db/DatabaseProvider.h"
#include "db/SqliteProvider.h"
#include "models/DatabaseEngine.h"

namespace sqlterm {

inline std::unique_ptr<DatabaseProvider> makeProvider(DatabaseEngine engine) {
    switch (engine) {
        case DatabaseEngine::Sqlite:
            return std::make_unique<SqliteProvider>();
        case DatabaseEngine::Postgres:
            return nullptr;  // P4
    }
    return nullptr;
}

}  // namespace sqlterm
