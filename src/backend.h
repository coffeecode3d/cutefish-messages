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
    virtual bool send(const Thread &thread, const std::string &body, std::string &error) = 0;
    // The original of a previewed picture, once it lands.
    virtual void fetchAttachment(int64_t messageId) = 0;

    // The thread list, a thread's messages, or an attachment changed.
    base::Signal<> threadsChanged;
    base::Signal<> statusChanged;
    // A message's sharper attachment arrived: its message id and file.
    base::Signal<int64_t, std::string> attachmentReady;
};

} // namespace cutefish::messages

#endif // CUTEFISH_MESSAGES_BACKEND_H_
