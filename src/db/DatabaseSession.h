// SPDX-License-Identifier: GPL-3.0-or-later
// DatabaseSession — hosts a DatabaseProvider on a dedicated worker thread so the
// UI thread is never blocked by disk/network I/O, and the provider is only ever
// touched by one thread at a time. Port of DatabaseSession.swift (DispatchQueue
// + NSLock + continuation -> std::thread + job queue + mutex + completion
// callbacks).
//
// Threading contract (mirrors the Swift original — see DatabaseSession.swift):
//   * connect/execute/disconnect run as jobs on the single worker thread, so they
//     are serialized and never overlap.
//   * `connMutex_` guards ONLY the provider pointer. It is held just long enough
//     to read or swap the pointer, NEVER across a blocking execute/connect.
//   * cancel() runs on the caller's (UI) thread. It takes connMutex_ and calls
//     the provider's thread-safe interrupt (sqlite3_interrupt / PQcancel), which
//     is safe to invoke while the worker is mid-query. Because disconnect also
//     takes connMutex_ before freeing the provider, cancel can never touch a
//     freed handle.
//
// Completion callbacks run ON THE WORKER THREAD; the caller is responsible for
// marshalling back to its UI thread (e.g. PostMessage).
#pragma once

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

#include "db/DatabaseProvider.h"
#include "models/DatabaseConnection.h"
#include "models/QueryResult.h"

namespace sqlterm {

class DatabaseSession {
public:
    using ConnectCallback = std::function<void(bool success, std::wstring error)>;
    using ResultCallback = std::function<void(QueryResult)>;

    DatabaseSession();
    ~DatabaseSession();

    DatabaseSession(const DatabaseSession&) = delete;
    DatabaseSession& operator=(const DatabaseSession&) = delete;

    // Open a connection for `config`, replacing any existing one. `done` fires on
    // the worker thread with success/error.
    void connectAsync(const DatabaseConnection& config, ConnectCallback done);

    // Execute `sql`; `done` fires on the worker thread with the result. Cancellable
    // via cancel() (interrupts the in-flight query).
    void executeAsync(const std::wstring& sql, ResultCallback done);

    // Like executeAsync but not affected by cancel() (used for background schema
    // queries in P6 so a user Ctrl+. doesn't kill a metadata fetch).
    void executeUncancellableAsync(const std::wstring& sql, ResultCallback done);

    void disconnectAsync(std::function<void()> done = {});

    // Interrupt an in-flight query. Safe to call from any thread; no-op when idle.
    void cancel();

    bool isConnected();
    bool isSSLActive();
    std::wstring statusMessage();

private:
    void workerLoop();
    void post(std::function<void()> job);

    std::thread worker_;
    std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::queue<std::function<void()>> jobs_;
    bool stop_ = false;

    std::mutex connMutex_;  // guards provider_ (read by cancel() off-thread)
    std::unique_ptr<DatabaseProvider> provider_;
};

}  // namespace sqlterm
