// SPDX-FileCopyrightText: 2026 coffeecode <212984587+coffeecode3d@users.noreply.github.com>
// SPDX-License-Identifier: BSD-3-Clause

#ifndef CUTEFISH_MESSAGES_CLIENT_H_
#define CUTEFISH_MESSAGES_CLIENT_H_

#include <string>
#include <vector>

#include "base/dbus.h"
#include "base/signal.h"

namespace cutefish::messages {

struct Message
{
    std::string handle;
    std::string body;
    std::string timestamp;
    std::string sender;
    bool outgoing = false;
    bool read = true;
    // A decoded preview written under the cache, when the message has one.
    std::string attachmentPath;
    // The phone's message id, to match an arriving attachment to its bubble.
    int64_t id = 0;
};

struct Thread
{
    std::string key;
    // Which service this conversation came from, and its display name.
    std::string service;
    std::string serviceName;
    std::string name;
    std::string lastTimestamp;
    // The latest message's text, for the list.
    std::string snippet;
    bool group = false;
    bool replyReady = true;
    bool starred = false;
    int unread = 0;
    // The phone's thread id, for the SMS backend.
    int64_t threadId = 0;
    // Who a reply goes to.
    std::string address;
    std::vector<std::string> addresses;
    std::vector<Message> messages;
};

struct Status
{
    // BlueFerry is on the bus at all.
    bool running = false;
    bool connected = false;
    std::string phone;
    int battery = -1;
    // "encrypted", "plaintext" or "none", as the daemon reports it.
    std::string storage;
};

// The BlueFerry backend's Messages1 interface: threads with their messages,
// replies, read state, and the events that say when history changed.
class MessagesClient
{
public:
    explicit MessagesClient(base::EventLoop *loop);
    ~MessagesClient();

    MessagesClient(const MessagesClient &) = delete;
    MessagesClient &operator=(const MessagesClient &) = delete;

    Status status() const;
    std::vector<Thread> threads(unsigned limit = 200) const;
    // A direct thread's reply. Group replies need the roster confirmation the
    // panel shows, so they are not sent yet; error says why.
    bool reply(const Thread &thread, const std::string &body, std::string &error);
    void markRead(const Thread &thread);

    // History, status or a notification click asked for a refresh.
    base::Signal<> changed;

private:
    base::dbus::Proxy m_messages;
    base::dbus::Proxy m_events;
    std::vector<base::dbus::Match> m_matches;
};

} // namespace cutefish::messages

#endif // CUTEFISH_MESSAGES_CLIENT_H_
