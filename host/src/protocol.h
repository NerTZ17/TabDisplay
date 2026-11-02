#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace td {

constexpr uint16_t kPort = 27183;
constexpr uint16_t kVersion = 1;
constexpr char kMagic[4] = {'T', 'D', 'S', 'P'};
constexpr uint32_t kMaxMessage = 16u * 1024u * 1024u;

enum class Msg : uint8_t {
    Hello = 1,
    ClientHello = 2,
    StreamConfig = 3,
    VideoFrame = 4,
    FrameAck = 5,
    Touch = 6,
    Pen = 7,
    RequestKeyframe = 8,
    CursorShape = 9,
    CursorPos = 10,
    Heartbeat = 11,
};

constexpr uint64_t kHeartbeatUs = 1000000;
constexpr uint64_t kTimeoutUs = 4000000;

enum class Codec : uint8_t { HEVC = 1, H264 = 2 };

constexpr uint8_t kFrameIdr = 1;
constexpr uint8_t kFrameCodecConfig = 2;

constexpr uint8_t kCapsHardware = 1;
constexpr uint8_t kCapsLowLatency = 2;

constexpr uint8_t kTouchContact = 1;
constexpr uint8_t kTouchCanceled = 2;

constexpr uint8_t kPenInRange = 1;
constexpr uint8_t kPenContact = 2;
constexpr uint8_t kPenBarrel = 4;
constexpr uint8_t kPenEraser = 8;

struct CodecCaps {
    Codec codec;
    uint8_t flags;
    uint16_t maxFps;
    uint16_t maxWidth;
    uint16_t maxHeight;
};

struct ClientHello {
    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t maxRefreshHz = 0;
    std::vector<CodecCaps> codecs;
};

struct TouchPoint {
    uint8_t id;
    uint8_t flags;
    uint16_t x;
    uint16_t y;
};

struct PenSample {
    uint8_t flags;
    uint16_t x;
    uint16_t y;
    uint16_t pressure;
    int8_t tiltX;
    int8_t tiltY;
    uint16_t rotation;
};

class Writer {
public:
    explicit Writer(Msg type, size_t reserve = 64) {
        buf_.reserve(reserve + 5);
        buf_.resize(4);
        buf_.push_back(static_cast<uint8_t>(type));
    }
    Writer& u8(uint8_t v) {
        buf_.push_back(v);
        return *this;
    }
    Writer& u16(uint16_t v) { return raw(&v, sizeof v); }
    Writer& i16(int16_t v) { return raw(&v, sizeof v); }
    Writer& u32(uint32_t v) { return raw(&v, sizeof v); }
    Writer& u64(uint64_t v) { return raw(&v, sizeof v); }
    Writer& raw(const void* p, size_t n) {
        auto b = static_cast<const uint8_t*>(p);
        buf_.insert(buf_.end(), b, b + n);
        return *this;
    }
    std::vector<uint8_t> finish() {
        uint32_t len = static_cast<uint32_t>(buf_.size() - 4);
        std::memcpy(buf_.data(), &len, 4);
        return std::move(buf_);
    }

private:
    std::vector<uint8_t> buf_;
};

class Reader {
public:
    Reader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    bool ok() const { return ok_; }
    size_t remaining() const { return ok_ ? n_ - off_ : 0; }
    uint8_t u8() { return get<uint8_t>(); }
    int8_t i8() { return get<int8_t>(); }
    uint16_t u16() { return get<uint16_t>(); }
    uint32_t u32() { return get<uint32_t>(); }
    uint64_t u64() { return get<uint64_t>(); }
    bool bytes(void* out, size_t k) {
        if (!ok_ || n_ - off_ < k) {
            ok_ = false;
            return false;
        }
        std::memcpy(out, p_ + off_, k);
        off_ += k;
        return true;
    }

private:
    template <typename T>
    T get() {
        T v{};
        bytes(&v, sizeof v);
        return v;
    }
    const uint8_t* p_;
    size_t n_;
    size_t off_ = 0;
    bool ok_ = true;
};

}
