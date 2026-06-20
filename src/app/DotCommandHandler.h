// SPDX-License-Identifier: GPL-3.0-or-later
// DotCommandHandler — parses terminal "dot-commands" (.tables/.schema/.count/…)
// into SQL to run, a message to show, a clear, or a reconnect. Pure; port of
// DotCommandHandler.swift.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "models/DatabaseEngine.h"

namespace sqlterm {

enum class DotKind { Sql, MultiSql, Message, Clear, Reconnect };

struct DotCommandResult {
    DotKind kind;
    std::vector<std::wstring> statements;  // Sql (1) / MultiSql (n)
    std::wstring text;                      // Message text, or Reconnect db name
};

// Returns nullopt if `input` is not a dot-command (doesn't start with '.').
std::optional<DotCommandResult> handleDotCommand(const std::wstring& input,
                                                 DatabaseEngine engine);

}  // namespace sqlterm
