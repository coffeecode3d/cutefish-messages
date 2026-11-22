// SPDX-FileCopyrightText: 2026 coffeecode <212984587+coffeecode3d@users.noreply.github.com>
// SPDX-License-Identifier: BSD-3-Clause

#ifndef CUTEFISH_MESSAGES_BACKEND_H_
#define CUTEFISH_MESSAGES_BACKEND_H_

#include <cstdint>
#include <string>
#include <vector>

#include "base/signal.h"
#include "messages_client.h"

namespace cutefish::messages {

// A service Messages can show: the phone's SMS, Signal, and so on.
struct ServiceAccount
{
    std::string id;
    std::string name;
    std::string detail;
    // Implemented yet. A known service that is not is offered as "coming".
    bool supported = true;
};

// One source of conversations behind the window. The window merges what each
// backend gives it and asks the right one to send.
class Backend
{
public:
    virtual ~Backend() = default;

    virtual ServiceAccount account() const = 0;
    virtual bool ready() const = 0;
    // The line under the window, such as the phone and its battery.
    virtual std::string statusLine() const = 0;
    virtual bool start(std::string &error) = 0;
    virtual void refresh() = 0;

    // Newest first; keys are namespaced by the backend.
    virtual std::vector<Thread> threads() const = 0;
    // Fill the thread's messages, if they are not held yet.
    virtual void loadThread(const Thread &thread) = 0;
    virtual void markRead(const Thread &thread) = 0;
    // Read state the services let us keep locally.
    virtual void setUnread(const Thread &thread, bool unread)
    {
        (void)thread;
        (void)unread;
    }
    // Whether a conversation can be removed from this app.
    virtual bool canDelete() const { return false; }
    virtual bool removeThread(const Thread &thread)
    {
        (void)thread;
        return false;
    }
    virtual bool send(const Thread &thread, const std::string &body, std::string &error) = 0;
    // Sends a file, with the composer text as its caption when it has one.
    virtual bool sendFile(const Thread &thread, const std::string &path,
                          const std::string &caption, std::string &error)
    {
        (void)thread;
        (void)path;
        (void)caption;
        error = "This service cannot send attachments yet.";
        return false;
    }
    // Tells the service the user is composing, where it supports that.
    virtual void sendTyping(const Thread &thread, bool typing)
    {
        (void)thread;
        (void)typing;
    }
    // The original of a previewed picture, once it lands.
    virtual void fetchAttachment(int64_t messageId) = 0;

    // The thread list, a thread's messages, or an attachment changed.
    base::Signal<> threadsChanged;
    base::Signal<> statusChanged;
    // A message's sharper attachment arrived: its message id and file.
    base::Signal<int64_t, std::string> attachmentReady;
    // The other side started or stopped typing in a thread.
    base::Signal<std::string, bool> typingChanged;
};

} // namespace cutefish::messages

#endif // CUTEFISH_MESSAGES_BACKEND_H_
