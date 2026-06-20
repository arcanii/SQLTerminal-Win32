// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/ResultFormat.h"

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <numeric>

#include <nlohmann/json.hpp>

#include "platform/Encoding.h"

namespace sqlterm {

namespace {

// Strict whole-string double parse (matches Swift's Double(String) — no leading
// or trailing junk/whitespace).
bool parseDouble(const std::wstring& s, double& out) {
    if (s.empty() || iswspace(s[0])) return false;
    wchar_t* end = nullptr;
    const double v = std::wcstod(s.c_str(), &end);
    if (end != s.c_str() + s.size()) return false;
    out = v;
    return true;
}

// Locale-independent natural compare: digit runs compared by numeric value,
// other characters case-insensitively. Approximates localizedStandardCompare.
int naturalCompare(const std::wstring& a, const std::wstring& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (iswdigit(a[i]) && iswdigit(b[j])) {
            size_t si = i, sj = j;
            while (i < a.size() && iswdigit(a[i])) ++i;
            while (j < b.size() && iswdigit(b[j])) ++j;
            // Skip leading zeros (keep at least one digit).
            while (si < i - 1 && a[si] == L'0') ++si;
            while (sj < j - 1 && b[sj] == L'0') ++sj;
            const size_t la = i - si, lb = j - sj;
            if (la != lb) return la < lb ? -1 : 1;
            for (size_t k = 0; k < la; ++k)
                if (a[si + k] != b[sj + k]) return a[si + k] < b[sj + k] ? -1 : 1;
        } else {
            const wchar_t ca = towlower(a[i]), cb = towlower(b[j]);
            if (ca != cb) return ca < cb ? -1 : 1;
            ++i;
            ++j;
        }
    }
    if (i < a.size()) return 1;
    if (j < b.size()) return -1;
    return 0;
}

}  // namespace

int smartCompare(const std::wstring& a, const std::wstring& b) {
    if (a == b) return 0;
    if (a == L"NULL") return 1;   // NULLs after everything
    if (b == L"NULL") return -1;
    double da = 0, db = 0;
    if (parseDouble(a, da) && parseDouble(b, db)) {
        if (da == db) return 0;
        return da < db ? -1 : 1;
    }
    return naturalCompare(a, b);
}

std::vector<size_t> sortedRowOrder(const std::vector<std::vector<std::wstring>>& rows,
                                   size_t column, bool ascending) {
    std::vector<size_t> order(rows.size());
    std::iota(order.begin(), order.end(), size_t{0});
    auto cell = [&](size_t r) -> const std::wstring& {
        static const std::wstring empty;
        return column < rows[r].size() ? rows[r][column] : empty;
    };
    std::stable_sort(order.begin(), order.end(), [&](size_t x, size_t y) {
        const int cmp = smartCompare(cell(x), cell(y));
        if (cmp == 0) return false;  // stable: keep original order
        return ascending ? (cmp < 0) : (cmp > 0);
    });
    return order;
}

std::wstring csvEscape(const std::wstring& value) {
    if (value.find(L',') != std::wstring::npos || value.find(L'"') != std::wstring::npos ||
        value.find(L'\n') != std::wstring::npos) {
        std::wstring out = L"\"";
        for (wchar_t c : value) {
            out.push_back(c);
            if (c == L'"') out.push_back(L'"');
        }
        out.push_back(L'"');
        return out;
    }
    return value;
}

std::wstring buildTsv(const std::vector<std::wstring>& columns,
                      const std::vector<std::vector<std::wstring>>& rows) {
    std::wstring out;
    auto joinTab = [](const std::vector<std::wstring>& v) {
        std::wstring s;
        for (size_t i = 0; i < v.size(); ++i) {
            if (i) s += L'\t';
            s += v[i];
        }
        return s;
    };
    out += joinTab(columns);
    for (const auto& row : rows) {
        out += L'\n';
        out += joinTab(row);
    }
    return out;
}

std::wstring buildCsv(const std::vector<std::wstring>& columns,
                      const std::vector<std::vector<std::wstring>>& rows) {
    std::wstring out;
    auto joinCsv = [](const std::vector<std::wstring>& v) {
        std::wstring s;
        for (size_t i = 0; i < v.size(); ++i) {
            if (i) s += L',';
            s += csvEscape(v[i]);
        }
        return s;
    };
    out += joinCsv(columns);
    for (const auto& row : rows) {
        out += L'\n';
        out += joinCsv(row);
    }
    return out;
}

std::optional<std::wstring> prettyPrintJson(const std::wstring& value) {
    try {
        const nlohmann::json obj = nlohmann::json::parse(utf8FromWide(value));
        return wideFromUtf8(obj.dump(2));  // nlohmann objects keep keys sorted
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace sqlterm
