// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/PostgresHba.h"

namespace sqlcore {

std::optional<std::wstring> PostgresHba::suggestedLine(const std::wstring& message) {
    if (message.find(L"no pg_hba.conf entry") == std::wstring::npos) return std::nullopt;

    const std::vector<std::wstring> quoted = quotedValues(message);
    if (quoted.size() < 3) return std::nullopt;

    const std::wstring& rawAddress = quoted[0];
    const std::wstring& user = quoted[1];
    const std::wstring& database = quoted[2];

    // Strip any IPv6 zone index (e.g. "%en0") — first '%'-delimited component,
    // skipping empty components (matches Swift's split(separator:"%").first).
    std::wstring address;
    {
        std::vector<std::wstring> comps;
        std::wstring cur;
        for (const wchar_t ch : rawAddress) {
            if (ch == L'%') {
                if (!cur.empty()) {
                    comps.push_back(cur);
                    cur.clear();
                }
            } else {
                cur.push_back(ch);
            }
        }
        if (!cur.empty()) comps.push_back(cur);
        address = comps.empty() ? rawAddress : comps.front();
    }

    // This client always connects over TCP, so we expect an IP literal; if it
    // isn't one, don't risk suggesting a malformed rule.
    const bool hasDot = address.find(L'.') != std::wstring::npos;
    const bool hasColon = address.find(L':') != std::wstring::npos;
    if (!hasDot && !hasColon) return std::nullopt;

    // Single-host CIDR: /32 for IPv4, /128 for IPv6.
    const std::wstring cidr = hasColon ? address + L"/128" : address + L"/32";

    return L"host    " + database + L"    " + user + L"    " + cidr +
           L"    scram-sha-256";
}

std::vector<std::wstring> PostgresHba::quotedValues(const std::wstring& string) {
    std::vector<std::wstring> values;
    std::wstring current;
    bool inQuote = false;
    for (const wchar_t ch : string) {
        if (ch == L'"') {
            if (inQuote) {
                values.push_back(current);
                current.clear();
            }
            inQuote = !inQuote;
        } else if (inQuote) {
            current.push_back(ch);
        }
    }
    return values;
}

}  // namespace sqlcore
