// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#ifndef CUTEFISH_MESSAGES_WINDOW_H_
#define CUTEFISH_MESSAGES_WINDOW_H_

#include <cstdint>
#include <map>
#include <set>
#include <memory>
#include <vector>

#include "messages_client.h"
#include "notifier.h"
#include "phone/link.h"
#include "ui/window.h"

namespace cutefish::ui {
class Label;
class ListView;
class TextField;
class Button;
} // namespace cutefish::ui

namespace cutefish::messages {

class SidebarRow;
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

    void phoneMessages(int64_t threadId, std::vector<phone::SmsMessage> messages);
    void phoneAttachment(int64_t messageId, std::string path);
    // Fetches the open thread again when a summary says it has news.
    void refreshOpenThread();
    void openImage(int64_t messageId, const std::string &path);
    void openConversation(int64_t threadId);
    void applyTheme();
    std::string nameOf(const std::string &address) const;
    std::string groupName(const std::vector<std::string> &addresses) const;
    void phoneConversations(std::vector<phone::Conversation> conversations);

    // The summaries arrive in a burst; one refresh follows it.
    base::Timer m_refresh;
    // Asks the phone for the conversation list now and then, since it does not
    // push one when a message is sent from the phone itself.
    base::Timer m_sync;
    std::vector<phone::Conversation> m_pendingConversations;
    // What the conversation pane is showing, so an unchanged thread is not
    // rebuilt and scrolled again.
    int64_t m_shownThread = 0;
    // A thread already asked for, so a burst of summaries asks once.
    int64_t m_requestedThread = 0;
    // Threads opened here, whose dots should stay cleared even while the
    // phone's own app has not marked them read.
    std::set<int64_t> m_readThreads;
    size_t m_shownCount = 0;
    std::string m_shownLast;

    std::unique_ptr<MessagesClient> m_client;
    std::unique_ptr<phone::PhoneLink> m_phone;
    std::unique_ptr<Notifier> m_notifier;
    base::Subscription m_notifierSubscription;
    // The newest date already announced per thread, so a re-poll is quiet.
    std::map<int64_t, int64_t> m_notified;
    bool m_usePhone = false;
    std::vector<phone::Conversation> m_conversations;
    std::map<int64_t, std::vector<phone::SmsMessage>> m_phoneThreads;
    std::vector<base::Subscription> m_phoneSubscriptions;
    std::vector<Thread> m_threads;
    int m_current = -1;
    bool m_demo = false;
    std::vector<base::Subscription> m_subscriptions;
    base::Subscription m_themeSubscription;

    RootView *m_root = nullptr;
};

} // namespace cutefish::messages

#endif // CUTEFISH_MESSAGES_WINDOW_H_
