// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#ifndef CUTEFISH_MESSAGES_NOTIFIER_H_
#define CUTEFISH_MESSAGES_NOTIFIER_H_

#include <cstdint>
#include <map>
#include <string>

#include "base/dbus.h"
#include "base/event_loop.h"
#include "base/signal.h"

namespace cutefish::messages {

// Desktop notifications: a text that arrives while its conversation is not on
// screen becomes a notification, and clicking it says which thread to open.
class Notifier
{
public:
    explicit Notifier(base::EventLoop *loop);
    ~Notifier();

    Notifier(const Notifier &) = delete;
    Notifier &operator=(const Notifier &) = delete;

    // Returns the notification's id, or 0 when the shell did not take it.
    uint32_t notify(const std::string &summary, const std::string &body, int64_t threadId);
    void withdraw(uint32_t id);

    // A notification was clicked (or its default action taken).
    base::Signal<int64_t> activated;

private:
    base::dbus::Proxy m_proxy;
    base::dbus::Match m_actions;
    base::dbus::Match m_closed;
    std::map<uint32_t, int64_t> m_threads;
};

} // namespace cutefish::messages

#endif // CUTEFISH_MESSAGES_NOTIFIER_H_
