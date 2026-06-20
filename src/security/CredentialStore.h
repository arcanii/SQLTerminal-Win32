// SPDX-License-Identifier: GPL-3.0-or-later
// CredentialStore — Postgres passwords in the Windows Credential Manager.
// Port of KeychainHelper.swift; the account key is preserved verbatim
// ("<user>@<host>:<port>/<db>") and the Credential Manager TargetName is
// "SQLTerminal:" + account. SQLite has no password.
#pragma once

#include <optional>
#include <string>

#include "models/DatabaseConnection.h"

namespace sqlterm {

class CredentialStore {
public:
    static std::wstring accountKey(const DatabaseConnection& connection);

    static bool savePassword(const std::wstring& account, const std::wstring& password);
    static std::optional<std::wstring> password(const std::wstring& account);
    // Idempotent: succeeds whether or not the item existed.
    static bool deletePassword(const std::wstring& account);
};

}  // namespace sqlterm
