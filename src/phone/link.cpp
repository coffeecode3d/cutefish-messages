// SPDX-FileCopyrightText: 2026 coffeecode <212984587+coffeecode3d@users.noreply.github.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "phone/link.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <array>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace cutefish::phone {

namespace {

constexpr int kPort = 1716;
constexpr int kHandshakeTimeout = 5000;

int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
}

std::string hostName()
{
    std::array<char, 256> name {};
    if (::gethostname(name.data(), name.size() - 1) != 0)
        return "desktop";
    return name.data();
}

std::string pemOf(X509 *certificate)
{
    BIO *bio = BIO_new(BIO_s_mem());
    if (!bio)
        return {};
    PEM_write_bio_X509(bio, certificate);
    char *data = nullptr;
    const long size = BIO_get_mem_data(bio, &data);
    std::string text(data ? std::string(data, size_t(size)) : std::string());
    BIO_free(bio);
    return text;
}

int acceptAny(int, X509_STORE_CTX *) { return 1; }

std::string digitsOf(const std::string &text)
{
    std::string digits;
    for (const char c : text) {
        if (c >= '0' && c <= '9')
            digits.push_back(c);
    }
    return digits;
}

// Numbers are written every way imaginable; compare them by their digits,
// with the country code that both sides prefix dropped.
std::string numberKey(const std::string &text)
{
    std::string digits = digitsOf(text);
    if (digits.size() > 10 && digits[0] == '1')
        digits.erase(0, 1);
    return digits;
}

std::string attachmentDirectory()
{
    std::string base;
    if (const char *cache = std::getenv("XDG_CACHE_HOME"))
        base = cache;
    else if (const char *home = std::getenv("HOME"))
        base = std::string(home) + "/.cache";
    else
        base = "/tmp";
    base += "/cutefish/messages/attachments";
    std::error_code error;
    std::filesystem::create_directories(base, error);
    return base;
}

std::string extensionFor(const std::string &mimeType)
{
    if (mimeType.find("jpeg") != std::string::npos || mimeType.find("jpg") != std::string::npos)
        return ".jpg";
    if (mimeType.find("png") != std::string::npos)
        return ".png";
    if (mimeType.find("webp") != std::string::npos)
        return ".webp";
    if (mimeType.find("gif") != std::string::npos)
        return ".gif";
    if (mimeType.find("mp4") != std::string::npos || mimeType.find("video") != std::string::npos)
        return ".mp4";
    if (mimeType.find("mpeg") != std::string::npos || mimeType.find("audio") != std::string::npos)
        return ".mp3";
    return ".bin";
}

// Base64, as the phone writes thumbnails: whitespace and all is skipped.
std::string base64Decode(const std::string &text)
{
    static const std::string alphabet =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int accumulator = 0;
    int bits = 0;
    for (const char c : text) {
        const auto position = alphabet.find(c);
        if (position == std::string::npos)
            continue;
        accumulator = (accumulator << 6) | int(position);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(char((accumulator >> bits) & 0xFF));
        }
    }
    return out;
}

// vCards fold long lines with a leading space.
std::vector<std::string> unfold(const std::string &text)
{
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = text.find('\n', start);
        std::string line = text.substr(start, end == std::string::npos ? std::string::npos
                                                                       : end - start);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!line.empty() && (line[0] == ' ' || line[0] == '\t') && !lines.empty())
            lines.back() += line.substr(1);
        else
            lines.push_back(line);
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return lines;
}

// The phone pins the certificate it paired with; the pairing dialog is the
// trust decision, and the code shown on both devices is what makes it safe.
std::string peerCertificatePem(SSL *ssl)
{
    X509 *peer = SSL_get1_peer_certificate(ssl);
    if (!peer)
        return {};
    const std::string text = pemOf(peer);
    X509_free(peer);
    return text;
}

// One line of JSON, the protocol's whole framing.
bool readLine(int fd, std::string &line, int timeoutMs, std::string &rest)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    char buffer[8192];
    for (;;) {
        const auto newline = rest.find('\n');
        if (newline != std::string::npos) {
            line = rest.substr(0, newline);
            rest.erase(0, newline + 1);
            return true;
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  deadline - std::chrono::steady_clock::now())
                                  .count();
        if (left <= 0)
            return false;
        pollfd entry {};
        entry.fd = fd;
        entry.events = POLLIN;
        if (::poll(&entry, 1, int(left)) <= 0)
            return false;
        const ssize_t got = ::recv(fd, buffer, sizeof(buffer), 0);
        if (got <= 0)
            return false;
        rest.append(buffer, size_t(got));
    }
}

} // namespace

