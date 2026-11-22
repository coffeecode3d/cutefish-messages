// SPDX-FileCopyrightText: 2026 coffeecode <212984587+coffeecode3d@users.noreply.github.com>
// SPDX-License-Identifier: BSD-3-Clause

// Listens as the desktop does, and pairs with the phone that connects to us:
// the protocol's inbound path, without any KDE process.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <string>

#include "base/event_loop.h"
#include "base/json.h"
#include "phone/certificate.h"
#include "phone/discovery.h"

using namespace cutefish;

namespace {

std::string hexOf(const unsigned char *bytes, size_t size)
{
    static constexpr char kDigits[] = "0123456789ABCDEF";
    std::string text(size * 2, '\0');
    for (size_t i = 0; i < size; ++i) {
        text[i * 2] = kDigits[bytes[i] >> 4];
        text[i * 2 + 1] = kDigits[bytes[i] & 0x0F];
    }
    return text;
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

bool writeFramed(SSL *ssl, int fd, const std::string &text)
{
    // The protocol carries one JSON object per line; there is no length prefix.
    const std::string line = text + "\n";
    size_t done = 0;
    while (done < line.size()) {
        const int written = ssl
                ? SSL_write(ssl, line.data() + done, int(line.size() - done))
                : int(::send(fd, line.data() + done, line.size() - done, MSG_NOSIGNAL));
        if (written <= 0)
            return false;
        done += size_t(written);
    }
    return true;
}

bool readFramed(SSL *ssl, int fd, std::string &text, int timeoutMs)
{
    pollfd entry {};
    entry.fd = fd;
    entry.events = POLLIN;
    text.clear();
    char buffer[4096];
    while (text.size() < 8 * 1024 * 1024) {
        entry.revents = 0;
        if (::poll(&entry, 1, timeoutMs) <= 0)
            return false;
        const int got = ssl ? SSL_read(ssl, buffer, sizeof(buffer))
                            : int(::recv(fd, buffer, sizeof(buffer), 0));
        if (got <= 0)
            return false;
        text.append(buffer, size_t(got));
        const auto newline = text.find('\n');
        if (newline != std::string::npos) {
            text.resize(newline);
            return true;
        }
    }
    return false;
}

} // namespace

int main()
{
    base::EventLoop loop;
    phone::Discovery discovery(&loop);
    const auto cert = phone::loadOrCreatePhoneCertificate();
    if (cert.certificate.empty()) {
        std::fprintf(stderr, "cannot make the certificate\n");
        return 1;
    }
    std::fprintf(stderr, "device %s, fingerprint %s\n", discovery.deviceId().c_str(),
                 cert.fingerprint.c_str());

    const int listener = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int reuse = 1;
    ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    ::setsockopt(listener, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(1716);
    if (::bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0
        || ::listen(listener, 8) != 0) {
        std::fprintf(stderr, "cannot listen on 1716\n");
        return 1;
    }
    if (!discovery.start()) {
        std::fprintf(stderr, "cannot broadcast\n");
        return 1;
    }

    std::fprintf(stderr, "waiting for the phone to connect…\n");
    pollfd entry {};
    entry.fd = listener;
    entry.events = POLLIN;
    if (::poll(&entry, 1, 20000) <= 0) {
        std::fprintf(stderr, "no connection\n");
        return 1;
    }
    sockaddr_in from {};
    socklen_t length = sizeof(from);
    const int fd = ::accept4(listener, reinterpret_cast<sockaddr *>(&from), &length, SOCK_CLOEXEC);
    std::fprintf(stderr, "accept4 -> %d errno %d, listener %d\n", fd, errno, listener);
    char host[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &from.sin_addr, host, sizeof(host));
    std::fprintf(stderr, "connected from %s\n", host);

    std::string text;
    std::fprintf(stderr, "accepted fd %d\n", fd);
    if (!readFramed(nullptr, fd, text, 5000)) {
        std::fprintf(stderr, "no plain identity\n");
        return 1;
    }
    std::fprintf(stderr, "plain: %s\n", text.substr(0, 220).c_str());
    const auto parsed = base::json::Value::parse(text);
    const std::string peerId = parsed ? (*parsed)["body"]["deviceId"].toString() : std::string();
    const std::string target = parsed ? (*parsed)["body"]["targetDeviceId"].toString() : std::string();
    std::fprintf(stderr, "phone %s -> target %s\n", peerId.c_str(), target.c_str());
    if (target != discovery.deviceId()) {
        std::fprintf(stderr, "the identity is for another device\n");
        return 1;
    }

    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, acceptAny);
    BIO *certBio = BIO_new_mem_buf(cert.certificate.data(), int(cert.certificate.size()));
    X509 *x509 = PEM_read_bio_X509(certBio, nullptr, nullptr, nullptr);
    BIO *keyBio = BIO_new_mem_buf(cert.key.data(), int(cert.key.size()));
    EVP_PKEY *key = PEM_read_bio_PrivateKey(keyBio, nullptr, nullptr, nullptr);
    SSL_CTX_use_certificate(ctx, x509);
    SSL_CTX_use_PrivateKey(ctx, key);
    X509_free(x509);
    EVP_PKEY_free(key);
    BIO_free(certBio);
    BIO_free(keyBio);

    SSL *ssl = SSL_new(ctx);
    SSL_set_fd(ssl, fd);
    SSL_set_connect_state(ssl);
    if (SSL_connect(ssl) <= 0) {
        char error[256] = {};
        ERR_error_string_n(ERR_get_error(), error, sizeof(error));
        std::fprintf(stderr, "TLS failed: %s\n", error);
        return 1;
    }
    std::fprintf(stderr, "TLS up\n");

    std::string peerPem;
    if (X509 *peer = SSL_get_peer_certificate(ssl))
        peerPem = pemOf(peer), X509_free(peer);

    base::json::Value body = base::json::Value::object();
    body.insert("deviceId", discovery.deviceId());
    body.insert("deviceName", "coffee-pc");
    body.insert("deviceType", "desktop");
    body.insert("protocolVersion", 8);
    base::json::Value::Array incoming;
    // What we can receive. Peers check this before sending, so the replies we
    // want (battery status, message batches) must be listed here.
    for (const char *capability : {"kdeconnect.battery", "kdeconnect.sms.messages",
                                   "kdeconnect.pair"})
        incoming.push_back(base::json::Value(capability));
    body.insert("incomingCapabilities", base::json::Value(incoming));
    base::json::Value::Array outgoing;
    for (const char *capability : {"kdeconnect.battery.request", "kdeconnect.sms.request",
                                   "kdeconnect.sms.request_conversations",
                                   "kdeconnect.sms.request_conversation",
                                   "kdeconnect.sms.request_attachment", "kdeconnect.pair"})
        outgoing.push_back(base::json::Value(capability));
    body.insert("outgoingCapabilities", base::json::Value(outgoing));
    base::json::Value identity = base::json::Value::object();
    identity.insert("id", 0);
    identity.insert("type", "kdeconnect.identity");
    identity.insert("body", std::move(body));
    writeFramed(ssl, fd, identity.serialize());

    const int64_t timestamp = int64_t(std::time(nullptr));
    const bool doPair = std::getenv("PAIR") != nullptr;
    base::json::Value pairBody = base::json::Value::object();
    pairBody.insert("pair", true);
    pairBody.insert("timestamp", timestamp);
    base::json::Value pair = base::json::Value::object();
    pair.insert("id", 1);
    pair.insert("type", "kdeconnect.pair");
    pair.insert("body", std::move(pairBody));
    if (doPair)
        writeFramed(ssl, fd, pair.serialize());

    // The code both sides display: SHA-256 over the two certificates, sorted, and the stamp.
    auto showCode = [&](int64_t stamp) {
        std::string ours = cert.certificate;
        std::string theirs = peerPem;
        if (theirs < ours)
            std::swap(ours, theirs);
        const std::string material = ours + theirs + std::to_string(stamp);
        std::array<unsigned char, SHA256_DIGEST_LENGTH> digest {};
        SHA256(reinterpret_cast<const unsigned char *>(material.data()), material.size(),
               digest.data());
        std::fprintf(stderr, "verification code %s\n", hexOf(digest.data(), 4).c_str());
    };
    showCode(timestamp);

    bool asked = false;
    bool askedThread = false;
    auto ask = [&](const char *type, bool batteryRequest = false) {
        base::json::Value body = base::json::Value::object();
        if (batteryRequest)
            body.insert("request", true);
        base::json::Value request = base::json::Value::object();
        request.insert("id", int64_t(std::time(nullptr)));
        request.insert("type", type);
        request.insert("body", std::move(body));
        writeFramed(ssl, fd, request.serialize());
        std::fprintf(stderr, "sent %s\n", type);
    };
    ask("kdeconnect.battery.request", true);
    ask("kdeconnect.sms.request_conversations");
    asked = true;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(150);
    auto nextAsk = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline) {
        std::string packet;
        if (readFramed(ssl, fd, packet, 1000)) {
            const auto parsedPacket = base::json::Value::parse(packet);
            if (!parsedPacket) {
                std::fprintf(stderr, "unreadable packet\n");
            } else {
                const std::string type = (*parsedPacket)["type"].toString();
                std::fprintf(stderr, "packet %s\n", type.c_str());
                if (type == "kdeconnect.pair") {
                    const auto &pairBody = (*parsedPacket)["body"];
                    const bool wanted = pairBody["pair"].toBool();
                    const int64_t stamp = pairBody["timestamp"].toInt();
                    std::fprintf(stderr, "pair request: %s, stamp %lld\n",
                                 wanted ? "accept" : "refuse", static_cast<long long>(stamp));
                    if (!wanted)
                        continue;
                    // The side that asked is waiting for our confirmation, echoing its stamp.
                    base::json::Value replyBody = base::json::Value::object();
                    replyBody.insert("pair", true);
                    replyBody.insert("timestamp", stamp);
                    base::json::Value reply = base::json::Value::object();
                    reply.insert("id", int64_t(std::time(nullptr)));
                    reply.insert("type", "kdeconnect.pair");
                    reply.insert("body", std::move(replyBody));
                    writeFramed(ssl, fd, reply.serialize());
                    std::fprintf(stderr, "pairing confirmed\n");
                    showCode(stamp);
                    ask("kdeconnect.battery.request", true);
                    ask("kdeconnect.sms.request_conversations");
                } else if (type == "kdeconnect.battery") {
                    std::fprintf(stderr, "BATTERY %s\n", packet.c_str());
                } else if (type == "kdeconnect.sms.messages") {
                    std::fprintf(stderr, "MESSAGES %s\n", packet.c_str());
                    if (!askedThread) {
                        const auto &messages = (*parsedPacket)["body"]["messages"];
                        if (messages.size() > 0) {
                            askedThread = true;
                            const int64_t thread = messages[size_t(0)]["thread_id"].toInt();
                            base::json::Value body = base::json::Value::object();
                            body.insert("threadID", thread);
                            base::json::Value request = base::json::Value::object();
                            request.insert("id", int64_t(std::time(nullptr)));
                            request.insert("type", "kdeconnect.sms.request_conversation");
                            request.insert("body", std::move(body));
                            writeFramed(ssl, fd, request.serialize());
                            std::fprintf(stderr, "asked for thread %lld\n",
                                         static_cast<long long>(thread));
                        }
                    }
                }
            }
        }
        if (asked && std::chrono::steady_clock::now() > nextAsk) {
            ask("kdeconnect.sms.request_conversations");
            nextAsk = std::chrono::steady_clock::now() + std::chrono::seconds(25);
        }
    }
    return 0;
}
