// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "notifier.h"

#include <cstdio>
#include <vector>

namespace cutefish::messages {

namespace {

constexpr const char *kService = "org.freedesktop.Notifications";
constexpr const char *kPath = "/org/freedesktop/Notifications";
constexpr const char *kInterface = "org.freedesktop.Notifications";

} // namespace

Notifier::Notifier(base::EventLoop *loop)
    : m_proxy(base::dbus::Connection::session(loop), kService, kPath, kInterface)
{
    m_actions = m_proxy.onSignal("ActionInvoked", [this](const base::dbus::Message &message) {
        uint32_t id = 0;
        std::string action;
        if (!message.read(id, action))
            return;
        const auto found = m_threads.find(id);
        if (found == m_threads.end())
            return;
        const int64_t thread = found->second;
        m_threads.erase(found);
        activated(thread);
    });
    m_closed = m_proxy.onSignal("NotificationClosed", [this](const base::dbus::Message &message) {
        uint32_t id = 0;
        uint32_t reason = 0;
        if (message.read(id, reason))
            m_threads.erase(id);
    });
}

Notifier::~Notifier() = default;

uint32_t Notifier::notify(const std::string &summary, const std::string &body, int64_t threadId)
{
    const auto reply = m_proxy.call("Notify", std::string("Messages"), uint32_t(0),
                                    std::string("org.cutefish.Messages"), summary, body,
                                    std::vector<std::string> {"default", "Open"},
                                    base::dbus::VariantMap {}, int32_t(6000));
    if (reply.isError())
        std::fprintf(stderr, "notify failed: %s %s\n", reply.error().name.c_str(),
                     reply.error().message.c_str());
    const auto id = reply.value<uint32_t>(0);
    if (!id || *id == 0)
        return 0;
    m_threads[*id] = threadId;
    return *id;
}

void Notifier::withdraw(uint32_t id)
{
    if (id == 0)
        return;
    m_proxy.send("CloseNotification", id);
    m_threads.erase(id);
}

} // namespace cutefish::messages