PhoneLink::PhoneLink(base::EventLoop *loop)
    : m_loop(loop)
    , m_discovery(loop)
    , m_attachmentTimeout(loop)
{
    m_certificate = loadOrCreatePhoneCertificate();
    m_discovery.setDeviceName(hostName());
}

PhoneLink::~PhoneLink()
{
    stop();
}

const std::string &PhoneLink::deviceId() const
{
    return m_discovery.deviceId();
}

bool PhoneLink::start(std::string &error)
{
    if (m_listener >= 0)
        return true;
    if (m_certificate.certificate.empty()) {
        error = "No TLS identity could be made.";
        return false;
    }
    if (!m_discovery.start()) {
        error = "Port 1716 is busy.";
        return false;
    }
    m_discoveryWatch = m_loop->watch(m_discovery.fd(), POLLIN,
                                     [this](uint32_t) { m_discovery.handleRead(); });

    m_listener = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (m_listener < 0) {
        error = "No socket could be made.";
        return false;
    }
    int reuse = 1;
    ::setsockopt(m_listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    ::setsockopt(m_listener, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(kPort);
    if (::bind(m_listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        error = "Port 1716 is busy.";
        return false;
    }
    if (::listen(m_listener, 8) != 0) {
        error = "Port 1716 cannot be listened on.";
        return false;
    }

    m_listenerWatch = m_loop->watch(m_listener, POLLIN,
                                    [this](uint32_t) { acceptConnection(); });

    base::json::Value body = base::json::Value::object();
    body.insert("deviceId", m_discovery.deviceId());
    body.insert("deviceName", hostName());
    body.insert("deviceType", "desktop");
    body.insert("protocolVersion", 8);
    base::json::Value::Array incoming;
    // Peers check these before sending, so every reply we want is listed.
    for (const char *capability : {"kdeconnect.battery", "kdeconnect.sms.messages",
                                   "kdeconnect.sms.attachment_file",
                                   "kdeconnect.contacts.response_uids_timestamps",
                                   "kdeconnect.contacts.response_vcards", "kdeconnect.pair"})
        incoming.push_back(base::json::Value(capability));
    body.insert("incomingCapabilities", base::json::Value(incoming));
    base::json::Value::Array outgoing;
    for (const char *capability : {"kdeconnect.battery.request", "kdeconnect.sms.request",
                                   "kdeconnect.sms.request_conversations",
                                   "kdeconnect.sms.request_conversation",
                                   "kdeconnect.sms.request_attachment",
                                   "kdeconnect.contacts.request_all_uids_timestamps",
                                   "kdeconnect.contacts.request_vcards_by_uid", "kdeconnect.pair"})
        outgoing.push_back(base::json::Value(capability));
    body.insert("outgoingCapabilities", base::json::Value(outgoing));

    base::json::Value identity = base::json::Value::object();
    identity.insert("id", 0);
    identity.insert("type", "kdeconnect.identity");
    identity.insert("body", std::move(body));
    m_identity = identity.serialize();
    return true;
}

void PhoneLink::stop()
{
    closeLink();
    if (m_listenerWatch) {
        m_loop->unwatch(m_listenerWatch);
        m_listenerWatch = 0;
    }
    if (m_discoveryWatch) {
        m_loop->unwatch(m_discoveryWatch);
        m_discoveryWatch = 0;
    }
    if (m_listener >= 0) {
        ::close(m_listener);
        m_listener = -1;
    }
    m_discovery.stop();
}

void PhoneLink::acceptConnection()
{
    sockaddr_in from {};
    socklen_t length = sizeof(from);
    const int fd = ::accept4(m_listener, reinterpret_cast<sockaddr *>(&from), &length,
                             SOCK_CLOEXEC);
    if (fd < 0)
        return;

    // One phone at a time; a second connection is the same phone retrying.
    if (m_ssl) {
        ::close(fd);
        return;
    }

    char host[INET_ADDRSTRLEN] = {};
    if (inet_ntop(AF_INET, &from.sin_addr, host, sizeof(host)))
        m_peerAddress = host;

    std::string error;
    if (!handshake(fd, error)) {
        ::close(fd);
        if (!error.empty())
            notice(error);
        return;
    }

    m_watch = m_loop->watch(m_fd, POLLIN, [this](uint32_t) { readAvailable(); });
    m_loop->post([this] {
        stateChanged();
        requestConversations();
        requestContacts();
        base::json::Value body = base::json::Value::object();
        body.insert("request", true);
        sendPacket("kdeconnect.battery.request", std::move(body));
    });
}

bool PhoneLink::handshake(int fd, std::string &error)
{
    timeval timeout {};
    timeout.tv_sec = kHandshakeTimeout / 1000;
    timeout.tv_usec = (kHandshakeTimeout % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    // The phone speaks first, in the clear, so we know which device it is.
    std::string line;
    std::string rest;
    if (!readLine(fd, line, kHandshakeTimeout, rest)) {
        error = "The phone connected but did not introduce itself.";
        return false;
    }
    const auto identity = base::json::Value::parse(line);
    if (!identity || (*identity)["type"].toString() != "kdeconnect.identity") {
        error = "The phone's first packet was not an identity.";
        return false;
    }
    const base::json::Value &body = (*identity)["body"];
    m_targetId = body["targetDeviceId"].toString();
    if (!m_targetId.empty() && m_targetId != m_discovery.deviceId()) {
        error.clear();
        return false;
    }
    m_peerId = body["deviceId"].toString();
    m_peerName = body["deviceName"].toString(m_peerId);
    std::fprintf(stderr, "phone: %s (%s) -> us\n", m_peerName.c_str(), m_peerId.c_str());

    m_ctx = SSL_CTX_new(TLS_client_method());
    if (!m_ctx) {
        error = "TLS could not be set up.";
        return false;
    }
    SSL_CTX_set_verify(m_ctx, SSL_VERIFY_PEER, acceptAny);
    BIO *certBio = BIO_new_mem_buf(m_certificate.certificate.data(), int(m_certificate.certificate.size()));
    X509 *x509 = PEM_read_bio_X509(certBio, nullptr, nullptr, nullptr);
    BIO *keyBio = BIO_new_mem_buf(m_certificate.key.data(), int(m_certificate.key.size()));
    EVP_PKEY *key = PEM_read_bio_PrivateKey(keyBio, nullptr, nullptr, nullptr);
    if (x509) {
        SSL_CTX_use_certificate(m_ctx, x509);
        X509_free(x509);
    }
    if (key) {
        SSL_CTX_use_PrivateKey(m_ctx, key);
        EVP_PKEY_free(key);
    }
    BIO_free(certBio);
    BIO_free(keyBio);

    m_ssl = SSL_new(m_ctx);
    if (!m_ssl) {
        error = "TLS could not be set up.";
        return false;
    }
    SSL_set_fd(m_ssl, fd);
    SSL_set_connect_state(m_ssl);
    if (SSL_connect(m_ssl) <= 0) {
        char text[256] = {};
        ERR_error_string_n(ERR_get_error(), text, sizeof(text));
        error = std::string("The phone closed the encrypted connection: ") + text;
        SSL_free(m_ssl);
        m_ssl = nullptr;
        SSL_CTX_free(m_ctx);
        m_ctx = nullptr;
        return false;
    }
    m_peerPem = peerCertificatePem(m_ssl);
    std::fprintf(stderr, "phone: encrypted channel up\n");
    m_fd = fd;

    // Our identity again, now that the channel is encrypted, so the phone can
    // tie the certificate to the device. It goes through TLS, like everything
    // else after the handshake.
    const std::string identityLine = m_identity + "\n";
    size_t done = 0;
    while (done < identityLine.size()) {
        const int written = SSL_write(m_ssl, identityLine.data() + done,
                                      int(identityLine.size() - done));
        if (written <= 0) {
            error = "The phone did not accept our identity.";
            SSL_free(m_ssl);
            m_ssl = nullptr;
            SSL_CTX_free(m_ctx);
            m_ctx = nullptr;
            return false;
        }
        done += size_t(written);
    }

    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    return true;
}

void PhoneLink::closeLink()
{
    if (m_writeWatch) {
        m_loop->unwatch(m_writeWatch);
        m_writeWatch = 0;
    }
    if (m_watch) {
        m_loop->unwatch(m_watch);
        m_watch = 0;
    }
    if (m_ssl) {
        SSL_free(m_ssl);
        m_ssl = nullptr;
    }
    if (m_ctx) {
        SSL_CTX_free(m_ctx);
        m_ctx = nullptr;
    }
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
    m_out.clear();
    m_sent = 0;
    m_in.clear();
    m_battery = -1;
    m_paired = false;
    stateChanged();
}

void PhoneLink::sendRaw(const std::string &line)
{
    if (!m_ssl)
        return;
    m_out += line;
    m_out += '\n';
    flush();
}

void PhoneLink::flush()
{
    if (!m_ssl)
        return;
    while (m_sent < m_out.size()) {
        const int written = SSL_write(m_ssl, m_out.data() + m_sent, int(m_out.size() - m_sent));
        if (written > 0) {
            m_sent += size_t(written);
            continue;
        }
        const int error = SSL_get_error(m_ssl, written);
        if (error == SSL_ERROR_WANT_WRITE || error == SSL_ERROR_WANT_READ) {
            if (!m_writeWatch) {
                m_writeWatch = m_loop->watch(m_fd, POLLOUT, [this](uint32_t) {
                    if (m_writeWatch) {
                        m_loop->unwatch(m_writeWatch);
                        m_writeWatch = 0;
                    }
                    flush();
                });
            }
            return;
        }
        closeLink();
        return;
    }
    m_out.clear();
    m_sent = 0;
    if (m_writeWatch) {
        m_loop->unwatch(m_writeWatch);
        m_writeWatch = 0;
    }
}

bool PhoneLink::sendPacket(const std::string &type, base::json::Value body)
{
    if (!m_ssl)
        return false;
    base::json::Value packet = base::json::Value::object();
    packet.insert("id", nowMs());
    packet.insert("type", type);
    packet.insert("body", std::move(body));
    sendRaw(packet.serialize());
    return true;
}

void PhoneLink::readAvailable()
{
    if (!m_ssl)
        return;
    char buffer[16384];
    for (;;) {
        const int got = SSL_read(m_ssl, buffer, sizeof(buffer));
        if (got > 0) {
            m_in.append(buffer, size_t(got));
            continue;
        }
        const int error = SSL_get_error(m_ssl, got);
        if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE)
            break;
        closeLink();
        return;
    }
    consumeLines();
}

void PhoneLink::consumeLines()
{
    for (;;) {
        const auto newline = m_in.find('\n');
        if (newline == std::string::npos)
            return;
        std::string line = m_in.substr(0, newline);
        m_in.erase(0, newline + 1);
        const auto packet = base::json::Value::parse(line);
        if (!packet || !packet->isObject())
            continue;
        const std::string type = (*packet)["type"].toString();
        if (type.empty())
            continue;
        if (type.rfind("kdeconnect.sms", 0) == 0)
            std::fprintf(stderr, "phone: <- %s\n", type.c_str());
        handlePacket(type, *packet);
    }
}

void PhoneLink::handlePacket(const std::string &type, const base::json::Value &packet)
{
    const base::json::Value &body = packet["body"];
    if (type == "kdeconnect.pair") {
        std::fprintf(stderr, "phone: pairing packet (pair=%s)\n",
                     body["pair"].toBool() ? "true" : "false");
        if (!body["pair"].toBool())
            return;
        const int64_t stamp = body["timestamp"].toInt();
        confirmPairing(stamp);
        return;
    }
    if (type == "kdeconnect.battery") {
        const int charge = int(body["currentCharge"].toInt(-1));
        std::fprintf(stderr, "phone: battery %d%%\n", charge);
        if (charge >= 0) {
            m_battery = charge;
            if (!m_paired) {
                m_paired = true;
                stateChanged();
            }
        }
        return;
    }
    if (type.rfind("kdeconnect.contacts.response_uids_timestamps", 0) == 0) {
        const base::json::Value &uids = body["uids"];
        base::json::Value::Array wanted;
        for (size_t i = 0; i < uids.size(); ++i) {
            const std::string uid = uids[i].toString();
            if (uid.empty())
                continue;
            wanted.push_back(base::json::Value(uid));
            if (wanted.size() >= 100) {
                base::json::Value request = base::json::Value::object();
                request.insert("uids", base::json::Value(std::move(wanted)));
                sendPacket("kdeconnect.contacts.request_vcards_by_uid", std::move(request));
                wanted = {};
            }
        }
        if (!wanted.empty()) {
            base::json::Value request = base::json::Value::object();
            request.insert("uids", base::json::Value(std::move(wanted)));
            sendPacket("kdeconnect.contacts.request_vcards_by_uid", std::move(request));
        }
        return;
    }
    if (type == "kdeconnect.contacts.response_vcards") {
        const base::json::Value &uids = body["uids"];
        for (size_t i = 0; i < uids.size(); ++i) {
            const std::string uid = uids[i].toString();
            if (uid.empty())
                continue;
            const std::string vcard = body[uid].toString();
            if (!vcard.empty())
                addContact(vcard);
        }
        std::fprintf(stderr, "phone: %zu contact(s) known\n", m_contactNames.size());
        contactsChanged();
        return;
    }
    if (type == "kdeconnect.sms.attachment_file") {
        const int port = int(packet["payloadTransferInfo"]["port"].toInt());
        const int64_t size = packet["payloadSize"].toInt();
        if (m_attachmentQueue.empty()) {
            std::fprintf(stderr, "phone: attachment arrived unasked\n");
            return;
        }
        // The filename usually names the part; otherwise the oldest ask wins.
        size_t chosen = 0;
        const std::string filename = body["filename"].toString();
        if (!filename.empty()) {
            for (size_t i = 0; i < m_attachmentQueue.size(); ++i) {
                if (filename.find(m_attachmentQueue[i].identifier) != std::string::npos) {
                    chosen = i;
                    break;
                }
            }
        }
        const PendingAttachment pending = m_attachmentQueue[chosen];
        m_attachmentQueue.erase(m_attachmentQueue.begin() + long(chosen));
        m_attachmentBusy = false;
        m_attachmentTimeout.stop();
        if (downloadPayload(port, size, pending.path)) {
            const auto found = m_threads.find(pending.threadId);
            if (found != m_threads.end()) {
                for (SmsMessage &message : found->second) {
                    if (message.id == pending.messageId) {
                        message.fullPath = pending.path;
                        break;
                    }
                }
            }
            attachmentReady(pending.messageId, pending.path);
        }
        pumpAttachments();
        return;
    }
    if (type != "kdeconnect.sms.messages")
        return;

    const base::json::Value &list = body["messages"];
    if (!list.isArray() || list.size() == 0)
        return;
    std::fprintf(stderr, "phone: %zu message(s)\n", list.size());
    m_paired = true;
    std::vector<SmsMessage> messages;
    messages.reserve(list.size());
    int64_t thread = -1;
    bool oneThread = true;
    for (size_t i = 0; i < list.size(); ++i) {
        const base::json::Value &entry = list[i];
        SmsMessage message;
        message.id = entry["_id"].toInt();
        message.threadId = entry["thread_id"].toInt();
        message.body = entry["body"].toString();
        message.date = entry["date"].toInt();
        message.type = int(entry["type"].toInt());
        message.read = entry["read"].toInt(1) != 0;
        const base::json::Value &addresses = entry["addresses"];
        if (addresses.isArray()) {
            for (size_t a = 0; a < addresses.size(); ++a) {
                const std::string value = addresses[a]["address"].toString();
                if (!value.empty())
                    message.addresses.push_back(value);
            }
        }
        if (!message.addresses.empty())
            message.address = message.addresses.front();
        const base::json::Value &attachments = entry["attachments"];
        if (attachments.isArray()) {
            for (size_t a = 0; a < attachments.size(); ++a) {
                SmsAttachment attachment;
                attachment.partId = attachments[a]["part_id"].toInt();
                attachment.identifier = attachments[a]["unique_identifier"].toString();
                attachment.mimeType = attachments[a]["mime_type"].toString();
                const std::string thumbnail =
                        base64Decode(attachments[a]["encoded_thumbnail"].toString());
                if (!thumbnail.empty()) {
                    const std::string path = attachmentDirectory() + "/t_"
                            + std::to_string(message.id) + "_"
                            + std::to_string(attachment.partId) + extensionFor(attachment.mimeType);
                    std::ofstream out(path, std::ios::binary | std::ios::trunc);
                    out.write(thumbnail.data(), std::streamsize(thumbnail.size()));
                    attachment.thumbnailPath = path;
                }
                // The original may already be in the cache.
                const std::string full = attachmentDirectory() + "/"
                        + std::to_string(message.id) + "_"
                        + std::to_string(attachment.partId) + extensionFor(attachment.mimeType);
                if (std::filesystem::exists(full))
                    message.fullPath = full;
                if (!attachment.thumbnailPath.empty() || !attachment.identifier.empty())
                    message.attachments.push_back(std::move(attachment));
            }
        }
        if (thread < 0)
            thread = message.threadId;
        else if (message.threadId != thread)
            oneThread = false;
        messages.push_back(std::move(message));
    }

    if (oneThread && thread >= 0 && thread == m_pendingThread) {
        m_pendingThread = 0;
        m_threads[thread] = messages;
        // A fetched thread knows its newest message; the phone's per-thread
        // summary can lag behind it, so let the message win and say so.
        int64_t newest = 0;
        const SmsMessage *last = nullptr;
        for (const SmsMessage &message : messages) {
            if (message.date >= newest) {
                newest = message.date;
                last = &message;
            }
        }
        if (last) {
            Conversation &conversation = m_conversations[thread];
            conversation.threadId = thread;
            conversation.date = newest;
            conversation.snippet = !last->body.empty()
                    ? last->body
                    : (last->attachments.empty() ? std::string() : std::string("Photo"));
            conversation.type = last->type;
            conversation.read = last->read;
            if (conversation.address.empty())
                conversation.address = last->address;
            conversation.addresses = last->addresses;
            std::vector<Conversation> list;
            list.reserve(m_conversations.size());
            for (const auto &[id, known] : m_conversations)
                list.push_back(known);
            std::sort(list.begin(), list.end(),
                      [](const Conversation &left, const Conversation &right) {
                          return left.date > right.date;
                      });
            conversationsReceived(std::move(list));
            std::fprintf(stderr, "phone: thread %lld has %zu messages, newest %lld\n",
                         static_cast<long long>(thread), messages.size(),
                         static_cast<long long>(newest));
        }
        const std::vector<SmsMessage> copy = messages;
        threadReceived(thread, std::move(messages));

        // The newest pictures upgrade on their own; older ones stay previews
        // until someone asks to see them, so opening a long thread stays light.
        int asked = 0;
        for (size_t i = copy.size(); i > 0 && asked < 6; --i) {
            const SmsMessage &message = copy[i - 1];
            if (!message.fullPath.empty() || message.attachments.empty())
                continue;
            if (message.attachments[0].identifier.empty())
                continue;
            requestAttachment(thread, message.id, 0);
            ++asked;
        }
        return;
    }

    // Otherwise these are the latest message of each thread, one packet at a
    // time; keep the newest per thread and publish the whole list.
    for (const SmsMessage &message : messages) {
        Conversation &conversation = m_conversations[message.threadId];
        if (conversation.threadId != 0 && conversation.date > message.date)
            continue;
        conversation.threadId = message.threadId;
        conversation.address = message.address;
        conversation.addresses = message.addresses;
        conversation.snippet = !message.body.empty()
                ? message.body
                : (message.attachments.empty() ? std::string() : std::string("Photo"));
        conversation.date = message.date;
        conversation.type = message.type;
        conversation.read = message.read;
    }
    // A message the phone pushed for a thread we already hold joins it, newest
    // last, with the same id replacing its older copy.
    for (const SmsMessage &message : messages) {
        const auto cached = m_threads.find(message.threadId);
        if (cached == m_threads.end())
            continue;
        std::vector<SmsMessage> &thread = cached->second;
        const auto same = std::find_if(thread.begin(), thread.end(), [&message](const SmsMessage &known) {
            return known.id == message.id;
        });
        if (same != thread.end())
            *same = message;
        else
            thread.push_back(message);
        std::sort(thread.begin(), thread.end(),
                  [](const SmsMessage &left, const SmsMessage &right) {
                      return left.date < right.date;
                  });
        threadReceived(message.threadId, thread);
    }

    std::vector<Conversation> conversations;
    conversations.reserve(m_conversations.size());
    for (const auto &[id, conversation] : m_conversations)
        conversations.push_back(conversation);
    std::sort(conversations.begin(), conversations.end(),
              [](const Conversation &left, const Conversation &right) {
                  return left.date > right.date;
              });
    conversationsReceived(std::move(conversations));
}

void PhoneLink::confirmPairing(int64_t stamp)
{
    // The phone asked while the user watches its screen; echoing the stamp is
    // the confirmation its side waits for.
    base::json::Value body = base::json::Value::object();
    body.insert("pair", true);
    body.insert("timestamp", stamp);
    sendPacket("kdeconnect.pair", std::move(body));
    std::fprintf(stderr, "phone: pairing confirmed with stamp %lld\n",
                 static_cast<long long>(stamp));
    if (!m_paired) {
        m_paired = true;
        stateChanged();
        requestConversations();
        requestContacts();
    }
}

void PhoneLink::requestConversations()
{
    if (!m_ssl)
        return;
    sendPacket("kdeconnect.sms.request_conversations", base::json::Value::object());
}

void PhoneLink::requestContacts()
{
    if (!m_ssl)
        return;
    sendPacket("kdeconnect.contacts.request_all_uids_timestamps", base::json::Value::object());
}

std::string PhoneLink::nameFor(const std::string &address) const
{
    const auto found = m_contactNames.find(numberKey(address));
    return found == m_contactNames.end() ? std::string() : found->second;
}

void PhoneLink::addContact(const std::string &vcard)
{
    std::string name;
    std::vector<std::string> numbers;
    for (const std::string &line : unfold(vcard)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        std::string key = line.substr(0, colon);
        for (char &c : key)
            c = char(std::toupper(static_cast<unsigned char>(c)));
        const std::string value = line.substr(colon + 1);
        if (key == "FN" || key.rfind("FN;", 0) == 0)
            name = value;
        else if (key == "TEL" || key.rfind("TEL;", 0) == 0)
            numbers.push_back(value);
    }
    if (name.empty())
        return;
    for (const std::string &number : numbers) {
        const std::string key = numberKey(number);
        if (!key.empty())
            m_contactNames[key] = name;
    }
}

void PhoneLink::requestAttachmentNow(int64_t messageId)
{
    for (const auto &[threadId, list] : m_threads) {
        for (const SmsMessage &message : list) {
            if (message.id != messageId || !message.fullPath.empty())
                continue;
            if (!message.attachments.empty())
                requestAttachment(threadId, messageId, 0, true);
            return;
        }
    }
}

void PhoneLink::requestAttachment(int64_t threadId, int64_t messageId, size_t index,
                                  bool priority)
{
    const auto found = m_threads.find(threadId);
    if (found == m_threads.end())
        return;
    for (const SmsMessage &message : found->second) {
        if (message.id != messageId || index >= message.attachments.size())
            continue;
        const SmsAttachment &attachment = message.attachments[index];
        if (attachment.identifier.empty())
            return;
        const std::string path = attachmentDirectory() + "/" + std::to_string(messageId) + "_"
                + std::to_string(attachment.partId) + extensionFor(attachment.mimeType);
        if (std::filesystem::exists(path))
            return;
        for (const PendingAttachment &queued : m_attachmentQueue) {
            if (queued.path == path)
                return;
        }
        const PendingAttachment pending {threadId, messageId, attachment.identifier, path};
        if (priority)
            m_attachmentQueue.insert(m_attachmentQueue.begin(), pending);
        else
            m_attachmentQueue.push_back(pending);
        pumpAttachments();
        return;
    }
}

void PhoneLink::pumpAttachments()
{
    if (m_attachmentBusy || m_attachmentQueue.empty() || !m_ssl)
        return;
    const PendingAttachment next = m_attachmentQueue.front();
    int64_t partId = 0;
    const auto found = m_threads.find(next.threadId);
    if (found != m_threads.end()) {
        for (const SmsMessage &message : found->second) {
            if (message.id != next.messageId)
                continue;
            for (const SmsAttachment &attachment : message.attachments) {
                if (attachment.identifier == next.identifier) {
                    partId = attachment.partId;
                    break;
                }
            }
        }
    }
    if (partId == 0) {
        m_attachmentQueue.erase(m_attachmentQueue.begin());
        pumpAttachments();
        return;
    }
    m_attachmentBusy = true;
    base::json::Value body = base::json::Value::object();
    body.insert("part_id", partId);
    body.insert("unique_identifier", next.identifier);
    sendPacket("kdeconnect.sms.request_attachment", std::move(body));
    std::fprintf(stderr, "phone: asked for part %lld of message %lld\n",
                 static_cast<long long>(partId), static_cast<long long>(next.messageId));
    // If the phone never answers, move on rather than stalling the queue.
    m_attachmentTimeout.setSingleShot(true);
    m_attachmentTimeout.setInterval(std::chrono::seconds(20));
    m_attachmentTimeout.onTimeout = [this] {
        if (!m_attachmentBusy)
            return;
        m_attachmentBusy = false;
        if (!m_attachmentQueue.empty())
            m_attachmentQueue.erase(m_attachmentQueue.begin());
        pumpAttachments();
    };
    m_attachmentTimeout.start();
}

bool PhoneLink::downloadPayload(int port, int64_t size, const std::string &path)
{
    if (m_peerAddress.empty() || port <= 0 || size <= 0 || size > 64 * 1024 * 1024)
        return false;

    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx)
        return false;
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, acceptAny);
    BIO *certBio = BIO_new_mem_buf(m_certificate.certificate.data(), int(m_certificate.certificate.size()));
    X509 *x509 = PEM_read_bio_X509(certBio, nullptr, nullptr, nullptr);
    BIO *keyBio = BIO_new_mem_buf(m_certificate.key.data(), int(m_certificate.key.size()));
    EVP_PKEY *key = PEM_read_bio_PrivateKey(keyBio, nullptr, nullptr, nullptr);
    if (x509) {
        SSL_CTX_use_certificate(ctx, x509);
        X509_free(x509);
    }
    if (key) {
        SSL_CTX_use_PrivateKey(ctx, key);
        EVP_PKEY_free(key);
    }
    BIO_free(certBio);
    BIO_free(keyBio);

    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        SSL_CTX_free(ctx);
        return false;
    }
    timeval timeout {};
    timeout.tv_sec = 10;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    bool ok = inet_pton(AF_INET, m_peerAddress.c_str(), &address.sin_addr) == 1
            && ::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0;
    if (!ok) {
        std::fprintf(stderr, "phone: cannot reach payload port %d\n", port);
        ::close(fd);
        SSL_CTX_free(ctx);
        return false;
    }

    SSL *ssl = SSL_new(ctx);
    SSL_set_fd(ssl, fd);
    SSL_set_connect_state(ssl);
    ok = SSL_connect(ssl) > 0;
    if (!ok) {
        char text[256] = {};
        ERR_error_string_n(ERR_get_error(), text, sizeof(text));
        std::fprintf(stderr, "phone: payload TLS failed: %s\n", text);
    }

    const std::string temporary = path + ".part";
    int64_t received = 0;
    if (ok) {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        ok = out.good();
        char buffer[16384];
        while (ok && received < size) {
            const int want = int(std::min<int64_t>(sizeof(buffer), size - received));
            const int got = SSL_read(ssl, buffer, want);
            if (got <= 0) {
                ok = false;
                break;
            }
            out.write(buffer, got);
            received += got;
        }
    }
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    ::close(fd);

    if (ok && received == size && std::rename(temporary.c_str(), path.c_str()) == 0) {
        std::fprintf(stderr, "phone: attachment %lld bytes saved\n",
                     static_cast<long long>(received));
        return true;
    }
    std::remove(temporary.c_str());
    std::fprintf(stderr, "phone: attachment incomplete (%lld of %lld)\n",
                 static_cast<long long>(received), static_cast<long long>(size));
    return false;
}

