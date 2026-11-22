// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include <cstdio>
#include <sys/epoll.h>

#include "base/event_loop.h"
#include "base/timer.h"
#include "phone/discovery.h"

int main()
{
    cutefish::base::EventLoop loop;
    cutefish::phone::Discovery discovery(&loop);
    if (!discovery.start()) {
        std::fprintf(stderr, "cannot listen on 1716\n");
        return 1;
    }
    loop.watch(discovery.fd(), EPOLLIN, [&discovery](uint32_t) { discovery.handleRead(); });

    cutefish::base::Timer stop(&loop);
    stop.setSingleShot(true);
    stop.setInterval(std::chrono::seconds(8));
    stop.onTimeout = [&loop] { loop.quit(); };
    stop.start();

    loop.run();
    for (const auto &peer : discovery.peers())
        std::printf("%s\t%s\t%s\t%s:%d\tprotocol %d\n", peer.id.c_str(), peer.name.c_str(),
                    peer.type.c_str(), peer.address.c_str(), peer.port, peer.protocol);
    return 0;
}
