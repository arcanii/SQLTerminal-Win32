// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/TerminalLogic.h"

#include <algorithm>

#include "core/SqlStatementClassifier.h"
#include "core/SqlStatementSplitter.h"

namespace sqlterm {

using sqlcore::SqlStatementClassifier;
using sqlcore::SqlStatementInfo;
using sqlcore::SqlStatementKind;
using sqlcore::SqlStatementSplitter;

namespace {

std::wstring trimWs(const std::wstring& s) {
    auto ws = [](wchar_t c) {
        return c == L' ' || c == L'\t' || c == L'\n' || c == L'\r' || c == L'\v' || c == L'\f';
    };
    size_t b = 0, e = s.size();
    while (b < e && ws(s[b])) ++b;
    while (e > b && ws(s[e - 1])) --e;
    return s.substr(b, e - b);
}

std::wstring replaceAll(std::wstring s, wchar_t from, wchar_t to) {
    for (auto& c : s)
        if (c == from) c = to;
    return s;
}

std::wstring escapeSingleQuotes(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : s) {
        out.push_back(c);
        if (c == L'\'') out.push_back(L'\'');
    }
    return out;
}

std::wstring destructiveWarning(const std::vector<SqlStatementInfo>& infos) {
    std::vector<std::wstring> labels;
    for (const auto& i : infos) {
        const std::wstring label = (i.leadingKeyword == L"DROP" || i.leadingKeyword == L"TRUNCATE")
                                       ? i.leadingKeyword
                                       : (i.leadingKeyword + L" without WHERE");
        if (std::find(labels.begin(), labels.end(), label) == labels.end()) labels.push_back(label);
    }
    std::wstring joined;
    for (size_t k = 0; k < labels.size(); ++k) {
        if (k) joined += L", ";
        joined += labels[k];
    }
    return L"This runs a destructive statement that can't be undone: " + joined + L". Run it anyway?";
}

}  // namespace

std::wstring normalizeSmartCharacters(const std::wstring& text) {
    std::wstring t = trimWs(text);
    for (auto& c : t) {
        switch (c) {
            case 0x201C:  // left double quote
            case 0x201D:  // right double quote
                c = L'"';
                break;
            case 0x2018:  // left single quote
            case 0x2019:  // right single quote
                c = L'\'';
                break;
            case 0x2013:  // en dash
            case 0x2014:  // em dash
                c = L'-';
                break;
            default:
                break;
        }
    }
    return t;
}

GuardDecision evaluateGuard(const std::vector<std::wstring>& statements, bool readOnly) {
    std::vector<SqlStatementInfo> infos;
    for (const auto& s : statements)
        for (const auto& info : SqlStatementClassifier::classifyAll(s)) infos.push_back(info);

    if (readOnly) {
        for (const auto& i : infos) {
            if (i.kind == SqlStatementKind::Write) {
                return {GuardAction::Block,
                        L"Read-only mode is on — " + i.leadingKeyword +
                            L" is blocked. Toggle it off in the toolbar to run writes."};
            }
        }
    }

    std::vector<SqlStatementInfo> destructive;
    for (const auto& i : infos)
        if (i.isDestructive) destructive.push_back(i);
    if (!destructive.empty()) return {GuardAction::Confirm, destructiveWarning(destructive)};

    return {GuardAction::Run, L""};
}

bool updateInTransaction(bool current, const std::vector<std::wstring>& statements) {
    for (const auto& s : statements) {
        for (const auto& stmt : SqlStatementSplitter::split(s)) {
            const std::wstring kw = SqlStatementClassifier::classify(stmt).leadingKeyword;
            if (kw == L"BEGIN" || kw == L"START") {
                current = true;
            } else if (kw == L"COMMIT" || kw == L"ROLLBACK" || kw == L"END" || kw == L"ABORT") {
                current = false;
            }
        }
    }
    return current;
}

std::wstring tableNamesSql(DatabaseEngine engine) {
    if (engine == DatabaseEngine::Postgres)
        return L"SELECT tablename FROM pg_tables WHERE schemaname = 'public' ORDER BY tablename;";
    return L"SELECT name FROM sqlite_master WHERE type = 'table' AND name NOT LIKE 'sqlite_%' "
           L"ORDER BY name;";
}

std::wstring columnsSql(DatabaseEngine engine, const std::wstring& table) {
    const std::wstring literal = escapeSingleQuotes(table);
    if (engine == DatabaseEngine::Postgres)
        return L"SELECT column_name, data_type FROM information_schema.columns WHERE table_schema = "
               L"'public' AND table_name = '" + literal + L"' ORDER BY ordinal_position;";
    return L"PRAGMA table_info('" + literal + L"');";
}

std::wstring quotedIdentifier(const std::wstring& name) {
    std::wstring inner;
    for (wchar_t c : name) {
        inner.push_back(c);
        if (c == L'"') inner.push_back(L'"');
    }
    return L"\"" + inner + L"\"";
}

std::wstring selectStatementFor(const std::wstring& table) {
    return L"SELECT * FROM " + quotedIdentifier(table) + L" LIMIT 100;";
}

void CommandHistory::add(const std::wstring& input) {
    if (entries_.empty() || entries_.back() != input) entries_.push_back(input);
    index_ = -1;
    saved_.clear();
}

std::optional<std::wstring> CommandHistory::up(const std::wstring& currentInput) {
    if (entries_.empty()) return std::nullopt;
    if (index_ == -1) {
        saved_ = currentInput;
        index_ = static_cast<int>(entries_.size()) - 1;
    } else if (index_ > 0) {
        index_ -= 1;
    }
    return entries_[static_cast<size_t>(index_)];
}

std::optional<std::wstring> CommandHistory::down() {
    if (index_ == -1) return std::nullopt;
    if (index_ < static_cast<int>(entries_.size()) - 1) {
        index_ += 1;
        return entries_[static_cast<size_t>(index_)];
    }
    index_ = -1;
    return saved_;
}

}  // namespace sqlterm
