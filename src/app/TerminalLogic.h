// SPDX-License-Identifier: GPL-3.0-or-later
// Pure terminal-controller logic ported from TerminalViewModel.swift: smart-quote
// normalization, the read-only/destructive guard, transaction-state tracking,
// command-history navigation, and schema-browsing SQL.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "models/DatabaseEngine.h"

namespace sqlterm {

// Trim and normalize the macOS "smart" quotes/dashes that would break SQL.
std::wstring normalizeSmartCharacters(const std::wstring& text);

// Read-only block + destructive-confirmation decision over a batch of statements.
enum class GuardAction { Run, Block, Confirm };
struct GuardDecision {
    GuardAction action;
    std::wstring message;  // for Block/Confirm
};
GuardDecision evaluateGuard(const std::vector<std::wstring>& statements, bool readOnly);

// New in-transaction state after running `statements` (last BEGIN/COMMIT/... wins).
bool updateInTransaction(bool current, const std::vector<std::wstring>& statements);

// Schema-browsing SQL (Postgres public schema; SQLite excludes sqlite_* tables).
std::wstring tableNamesSql(DatabaseEngine engine);
std::wstring columnsSql(DatabaseEngine engine, const std::wstring& table);
std::wstring quotedIdentifier(const std::wstring& name);
std::wstring selectStatementFor(const std::wstring& table);  // SELECT * FROM <q> LIMIT 100;

// ⌘↑/⌘↓ command-history navigation state machine.
class CommandHistory {
public:
    // Append an executed command (skips a consecutive duplicate); resets the cursor.
    void add(const std::wstring& input);
    // Move up; returns the text to show, or nullopt if there's nothing.
    std::optional<std::wstring> up(const std::wstring& currentInput);
    // Move down; returns the text to show (the saved input at the bottom), or nullopt.
    std::optional<std::wstring> down();

private:
    std::vector<std::wstring> entries_;
    int index_ = -1;
    std::wstring saved_;
};

}  // namespace sqlterm
