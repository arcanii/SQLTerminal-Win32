// SPDX-License-Identifier: GPL-3.0-or-later
// Modal Connection dialog (port of ConnectionSheet/ConnectionViewModel): engine
// picker, SQLite file or PostgreSQL fields, password show/hide, SSL mode, a
// recents/saved-profiles picker, and "save as profile" / "remember password".
// Returns the chosen connection + options; the caller connects (off-thread) and,
// on success, records recents / saves the profile / stores the password.
#pragma once

#include <optional>
#include <string>

#include "models/DatabaseConnection.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace sqlterm {

struct ConnectionRequest {
    DatabaseConnection connection;
    std::optional<std::wstring> saveAsName;  // save as a named profile if set
    bool rememberPassword = false;           // persist password in Credential Manager
};

// Runs the dialog modally over `owner`. Returns the request on Connect, or
// nullopt if cancelled.
std::optional<ConnectionRequest> showConnectionDialog(HWND owner);

}  // namespace sqlterm
