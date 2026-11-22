// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "messages_client.h"

#include <algorithm>

#include "base/json.h"
#include "base/i18n.h"

namespace cutefish::messages {

namespace {

constexpr const char *kService = "io.weirdware.BlueFerry";
constexpr const char *kPath = "/io/weirdware/BlueFerry";

} // namespace

MessagesClient::MessagesClient(base::EventLoop *loop)
    : m_messages(base::dbus::Connection::session(loop), kService, kPath,
                 "io.weirdware.BlueFerry.Messages1")
    , m_events(base::dbus::Connection::session(loop), kService, kPath,
               "io.weirdware.BlueFerry.Events1")
{
    const auto refresh = [this](const base::dbus::Message &) { changed(); };
    m_matches.push_back(m_events.onSignal("HistoryChanged", refresh));
    m_matches.push_back(m_events.onSignal("StatusChanged", refresh));
    m_matches.push_back(m_events.onSignal("OpenMessageRequested", refresh));
}

MessagesClient::~MessagesClient() = default;

Status MessagesClient::status() const
{
    Status status;
    const auto text = m_messages.call("GetStatus").value<std::string>();
    if (!text)
        return status;
    status.running = true;
    const auto parsed = base::json::Value::parse(*text);
    if (!parsed || !parsed->isObject())
        return status;
    status.connected = (*parsed)["connected"].toBool();
    status.phone = (*parsed)["phone_name"].toString();
    if (status.phone.empty())
        status.phone = (*parsed)["phone_model"].toString();
    status.battery = int((*parsed)["phone_battery"].toInt(-1));
    status.storage = (*parsed)["storage"].toString();
    return status;
}

std::vector<Thread> MessagesClient::threads(unsigned limit) const
{
    std::vector<Thread> result;
    const auto text = m_messages.call("ListThreads", uint32_t(limit)).value<std::string>();
    if (!text)
        return result;
    const auto parsed = base::json::Value::parse(*text);
    if (!parsed)
        return result;

    const base::json::Value::Array *array = nullptr;
    if (parsed->isArray()) {
        array = &parsed->toArray();
    } else if (parsed->isObject() && (*parsed)["threads"].isArray()) {
        array = &(*parsed)["threads"].toArray();
    }
    if (!array)
        return result;

    for (const base::json::Value &value : *array) {
        if (!value.isObject())
            continue;
        Thread thread;
        thread.key = value["key"].toString();
        thread.name = value["name"].toString();
        thread.group = value["is_group"].toBool();
        thread.replyReady = value["reply_ready"].toBool(true);
        thread.starred = value["starred"].toBool();
        thread.lastTimestamp = value["last_ts"].toString();
        if (thread.name.empty())
            thread.name = "Unknown";

        const base::json::Value &messages = value["messages"];
        if (messages.isArray()) {
            for (const base::json::Value &item : messages.toArray()) {
                Message message;
                message.handle = item["handle"].toString();
                message.body = item["body"].toString();
                message.timestamp = item["timestamp"].toString();
                message.sender = item["sender"].toString();
                message.outgoing = item["outgoing"].toBool();
                message.read = item["read"].toBool(true);
                if (!message.outgoing && !message.read)
                    ++thread.unread;
                thread.messages.push_back(std::move(message));
            }
        }
        if (!thread.messages.empty())
            thread.snippet = thread.messages.back().body;
        if (!thread.key.empty())
            result.push_back(std::move(thread));
    }
    // The daemon orders by last message; keep that.
    return result;
}

bool MessagesClient::reply(const Thread &thread, const std::string &body, std::string &error)
{
    if (body.empty())
        return false;
    if (thread.group || !thread.replyReady) {
        error = i18n::tr("Group replies are not in this version yet.");
        return false;
    }
    const auto transfer = m_messages.call("SendToThread", thread.key, body, false).value<std::string>();
    if (!transfer) {
        error = i18n::tr("The message could not be sent.");
        return false;
    }
    return true;
}

void MessagesClient::markRead(const Thread &thread)
{
    if (thread.unread > 0)
        m_messages.send("MarkThreadRead", thread.key);
}

} // namespace cutefish::messages
