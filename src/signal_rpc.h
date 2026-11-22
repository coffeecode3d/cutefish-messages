// SPDX-FileCopyrightText: 2026 coffeecode <212984587+coffeecode3d@users.noreply.github.com>
// SPDX-License-Identifier: BSD-3-Clause

#ifndef CUTEFISH_MESSAGES_SIGNAL_RPC_H_
#define CUTEFISH_MESSAGES_SIGNAL_RPC_H_

#include <cstdint>
#include <map>
#include <string>

#include "base/event_loop.h"
#include "base/json.h"
#include "base/signal.h"

namespace cutefish::messages {

// Talks JSON-RPC 2.0, one object per line, to a signal-cli daemon on a Unix
// socket that this starts for the app. The daemon is multi-account, which is
// what exposes startLink and finishLink.
class SignalRpc
{
public:
    using Reply = std::function<void(bool ok, const base::json::Value &result,
                                    const std::string &error)>;

    explicit SignalRpc(base::EventLoop *loop);
    ~SignalRpc();

    // Spawns `binary --data-dir <dataDir> daemon --socket=<socket>`, waits for
    // the socket, and connects.
    bool start(const std::string &binary, const std::string &dataDir,
               const std::string &socketPath, std::string &error);
    void stop();
    bool isRunning() const { return m_fd >= 0; }

    void call(const std::string &method, base::json::Value params, Reply done);
    void call(const std::string &method, Reply done)
    {
        call(method, base::json::Value::object(), std::move(done));
    }

    // A notification's params, e.g. the envelope of `receive`.
    base::Signal<const base::json::Value &> notification;

private:
    void onReadable();
    void consume();
    void failAll(const std::string &error);

    base::EventLoop *m_loop;
    int m_fd = -1;
    int m_watch = 0;
    int64_t m_process = 0;
    std::string m_buffer;
    int64_t m_nextId = 1;
    std::map<int64_t, Reply> m_pending;
};

} // namespace cutefish::messages

#endif // CUTEFISH_MESSAGES_SIGNAL_RPC_H_
