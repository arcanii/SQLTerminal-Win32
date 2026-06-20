// SPDX-License-Identifier: GPL-3.0-or-later
#define _CRT_SECURE_NO_WARNINGS  // _wgetenv
#include "persistence/Stores.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <objbase.h>  // CoCreateGuid, StringFromGUID2
#include <shlobj.h>   // SHGetKnownFolderPath
#include <windows.h>

#include <nlohmann/json.hpp>

#include "platform/Encoding.h"

namespace sqlterm {
namespace {

using json = nlohmann::json;

std::mutex g_mutex;

namespace fs = std::filesystem;

// ---- small helpers ----------------------------------------------------------

std::wstring trimWs(const std::wstring& s) {
    auto isWs = [](wchar_t c) {
        return c == L' ' || c == L'\t' || c == L'\n' || c == L'\r' || c == L'\v' || c == L'\f';
    };
    size_t b = 0, e = s.size();
    while (b < e && isWs(s[b])) ++b;
    while (e > b && isWs(s[e - 1])) --e;
    return s.substr(b, e - b);
}

// Locale-aware case-insensitive compare (mirrors localizedCaseInsensitiveCompare).
int ciCompare(const std::wstring& a, const std::wstring& b) {
    const int r = CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE, a.c_str(),
                                  static_cast<int>(a.size()), b.c_str(),
                                  static_cast<int>(b.size()), nullptr, nullptr, 0);
    return (r == 0) ? 0 : (r - 2);  // CSTR_LESS_THAN=1, EQUAL=2, GREATER=3
}
bool ciEqual(const std::wstring& a, const std::wstring& b) { return ciCompare(a, b) == 0; }

std::wstring newGuid() {
    GUID g{};
    if (CoCreateGuid(&g) != S_OK) return L"";
    wchar_t buf[64];
    const int n = StringFromGUID2(g, buf, 64);
    return (n > 0) ? std::wstring(buf, static_cast<size_t>(n - 1)) : std::wstring();
}

std::wstring filePathFor(const wchar_t* name) { return storageDir() + L"\\" + name; }

std::optional<std::string> readFile(const std::wstring& path) {
    std::ifstream in(fs::path(path), std::ios::binary);
    if (!in) return std::nullopt;
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return content;
}

void writeFile(const std::wstring& path, const std::string& content) {
    std::ofstream out(fs::path(path), std::ios::binary | std::ios::trunc);
    if (out) out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

// ---- enum <-> string --------------------------------------------------------

std::string engineToStr(DatabaseEngine e) {
    return e == DatabaseEngine::Postgres ? "PostgreSQL" : "SQLite";
}
DatabaseEngine engineFromStr(const std::string& s) {
    return s == "PostgreSQL" ? DatabaseEngine::Postgres : DatabaseEngine::Sqlite;
}
std::string sslToStr(SslMode m) {
    switch (m) {
        case SslMode::Off: return "off";
        case SslMode::Require: return "require";
        case SslMode::Prefer: return "prefer";
    }
    return "prefer";
}
SslMode sslFromStr(const std::string& s) {
    if (s == "off") return SslMode::Off;
    if (s == "require") return SslMode::Require;
    return SslMode::Prefer;
}

// ---- json conversions -------------------------------------------------------

json toJson(const ConnectionProfile& p) {
    json j;
    if (p.name) j["name"] = utf8FromWide(*p.name);
    j["engine"] = engineToStr(p.engine);
    j["filePath"] = utf8FromWide(p.filePath);
    j["host"] = utf8FromWide(p.host);
    j["port"] = utf8FromWide(p.port);
    j["databaseName"] = utf8FromWide(p.databaseName);
    j["username"] = utf8FromWide(p.username);
    if (p.sslMode) j["sslMode"] = sslToStr(*p.sslMode);
    return j;
}
ConnectionProfile profileFromJson(const json& j) {
    ConnectionProfile p;
    if (j.contains("name") && !j["name"].is_null())
        p.name = wideFromUtf8(j["name"].get<std::string>());
    p.engine = engineFromStr(j.value("engine", std::string("SQLite")));
    p.filePath = wideFromUtf8(j.value("filePath", std::string()));
    p.host = wideFromUtf8(j.value("host", std::string()));
    p.port = wideFromUtf8(j.value("port", std::string()));
    p.databaseName = wideFromUtf8(j.value("databaseName", std::string()));
    p.username = wideFromUtf8(j.value("username", std::string()));
    if (j.contains("sslMode") && !j["sslMode"].is_null())
        p.sslMode = sslFromStr(j["sslMode"].get<std::string>());
    return p;
}

json toJson(const QueryHistoryEntry& e) {
    return json{{"id", utf8FromWide(e.id)},
                {"sql", utf8FromWide(e.sql)},
                {"lastRun", e.lastRunEpoch},
                {"runCount", e.runCount}};
}
QueryHistoryEntry historyFromJson(const json& j) {
    QueryHistoryEntry e;
    e.id = wideFromUtf8(j.value("id", std::string()));
    e.sql = wideFromUtf8(j.value("sql", std::string()));
    e.lastRunEpoch = j.value("lastRun", 0.0);
    e.runCount = j.value("runCount", 0LL);
    return e;
}

json toJson(const QuerySnippet& s) {
    return json{{"id", utf8FromWide(s.id)},
                {"name", utf8FromWide(s.name)},
                {"sql", utf8FromWide(s.sql)}};
}
QuerySnippet snippetFromJson(const json& j) {
    QuerySnippet s;
    s.id = wideFromUtf8(j.value("id", std::string()));
    s.name = wideFromUtf8(j.value("name", std::string()));
    s.sql = wideFromUtf8(j.value("sql", std::string()));
    return s;
}

template <class T, class FromJ>
std::vector<T> loadVec(const wchar_t* file, FromJ fromJ) {
    const auto content = readFile(filePathFor(file));
    if (!content) return {};
    try {
        const json arr = json::parse(*content);
        if (!arr.is_array()) return {};
        std::vector<T> out;
        out.reserve(arr.size());
        for (const auto& e : arr) out.push_back(fromJ(e));
        return out;
    } catch (...) {
        return {};  // decode failure -> empty
    }
}

template <class T, class ToJ>
void saveVec(const wchar_t* file, const std::vector<T>& list, ToJ toJ) {
    json arr = json::array();
    for (const auto& x : list) arr.push_back(toJ(x));
    writeFile(filePathFor(file), arr.dump(2));
}

constexpr wchar_t kRecents[] = L"recents.json";
constexpr wchar_t kProfiles[] = L"profiles.json";
constexpr wchar_t kHistory[] = L"history.json";
constexpr wchar_t kSnippets[] = L"snippets.json";

}  // namespace

std::wstring storageDir() {
    std::wstring dir;
    if (const wchar_t* override = _wgetenv(L"SQLT_DATA_DIR")) {
        dir = override;
    } else {
        PWSTR p = nullptr;
        if (SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p) == S_OK && p) {
            dir = p;
            CoTaskMemFree(p);
        }
        dir += L"\\SQLTerminal";
    }
    std::error_code ec;
    fs::create_directories(fs::path(dir), ec);
    return dir;
}

