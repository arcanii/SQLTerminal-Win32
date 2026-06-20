// SPDX-License-Identifier: GPL-3.0-or-later
// PostgresHba — pure helpers for turning a Postgres "no pg_hba.conf entry for
// host ..." rejection into the exact `host` rule that would let the connection
// in. Byte-for-byte port of SQLTerminal/Core/PostgresHBA.swift.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace sqlcore {

class PostgresHba {
public:
    // If `message` is a Postgres "no pg_hba.conf entry for host ..." rejection,
    // returns the pg_hba.conf line that would permit the connection; else nullopt.
    static std::optional<std::wstring> suggestedLine(const std::wstring& message);

    // The substrings enclosed in double quotes, in order of appearance.
    static std::vector<std::wstring> quotedValues(const std::wstring& string);
};

}  // namespace sqlcore
