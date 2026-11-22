// SPDX-FileCopyrightText: 2026 coffeecode <212984587+coffeecode3d@users.noreply.github.com>
// SPDX-License-Identifier: BSD-3-Clause

#ifndef CUTEFISH_MESSAGES_PHONE_DISCOVERY_H_
#define CUTEFISH_MESSAGES_PHONE_DISCOVERY_H_

#include <mutex>
#include <string>
#include <vector>

#include "base/event_loop.h"
#include "base/signal.h"
#include "base/timer.h"

namespace cutefish::phone {

// A phone or computer found on the local network, speaking the KDE Connect
// protocol: what the identity packet says, and where it answered from.
struct Peer
{
    std::string id;
    std::string name;
    std::string type;
    std::string address;
    int port = 1716;
    int protocol = 8;
};

// The UDP side of the protocol: every device broadcasts an identity packet on
// 1716, and hears the others. No daemon, no other desktop's libraries.
class Discovery
{
public:
    explicit Discovery(base::EventLoop *loop);
    ~Discovery();

    Discovery(const Discovery &) = delete;
    Discovery &operator=(const Discovery &) = delete;

    bool start();
    void stop();
    // The socket to watch, and what to do when it is readable.
    int fd() const { return m_fd; }
    void handleRead();
    // Broadcasts our identity, and asks the others to answer in kind.
    void announce();

    std::vector<Peer> peers() const;
    // Our stable id, made once and kept.
    const std::string &deviceId() const { return m_id; }
    void setDeviceName(std::string name) { m_name = std::move(name); }

    base::Signal<const Peer &> found;
    base::Signal<> changed;

private:
    base::EventLoop *m_loop;
    base::Timer m_announce;
    std::string m_id;
    std::string m_name;
    int m_fd = -1;
    mutable std::mutex m_mutex;
    std::vector<Peer> m_peers;
};

} // namespace cutefish::phone

#endif // CUTEFISH_MESSAGES_PHONE_DISCOVERY_H_
