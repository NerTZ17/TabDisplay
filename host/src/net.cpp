#include "net.h"

#include <ws2tcpip.h>

namespace td {

NetInit::NetInit() {
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
}

NetInit::~NetInit() { WSACleanup(); }

Connection::~Connection() { close(); }

bool Connection::connect(uint16_t port) {
    close();
    sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock_ == INVALID_SOCKET) {
        return false;
    }
    BOOL noDelay = TRUE;
    setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof noDelay);
    int bufSize = 8 * 1024 * 1024;
    setsockopt(sock_, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&bufSize), sizeof bufSize);
    setsockopt(sock_, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&bufSize), sizeof bufSize);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        close();
        return false;
    }
    currentTimeout_ = -1;
    return true;
}

void Connection::close() {
    if (sock_ != INVALID_SOCKET) {
        shutdown(sock_, SD_BOTH);
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
    }
}

bool Connection::send(const std::vector<uint8_t>& message) {
    std::lock_guard<std::mutex> lock(sendMutex_);
    if (sock_ == INVALID_SOCKET) {
        return false;
    }
    const char* p = reinterpret_cast<const char*>(message.data());
    size_t left = message.size();
    while (left > 0) {
        int chunk = static_cast<int>(left > (1u << 30) ? (1u << 30) : left);
        int sent = ::send(sock_, p, chunk, 0);
        if (sent <= 0) {
            return false;
        }
        p += sent;
        left -= static_cast<size_t>(sent);
    }
    return true;
}

bool Connection::readExact(uint8_t* p, size_t n) {
    while (n > 0) {
        int got = recv(sock_, reinterpret_cast<char*>(p), static_cast<int>(n), 0);
        if (got <= 0) {
            return false;
        }
        p += got;
        n -= static_cast<size_t>(got);
    }
    return true;
}

bool Connection::receive(Msg& type, std::vector<uint8_t>& payload, int timeoutMs) {
    if (sock_ == INVALID_SOCKET) {
        return false;
    }
    if (timeoutMs != currentTimeout_) {
        DWORD t = timeoutMs < 0 ? 0 : static_cast<DWORD>(timeoutMs);
        setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof t);
        currentTimeout_ = timeoutMs;
    }
    uint8_t header[5];
    if (!readExact(header, sizeof header)) {
        return false;
    }
    uint32_t length = 0;
    std::memcpy(&length, header, 4);
    if (length == 0 || length > kMaxMessage) {
        return false;
    }
    type = static_cast<Msg>(header[4]);
    payload.resize(length - 1);
    return payload.empty() || readExact(payload.data(), payload.size());
}

}