void PhoneLink::requestThread(int64_t threadId)
{
    if (!m_ssl)
        return;
    m_pendingThread = threadId;
    base::json::Value body = base::json::Value::object();
    body.insert("threadID", threadId);
    sendPacket("kdeconnect.sms.request_conversation", std::move(body));
}

void PhoneLink::noteSent(int64_t threadId, const std::string &body, int64_t date)
{
    Conversation &conversation = m_conversations[threadId];
    conversation.threadId = threadId;
    conversation.date = date;
    conversation.snippet = body;
    conversation.type = 2;
    conversation.read = true;
}

bool PhoneLink::sendSms(const std::vector<std::string> &addresses,
                        const std::string &body, std::string &error)
{
    if (!m_ssl) {
        error = "The phone is not connected.";
        return false;
    }
    base::json::Value::Array entries;
    for (const std::string &address : addresses) {
        base::json::Value entry = base::json::Value::object();
        entry.insert("address", address);
        entries.push_back(std::move(entry));
    }
    if (entries.empty()) {
        error = "This conversation has no address.";
        return false;
    }

    std::fprintf(stderr, "phone: sending to %zu address(es): \"%s\"\n", entries.size(),
                 body.c_str());
    base::json::Value packetBody = base::json::Value::object();
    packetBody.insert("addresses", base::json::Value(std::move(entries)));
    packetBody.insert("messageBody", body);
    packetBody.insert("version", 2);
    if (!sendPacket("kdeconnect.sms.request", std::move(packetBody))) {
        error = "The message could not be sent.";
        std::fprintf(stderr, "phone: send failed\n");
        return false;
    }
    std::fprintf(stderr, "phone: send packet written\n");
    return true;
}

} // namespace cutefish::phone
