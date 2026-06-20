// SPDX-License-Identifier: GPL-3.0-or-later
// Pure (libpq-free) builder for a libpq connection string from a
// DatabaseConnection. Kept separate from PostgresProvider so it can be
// unit-tested without linking libpq.
//
// libpq replaces the Swift app's manual credential-probe + SSL fallback: a single
// connection string with sslmode=disable/prefer/require does it all (`prefer`
// auto-falls back to non-SSL), and libpq negotiates SCRAM/MD5/cleartext/trust.
#pragma once

#include <string>

#include "models/DatabaseConnection.h"
#include "platform/Encoding.h"

namespace sqlterm {

inline const char* sslModeKeyword(SslMode mode) {
    switch (mode) {
        case SslMode::Off: return "disable";
        case SslMode::Require: return "require";
        case SslMode::Prefer: return "prefer";
    }
    return "prefer";
}

// Single-quote a conninfo value, backslash-escaping ' and \ (libpq rules).
inline std::string pgQuote(const std::wstring& w) {
    const std::string v = utf8FromWide(w);
    std::string out = "'";
    for (const char ch : v) {
        if (ch == '\\' || ch == '\'') out.push_back('\\');
        out.push_back(ch);
    }
    out.push_back('\'');
    return out;
}

inline std::string buildConnInfo(const DatabaseConnection& c, SslMode mode) {
    std::string s;
    auto add = [&](const char* key, const std::wstring& val) {
        if (val.empty()) return;
        if (!s.empty()) s += ' ';
        s += key;
        s += '=';
        s += pgQuote(val);
    };
    add("host", c.host);
    add("port", c.port);
    add("dbname", c.databaseName);
    add("user", c.username);
    add("password", c.password);
    if (!s.empty()) s += ' ';
    s += "sslmode='";
    s += sslModeKeyword(mode);
    s += "'";
    s += " connect_timeout='10'";  // don't hang forever on an unreachable host
    return s;
}

}  // namespace sqlterm
