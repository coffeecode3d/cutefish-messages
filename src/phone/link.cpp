// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
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
#include <array>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdio>
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
        if (addresses.isArray() && addresses.size() > 0)
            message.address = addresses[size_t(0)]["address"].toString();
        if (thread < 0)
            thread = message.threadId;
        else if (message.threadId != thread)
            oneThread = false;
        messages.push_back(std::move(message));
    }

    if (oneThread && thread >= 0 && thread == m_pendingThread) {
        m_pendingThread = 0;
        m_threads[thread] = messages;
        threadReceived(thread, std::move(messages));
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
        conversation.snippet = message.body;
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

void PhoneLink::requestThread(int64_t threadId)
{
    if (!m_ssl)
        return;
    m_pendingThread = threadId;
    base::json::Value body = base::json::Value::object();
    body.insert("threadID", threadId);
    sendPacket("kdeconnect.sms.request_conversation", std::move(body));
}

bool PhoneLink::sendSms(const std::string &address, const std::string &body, std::string &error)
{
    if (!m_ssl) {
        error = "The phone is not connected.";
        return false;
    }
    base::json::Value entry = base::json::Value::object();
    entry.insert("address", address);
    base::json::Value::Array addresses;
    addresses.push_back(std::move(entry));

    std::fprintf(stderr, "phone: sending to %s: \"%s\"\n", address.c_str(), body.c_str());
    base::json::Value packetBody = base::json::Value::object();
    packetBody.insert("addresses", base::json::Value(std::move(addresses)));
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