// ---- RecentConnectionsStore -------------------------------------------------

std::vector<ConnectionProfile> RecentConnectionsStore::load() {
    return loadVec<ConnectionProfile>(kRecents, profileFromJson);
}
void RecentConnectionsStore::add(const ConnectionProfile& profile) {
    if (!profile.isValid()) return;
    std::lock_guard<std::mutex> lk(g_mutex);
    auto list = loadVec<ConnectionProfile>(kRecents, profileFromJson);
    const std::wstring pid = profile.id();
    list.erase(std::remove_if(list.begin(), list.end(),
                              [&](const ConnectionProfile& p) { return p.id() == pid; }),
               list.end());
    list.insert(list.begin(), profile);
    if (list.size() > 10) list.resize(10);
    saveVec(kRecents, list, [](const ConnectionProfile& p) { return toJson(p); });
}
void RecentConnectionsStore::clear() {
    std::lock_guard<std::mutex> lk(g_mutex);
    std::error_code ec;
    fs::remove(fs::path(filePathFor(kRecents)), ec);
}

// ---- SavedProfilesStore -----------------------------------------------------

std::vector<ConnectionProfile> SavedProfilesStore::load() {
    return loadVec<ConnectionProfile>(kProfiles, profileFromJson);
}
void SavedProfilesStore::save(const ConnectionProfile& profile) {
    if (!profile.isValid()) return;
    std::lock_guard<std::mutex> lk(g_mutex);
    auto list = loadVec<ConnectionProfile>(kProfiles, profileFromJson);
    const std::wstring pid = profile.id();
    list.erase(std::remove_if(list.begin(), list.end(),
                              [&](const ConnectionProfile& p) { return p.id() == pid; }),
               list.end());
    list.push_back(profile);
    std::sort(list.begin(), list.end(), [](const ConnectionProfile& a, const ConnectionProfile& b) {
        return ciCompare(a.displayName(), b.displayName()) < 0;
    });
    saveVec(kProfiles, list, [](const ConnectionProfile& p) { return toJson(p); });
}
void SavedProfilesStore::remove(const ConnectionProfile& profile) {
    std::lock_guard<std::mutex> lk(g_mutex);
    auto list = loadVec<ConnectionProfile>(kProfiles, profileFromJson);
    const std::wstring pid = profile.id();
    list.erase(std::remove_if(list.begin(), list.end(),
                              [&](const ConnectionProfile& p) { return p.id() == pid; }),
               list.end());
    saveVec(kProfiles, list, [](const ConnectionProfile& p) { return toJson(p); });
}

