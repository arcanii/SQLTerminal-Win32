// SPDX-License-Identifier: GPL-3.0-or-later
// Persistent, app-wide query history and named snippets (port of
// QueryHistory.swift). Stored as JSON files; see persistence/Stores.h.
#pragma once

#include <string>

namespace sqlterm {

// A previously-executed query, remembered across launches.
struct QueryHistoryEntry {
    std::wstring id;          // GUID string (generated on first insert)
    std::wstring sql;
    double lastRunEpoch = 0;  // Unix seconds
    long long runCount = 0;
};

// A saved, named, reusable query.
struct QuerySnippet {
    std::wstring id;  // GUID string
    std::wstring name;
    std::wstring sql;
};

}  // namespace sqlterm
