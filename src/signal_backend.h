// SPDX-FileCopyrightText: 2026 coffeecode <212984587+coffeecode3d@users.noreply.github.com>
// SPDX-License-Identifier: BSD-3-Clause

#ifndef CUTEFISH_MESSAGES_SIGNAL_BACKEND_H_
#define CUTEFISH_MESSAGES_SIGNAL_BACKEND_H_

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "backend.h"
#include "signal_rpc.h"

namespace cutefish::messages {

// Signal, through a signal-cli daemon this process starts. Linking is the
// daemon's own startLink/finishLink, so no helper CLI is needed.
class SignalBackend : public Backend
{
public:
    using LinkUri = std::function<void(const std::string &uri)>;
    using LinkDone = std::function<void(bool ok, const std::string &error)>;

    explicit SignalBackend(base::EventLoop *loop);
    ~SignalBackend() override;

    ServiceAccount account() const override;
    bool ready() const override { return m_linked; }
    std::string statusLine() const override;
    bool start(std::string &error) override;
    void refresh() override;

    std::vector<Thread> threads() const override;
    void loadThread(const Thread &thread) override { (void)thread; }
    void markRead(const Thread &thread) override;
    bool send(const Thread &thread, const std::string &body, std::string &error) override;
    void fetchAttachment(int64_t messageId) override { (void)messageId; }

    // Begins linking this app as a device; the URI is meant for a QR code.
    void link(LinkUri onUri, LinkDone done);

private:
    bool ensureDaemon(std::string &error);
    void beginReceive();
    void handleNotification(const base::json::Value &params);
    void handleEnvelope(const base::json::Value &envelope);
    Thread &threadFor(const std::string &key, const std::string &name, bool group);
    bool hasMessage(const Thread &thread, int64_t id) const;
    void ensureThreadsSorted();

    base::EventLoop *m_loop;
    SignalRpc m_rpc;
    std::string m_binary;
    std::string m_dataDir;
    std::string m_socket;
    std::string m_account;
    bool m_linked = false;
    bool m_receiving = false;
    std::map<std::string, Thread> m_threads;
    std::vector<base::Subscription> m_subscriptions;
};

} // namespace cutefish::messages

#endif // CUTEFISH_MESSAGES_SIGNAL_BACKEND_H_