// ---- QueryHistoryStore ------------------------------------------------------

std::vector<QueryHistoryEntry> QueryHistoryStore::load() {
    return loadVec<QueryHistoryEntry>(kHistory, historyFromJson);
}
void QueryHistoryStore::record(const std::wstring& rawSql, double nowEpoch) {
    const std::wstring sql = trimWs(rawSql);
    if (sql.empty()) return;
    std::lock_guard<std::mutex> lk(g_mutex);
    auto list = loadVec<QueryHistoryEntry>(kHistory, historyFromJson);
    const auto it = std::find_if(list.begin(), list.end(),
                                 [&](const QueryHistoryEntry& e) { return e.sql == sql; });
    if (it != list.end()) {
        QueryHistoryEntry entry = *it;
        list.erase(it);
        entry.lastRunEpoch = nowEpoch;
        entry.runCount += 1;
        list.insert(list.begin(), entry);
    } else {
        QueryHistoryEntry entry;
        entry.id = newGuid();
        entry.sql = sql;
        entry.lastRunEpoch = nowEpoch;
        entry.runCount = 1;
        list.insert(list.begin(), entry);
    }
    if (list.size() > 200) list.resize(200);
    saveVec(kHistory, list, [](const QueryHistoryEntry& e) { return toJson(e); });
}
void QueryHistoryStore::clear() {
    std::lock_guard<std::mutex> lk(g_mutex);
    std::error_code ec;
    fs::remove(fs::path(filePathFor(kHistory)), ec);
}

// ---- SnippetStore -----------------------------------------------------------

std::vector<QuerySnippet> SnippetStore::load() {
    return loadVec<QuerySnippet>(kSnippets, snippetFromJson);
}
void SnippetStore::save(const std::wstring& rawName, const std::wstring& rawSql) {
    const std::wstring name = trimWs(rawName);
    const std::wstring sql = trimWs(rawSql);
    if (name.empty() || sql.empty()) return;
    std::lock_guard<std::mutex> lk(g_mutex);
    auto list = loadVec<QuerySnippet>(kSnippets, snippetFromJson);
    list.erase(std::remove_if(list.begin(), list.end(),
                              [&](const QuerySnippet& s) { return ciEqual(s.name, name); }),
               list.end());
    QuerySnippet snip;
    snip.id = newGuid();
    snip.name = name;
    snip.sql = sql;
    list.push_back(snip);
    std::sort(list.begin(), list.end(), [](const QuerySnippet& a, const QuerySnippet& b) {
        return ciCompare(a.name, b.name) < 0;
    });
    saveVec(kSnippets, list, [](const QuerySnippet& s) { return toJson(s); });
}
void SnippetStore::remove(const QuerySnippet& snippet) {
    std::lock_guard<std::mutex> lk(g_mutex);
    auto list = loadVec<QuerySnippet>(kSnippets, snippetFromJson);
    list.erase(std::remove_if(list.begin(), list.end(),
                              [&](const QuerySnippet& s) { return s.id == snippet.id; }),
               list.end());
    saveVec(kSnippets, list, [](const QuerySnippet& s) { return toJson(s); });
}

}  // namespace sqlterm
