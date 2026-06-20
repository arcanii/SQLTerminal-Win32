// SPDX-License-Identifier: GPL-3.0-or-later
// JSON-backed persistence stores (port of the UserDefaults stores in
// ConnectionProfile.swift / QueryHistory.swift). Files live under
// %APPDATA%\SQLTerminal (override with the SQLT_DATA_DIR env var, used by tests).
// Decode failure yields an empty list; mutations are mutex-guarded.
#pragma once

#include <string>
#include <vector>

#include "models/ConnectionProfile.h"
#include "models/QueryHistory.h"

namespace sqlterm {

// The directory where JSON stores are read/written (created if missing).
std::wstring storageDir();

// Recents: auto, capped at 10, deduped by ConnectionProfile.id(), MRU-first.
class RecentConnectionsStore {
public:
    static std::vector<ConnectionProfile> load();
    static void add(const ConnectionProfile& profile);
    static void clear();
};

// Saved profiles: uncapped, deduped by id(), sorted by displayName (ci).
class SavedProfilesStore {
public:
    static std::vector<ConnectionProfile> load();
    static void save(const ConnectionProfile& profile);
    static void remove(const ConnectionProfile& profile);
};

// Query history: capped at 200, deduped by exact trimmed SQL, runCount bumped.
class QueryHistoryStore {
public:
    static std::vector<QueryHistoryEntry> load();
    static void record(const std::wstring& rawSql, double nowEpoch);
    static void clear();
};

// Snippets: uncapped, upsert by case-insensitive name, sorted by name (ci).
class SnippetStore {
public:
    static std::vector<QuerySnippet> load();
    static void save(const std::wstring& rawName, const std::wstring& rawSql);
    static void remove(const QuerySnippet& snippet);
};

}  // namespace sqlterm
