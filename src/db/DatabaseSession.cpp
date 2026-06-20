// SPDX-License-Identifier: GPL-3.0-or-later
#include "db/DatabaseSession.h"

#include "db/DatabaseProviderFactory.h"

namespace sqlterm {

DatabaseSession::DatabaseSession() {
    worker_ = std::thread(&DatabaseSession::workerLoop, this);
}

DatabaseSession::~DatabaseSession() {
    // Interrupt any in-flight query so the worker returns promptly, then stop it.
    cancel();
    {
        std::lock_guard<std::mutex> lk(queueMutex_);
        stop_ = true;
    }
    queueCv_.notify_all();
    if (worker_.joinable()) worker_.join();

    // Worker has stopped; safe to tear the provider down directly.
    std::lock_guard<std::mutex> lk(connMutex_);
    if (provider_) {
        provider_->disconnect();
        provider_.reset();
    }
}

void DatabaseSession::post(std::function<void()> job) {
    {
        std::lock_guard<std::mutex> lk(queueMutex_);
        jobs_.push(std::move(job));
    }
    queueCv_.notify_one();
}

void DatabaseSession::workerLoop() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lk(queueMutex_);
            queueCv_.wait(lk, [this] { return stop_ || !jobs_.empty(); });
            if (stop_) return;  // discard any pending jobs on shutdown
            job = std::move(jobs_.front());
            jobs_.pop();
        }
        job();
    }
}

void DatabaseSession::connectAsync(const DatabaseConnection& config, ConnectCallback done) {
    post([this, config, done = std::move(done)]() {
        // Tear down any previous provider first.
        {
            std::lock_guard<std::mutex> lk(connMutex_);
            if (provider_) {
                provider_->disconnect();
                provider_.reset();
            }
        }

        std::unique_ptr<DatabaseProvider> next = makeProvider(config.engine);
        if (!next) {
            if (done) done(false, L"This engine is not supported yet.");
            return;
        }

        // connect() is blocking — run it WITHOUT holding connMutex_ (the new
        // provider isn't published yet, so cancel() can't see it).
        std::wstring error;
        const bool ok = next->connect(config, error);
        if (ok) {
            std::lock_guard<std::mutex> lk(connMutex_);
            provider_ = std::move(next);
        }
        if (done) done(ok, error);
    });
}

void DatabaseSession::executeAsync(const std::wstring& sql, ResultCallback done) {
    post([this, sql, done = std::move(done)]() {
        DatabaseProvider* p = nullptr;
        {
            std::lock_guard<std::mutex> lk(connMutex_);
            p = provider_.get();
        }
        // execute() runs WITHOUT connMutex_, so cancel() can interrupt it.
        QueryResult result =
            p ? p->execute(sql) : QueryResult::failure(L"No database connection.");
        if (done) done(std::move(result));
    });
}

void DatabaseSession::executeUncancellableAsync(const std::wstring& sql, ResultCallback done) {
    // Same body as executeAsync; the distinction is purely that callers won't fire
    // cancel() against these (see header). Cancellation safety is identical.
    executeAsync(sql, std::move(done));
}

void DatabaseSession::disconnectAsync(std::function<void()> done) {
    post([this, done = std::move(done)]() {
        {
            std::lock_guard<std::mutex> lk(connMutex_);
            if (provider_) {
                provider_->disconnect();
                provider_.reset();
            }
        }
        if (done) done();
    });
}

void DatabaseSession::cancel() {
    std::lock_guard<std::mutex> lk(connMutex_);
    if (provider_) provider_->cancel();
}

bool DatabaseSession::isConnected() {
    std::lock_guard<std::mutex> lk(connMutex_);
    return provider_ && provider_->isConnected();
}

bool DatabaseSession::isSSLActive() {
    std::lock_guard<std::mutex> lk(connMutex_);
    return provider_ && provider_->isSSLActive();
}

std::wstring DatabaseSession::statusMessage() {
    std::lock_guard<std::mutex> lk(connMutex_);
    return provider_ ? provider_->statusMessage() : std::wstring(L"Disconnected");
}

}  // namespace sqlterm
