// SPDX-FileCopyrightText: 2026 coffeecode <212984587+coffeecode3d@users.noreply.github.com>
// SPDX-License-Identifier: BSD-3-Clause

#ifndef CUTEFISH_MESSAGES_WINDOW_H_
#define CUTEFISH_MESSAGES_WINDOW_H_

#include <cstdint>
#include <map>
#include <set>
#include <memory>
#include <vector>

#include "backend.h"
#include "base/timer.h"
#include "messages_client.h"
#include "notifier.h"
#include "ui/window.h"

namespace cutefish::ui {
class Label;
class ListView;
class TextField;
class Button;
} // namespace cutefish::ui

namespace cutefish::messages {

class SidebarRow;
class SignalBackend;
class MessageRow;

// Messages: conversations on the left, the chosen one and its composer on the
// right, and the phone's state along the bottom.
class MessagesWindow : public ui::Window
{
public:
    explicit MessagesWindow(base::EventLoop *loop, bool demo = false);
    ~MessagesWindow() override;

private:
    class RootView;

    void reloadThreads();
    void reloadFromPhone();
    void selectThread(int index);
    void send();
    void updateStatus();

    void openImage(int64_t messageId, const std::string &path);
    void openConversation(const std::string &key);
    void linkSignal();
    void applyTheme();
    Backend *backendFor(const std::string &service) const;
    void updateAttachment(int64_t messageId, std::string path);
    // Asks each service for its list now and then; not every service pushes.
    base::Timer m_sync;
    // What the conversation pane is showing, so an unchanged thread is not
    // rebuilt and scrolled again.
    std::string m_shownKey;
    size_t m_shownCount = 0;
    std::string m_shownLast;

    std::unique_ptr<MessagesClient> m_client;
    std::vector<std::unique_ptr<Backend>> m_backends;
    SignalBackend *m_signal = nullptr;
    std::vector<base::Subscription> m_backendSubscriptions;
    std::unique_ptr<Notifier> m_notifier;
    base::Subscription m_notifierSubscription;
    // The newest time already announced per thread, so a re-poll is quiet.
    std::map<std::string, std::string> m_notified;
    bool m_usePhone = false;
    std::vector<Thread> m_threads;
    int m_current = -1;
    bool m_demo = false;
    std::vector<base::Subscription> m_subscriptions;
    base::Subscription m_themeSubscription;

    RootView *m_root = nullptr;
};

} // namespace cutefish::messages

#endif // CUTEFISH_MESSAGES_WINDOW_H_
