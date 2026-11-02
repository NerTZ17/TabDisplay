#pragma once

#include <winsock2.h>

#include <cstdint>
#include <mutex>
#include <vector>

#include "protocol.h"

namespace td {

class NetInit {
public:
    NetInit();
    ~NetInit();
};

class Connection {
public:
    ~Connection();

    bool connect(uint16_t port);
    void close();
    bool send(const std::vector<uint8_t>& message);
    bool receive(Msg& type, std::vector<uint8_t>& payload, int timeoutMs);
    bool isOpen() const { return sock_ != INVALID_SOCKET; }

private:
    bool readExact(uint8_t* p, size_t n);

    SOCKET sock_ = INVALID_SOCKET;
    std::mutex sendMutex_;
    int currentTimeout_ = -1;
};

}
