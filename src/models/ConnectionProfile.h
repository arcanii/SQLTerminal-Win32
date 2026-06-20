// SPDX-License-Identifier: GPL-3.0-or-later
// ConnectionProfile — a non-secret snapshot of a connection, used for the auto
// Recents list (no name) and explicitly Saved profiles (named). The password is
// never stored here (it lives in the Credential Manager — see CredentialStore).
// Port of ConnectionProfile.swift.
#pragma once

#include <optional>
#include <string>

#include "models/DatabaseConnection.h"

namespace sqlterm {

struct ConnectionProfile {
    std::optional<std::wstring> name;  // set for saved profiles; nullopt for recents
    DatabaseEngine engine = DatabaseEngine::Sqlite;
    std::wstring filePath;
    std::wstring host;
    std::wstring port;
    std::wstring databaseName;
    std::wstring username;
    std::optional<SslMode> sslMode;  // optional for back-compat

    ConnectionProfile() = default;
    explicit ConnectionProfile(const DatabaseConnection& c,
                               std::optional<std::wstring> profileName = std::nullopt)
        : name(std::move(profileName)),
          engine(c.engine),
          filePath(c.filePath),
          host(c.host),
          port(c.port),
          databaseName(c.databaseName),
          username(c.username),
          sslMode(c.sslMode) {}

    // Stable identity: by name for saved profiles, by connection for recents.
    std::wstring id() const {
        if (name && !name->empty()) return L"named:" + *name;
        switch (engine) {
            case DatabaseEngine::Sqlite:
                return L"sqlite:" + filePath;
            case DatabaseEngine::Postgres:
                return L"postgres:" + username + L"@" + host + L":" + port + L"/" + databaseName;
        }
        return L"";
    }

    std::wstring displayName() const {
        if (name && !name->empty()) return *name;
        switch (engine) {
            case DatabaseEngine::Sqlite: {
                const size_t slash = filePath.find_last_of(L"\\/");
                const std::wstring leaf =
                    (slash == std::wstring::npos) ? filePath : filePath.substr(slash + 1);
                return L"SQLite — " + (leaf.empty() ? filePath : leaf);
            }
            case DatabaseEngine::Postgres:
                return username + L"@" + host + L":" + port + L"/" + databaseName;
        }
        return L"";
    }

    bool isValid() const {
        switch (engine) {
            case DatabaseEngine::Sqlite:
                return !filePath.empty();
            case DatabaseEngine::Postgres:
                return !host.empty() && !databaseName.empty() && !username.empty();
        }
        return false;
    }
};

}  // namespace sqlterm
