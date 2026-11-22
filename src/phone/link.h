// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#ifndef CUTEFISH_MESSAGES_PHONE_LINK_H_
#define CUTEFISH_MESSAGES_PHONE_LINK_H_

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "base/event_loop.h"
#include "base/json.h"
#include "base/signal.h"
#include "base/timer.h"
#include "phone/certificate.h"
#include "phone/discovery.h"

typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_st SSL;

namespace cutefish::phone {

// A picture or sound that came with a message; the phone includes a small
// base64 thumbnail for pictures, enough to show a preview.
struct SmsAttachment
{
    int64_t partId = 0;
    std::string identifier;
    std::string mimeType;
    // The small preview the phone sends inline, written under the cache.
    std::string thumbnailPath;
};

// One SMS, as the phone reports it.
struct SmsMessage
{
    int64_t id = 0;
    int64_t threadId = 0;
    std::string address;
    std::string body;
    // Epoch milliseconds.
    int64_t date = 0;
    // Android's column: 1 is received, 2 is sent.
    int type = 0;
    bool read = true;
    std::vector<SmsAttachment> attachments;
    // The original file, once its payload has been fetched.
    std::string fullPath;
};

// The latest message in a thread, which is what the phone lists.
struct Conversation
{
    int64_t threadId = 0;
    std::string address;
    std::string snippet;
    int64_t date = 0;
    int type = 0;
    bool read = true;
    std::vector<SmsAttachment> attachments;
    // The original file, once its payload has been fetched.
    std::string fullPath;
};

// The phone's side of the KDE Connect protocol, written against the published
// packet formats: the phone finds us by broadcast, connects to TCP 1716, and
// the listening side is the TLS client. Nothing of KDE runs here.
class PhoneLink
{
public:
    explicit PhoneLink(base::EventLoop *loop);
    ~PhoneLink();

    PhoneLink(const PhoneLink &) = delete;
    PhoneLink &operator=(const PhoneLink &) = delete;

    // Listens, and broadcasts so the phone can find us.
    bool start(std::string &error);
    void stop();

    bool isConnected() const { return m_ssl != nullptr; }
    bool isPaired() const { return m_paired; }
    const std::string &phoneName() const { return m_peerName; }
    const std::string &deviceId() const;
    int battery() const { return m_battery; }

    void requestConversations();
    void requestThread(int64_t threadId);
    // Fetches the original of one attachment over the payload channel.
    void requestAttachment(int64_t threadId, int64_t messageId, size_t index,
                           bool priority = false);
    // A picture the user clicked: fetch its original ahead of the queue.
    void requestAttachmentNow(int64_t messageId);
    // Asks the phone for its address book, so threads show names.
    void requestContacts();
    // The contact's name for a number, or empty when it is not in the book.
    std::string nameFor(const std::string &address) const;
    // Sends through the phone's SIM. False when there is no link.
    bool sendSms(const std::string &address, const std::string &body, std::string &error);

    // Connection, pairing or battery changed.
    base::Signal<> stateChanged;
    base::Signal<std::vector<Conversation>> conversationsReceived;
    base::Signal<int64_t, std::vector<SmsMessage>> threadReceived;
    // One attachment's original arrived: its message id, and the file.
    base::Signal<int64_t, std::string> attachmentReady;
    // The address book changed.
    base::Signal<> contactsChanged;
    // Something worth showing on the status line.
    base::Signal<std::string> notice;

private:
    struct Packet
    {
        std::string type;
        std::string text;
    };

    void acceptConnection();
    bool handshake(int fd, std::string &error);
    void closeLink();
    void watchLink();
    bool sendPacket(const std::string &type, base::json::Value body);
    void sendRaw(const std::string &line);
    void flush();
    void readAvailable();
    void consumeLines();
    void handlePacket(const std::string &type, const base::json::Value &packet);
    void confirmPairing(int64_t stamp);
    void addContact(const std::string &vcard);
    bool downloadPayload(int port, int64_t size, const std::string &path);
    void pumpAttachments();
    void writePreviews(int64_t threadId);
    void buildIdentity();

    base::EventLoop *m_loop;
    Discovery m_discovery;
    PhoneCertificate m_certificate;

    int m_listener = -1;
    int m_listenerWatch = 0;
    int m_discoveryWatch = 0;

    int m_fd = -1;
    int m_watch = 0;
    int m_writeWatch = 0;
    SSL_CTX *m_ctx = nullptr;
    SSL *m_ssl = nullptr;

    std::string m_out;
    size_t m_sent = 0;
    std::string m_in;

    std::string m_peerId;
    std::string m_peerName;
    std::string m_peerPem;
    std::string m_peerAddress;
    std::string m_targetId;
    std::string m_identity;
    bool m_paired = false;
    int m_battery = -1;
    // The thread asked for, so its reply is not mistaken for a new summary.
    int64_t m_pendingThread = 0;
    std::map<int64_t, Conversation> m_conversations;
    // Normalized number to name, from the phone's vCards.
    std::map<std::string, std::string> m_contactNames;
    // Threads already fetched, so a newly pushed message joins its thread.
    std::map<int64_t, std::vector<SmsMessage>> m_threads;
    // One entry per attachment asked for, in the order asked.
    struct PendingAttachment
    {
        int64_t threadId = 0;
        int64_t messageId = 0;
        std::string identifier;
        std::string path;
    };
    std::vector<PendingAttachment> m_attachmentQueue;
    // One request in flight, so the phone is never asked for a pile at once.
    bool m_attachmentBusy = false;
    base::Timer m_attachmentTimeout;
};

} // namespace cutefish::phone

#endif // CUTEFISH_MESSAGES_PHONE_LINK_H_
