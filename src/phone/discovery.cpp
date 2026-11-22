// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "phone/discovery.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>

#include "base/json.h"
#include "base/paths.h"
#include "base/uuid.h"

namespace cutefish::phone {

namespace {

constexpr int kPort = 1716;
constexpr const char *kIdentity = "kdeconnect.identity";

// The protocol's ids are 32 hex characters; a dashed UUID is not one.
std::string normalizeId(std::string id)
{
    std::erase(id, '-');
    return id;
}

std::string loadOrMakeId()
{
    const std::filesystem::path path(base::paths::cutefishConfig("messages") + ".d/device_id");
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ifstream file(path);
    std::string id;
    if (file && std::getline(file, id)) {
        id = normalizeId(id);
        if (id.size() == 32)
            return id;
    }
    id = normalizeId(base::Uuid::generate().toString());
    std::ofstream out(path, std::ios::trunc);
    out << id << "\n";
    return id;
}

base::json::Value identityPacket(const std::string &id, const std::string &name)
{
    base::json::Value body = base::json::Value::object();
    body.insert("deviceId", id);
    body.insert("deviceName", name);
    body.insert("deviceType", "desktop");
    body.insert("protocolVersion", 8);
    base::json::Value::Array incoming;
    incoming.push_back(base::json::Value("kdeconnect.sms.request"));
    incoming.push_back(base::json::Value("kdeconnect.sms.request_conversations"));
    incoming.push_back(base::json::Value("kdeconnect.sms.request_conversation"));
    incoming.push_back(base::json::Value("kdeconnect.pair"));
    body.insert("incomingCapabilities", base::json::Value(incoming));
    base::json::Value::Array outgoing;
    outgoing.push_back(base::json::Value("kdeconnect.sms.messages"));
    outgoing.push_back(base::json::Value("kdeconnect.pair"));
    body.insert("outgoingCapabilities", base::json::Value(outgoing));

    base::json::Value packet = base::json::Value::object();
    packet.insert("id", 0);
    packet.insert("type", kIdentity);
    packet.insert("body", std::move(body));
    return packet;
}

} // namespace

Discovery::Discovery(base::EventLoop *loop)
    : m_loop(loop)
    , m_announce(loop)
    , m_id(loadOrMakeId())
{
    char host[256] = {};
    if (::gethostname(host, sizeof(host) - 1) == 0 && host[0] != '\0')
        m_name = host;
    else
        m_name = "Cutefish";
}

Discovery::~Discovery()
{
    stop();
}

bool Discovery::start()
{
    m_fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (m_fd < 0)
        return false;
    int reuse = 1;
    ::setsockopt(m_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    ::setsockopt(m_fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
    ::setsockopt(m_fd, SOL_SOCKET, SO_BROADCAST, &reuse, sizeof(reuse));

    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(kPort);
    if (::bind(m_fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        stop();
        return false;
    }

    m_announce.setInterval(std::chrono::seconds(10));
    m_announce.onTimeout = [this] { announce(); };
    m_announce.start();
    announce();
    return true;
}

void Discovery::stop()
{
    m_announce.stop();
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
}

void Discovery::announce()
{
    if (m_fd < 0)
        return;
    const std::string text = identityPacket(m_id, m_name).serialize();
    sockaddr_in to {};
    to.sin_family = AF_INET;
    to.sin_port = htons(kPort);
    to.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    // The broadcast address of every interface, so Wi-Fi and Ethernet both
    // hear it, and the phone can answer.
    ifaddrs *interfaces = nullptr;
    if (getifaddrs(&interfaces) == 0) {
        for (ifaddrs *entry = interfaces; entry; entry = entry->ifa_next) {
            if (!entry->ifa_addr || entry->ifa_addr->sa_family != AF_INET || !entry->ifa_broadaddr)
                continue;
            ::sendto(m_fd, text.data(), text.size(), 0, entry->ifa_broadaddr,
                     sizeof(sockaddr_in));
        }
        freeifaddrs(interfaces);
    }
    ::sendto(m_fd, text.data(), text.size(), 0, reinterpret_cast<sockaddr *>(&to), sizeof(to));

    // And straight to each device already heard from: a phone in doze is more
    // likely to act on a unicast identity than on a broadcast.
    std::vector<Peer> known;
    {
        const std::lock_guard lock(m_mutex);
        known = m_peers;
    }
    for (const Peer &peer : known) {
        sockaddr_in direct {};
        direct.sin_family = AF_INET;
        direct.sin_port = htons(kPort);
        if (inet_pton(AF_INET, peer.address.c_str(), &direct.sin_addr) != 1)
            continue;
        ::sendto(m_fd, text.data(), text.size(), 0, reinterpret_cast<sockaddr *>(&direct),
                 sizeof(direct));
    }
}

void Discovery::handleRead()
{
    char buffer[65536];
    sockaddr_in from {};
    socklen_t length = sizeof(from);
    const ssize_t got = ::recvfrom(m_fd, buffer, sizeof(buffer) - 1, 0,
                                   reinterpret_cast<sockaddr *>(&from), &length);
    if (got <= 0)
        return;
    const std::string text(buffer, size_t(got));
    const auto parsed = base::json::Value::parse(text);
    if (!parsed || !parsed->isObject() || (*parsed)["type"].toString() != kIdentity)
        return;
    const base::json::Value &body = (*parsed)["body"];
    const std::string id = body["deviceId"].toString();
    if (id.empty() || id == m_id)
        return;

    Peer peer;
    peer.id = id;
    peer.name = body["deviceName"].toString();
    peer.type = body["deviceType"].toString();
    peer.protocol = int(body["protocolVersion"].toInt(8));
    char host[INET_ADDRSTRLEN] = {};
    if (inet_ntop(AF_INET, &from.sin_addr, host, sizeof(host)))
        peer.address = host;
    if (peer.name.empty())
        peer.name = peer.address;
    if (peer.address.empty())
        return;

    bool isNew = false;
    {
        const std::lock_guard lock(m_mutex);
        const auto found = std::find_if(m_peers.begin(), m_peers.end(),
                                        [&id](const Peer &known) { return known.id == id; });
        if (found == m_peers.end()) {
            m_peers.push_back(peer);
            isNew = true;
        } else {
            found->name = peer.name;
            found->type = peer.type;
            found->address = peer.address;
        }
    }
    if (isNew) {
        found(peer);
        changed();
    }
}

std::vector<Peer> Discovery::peers() const
{
    const std::lock_guard lock(m_mutex);
    return m_peers;
}

} // namespace cutefish::phone
