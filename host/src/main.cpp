#include <winsock2.h>
#include <windows.h>
#include <timeapi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cwctype>
#include <string>
#include <thread>
#include <vector>

#include "adb.h"
#include "capture.h"
#include "inject.h"
#include "net.h"
#include "nvenc.h"
#include "protocol.h"
#include "stats.h"

using namespace td;

namespace {

std::atomic<bool> g_quit{false};
std::atomic<bool> g_logInput{false};

struct Options {
    std::wstring monitor;
    std::wstring serial;
    std::wstring codec = L"auto";
    std::wstring dumpPath;
    uint32_t fps = 0;
    uint32_t bitrateKbps = 80000;
    int maxInFlight = 3;
    int benchSeconds = 0;
    uint16_t port = kPort;
    bool force = false;
    bool list = false;
    bool noAdb = false;
    bool stress = false;
    bool noCursorRestore = false;
};

std::wstring lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) {
        return {};
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

void usage() {
    std::printf(
        "tabdisplay-host [options]\n"
        "  --list                 list monitors and exit\n"
        "  --monitor <name>       \\\\.\\DISPLAYn or part of the monitor name (default: first VDD/virtual monitor)\n"
        "  --codec auto|hevc|h264 codec choice (default auto)\n"
        "  --fps <n>              target frame rate (default: monitor refresh rate)\n"
        "  --bitrate <mbps>       bitrate in Mbps (default 80)\n"
        "  --inflight <n>         frames allowed without ack (default 3)\n"
        "  --force                ignore decoder limits reported by the tablet\n"
        "  --serial <id>          adb device serial\n"
        "  --no-adb               do not run adb forward\n"
        "  --port <n>             TCP port (default 27183)\n"
        "  --bench <seconds>      capture and encode locally without a tablet\n"
        "  --stress               with --bench: encode at the target rate even when the screen is static\n"
        "  --log-input            print touch and pen events received from the tablet\n"
        "  --no-cursor-restore    leave the mouse cursor where the tablet tap moved it\n"
        "  --dump <file>          with --bench: write the encoded stream to a file\n");
}

bool parseArgs(int argc, wchar_t** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        auto next = [&](std::wstring& out) {
            if (i + 1 >= argc) {
                return false;
            }
            out = argv[++i];
            return true;
        };
        std::wstring v;
        if (a == L"--list") {
            o.list = true;
        } else if (a == L"--force") {
            o.force = true;
        } else if (a == L"--no-adb") {
            o.noAdb = true;
        } else if (a == L"--stress") {
            o.stress = true;
        } else if (a == L"--log-input") {
            g_logInput = true;
        } else if (a == L"--no-cursor-restore") {
            o.noCursorRestore = true;
        } else if (a == L"--monitor" && next(v)) {
            o.monitor = v;
        } else if (a == L"--serial" && next(v)) {
            o.serial = v;
        } else if (a == L"--codec" && next(v)) {
            o.codec = lower(v);
        } else if (a == L"--dump" && next(v)) {
            o.dumpPath = v;
        } else if (a == L"--fps" && next(v)) {
            o.fps = static_cast<uint32_t>(std::stoul(v));
        } else if (a == L"--bitrate" && next(v)) {
            o.bitrateKbps = static_cast<uint32_t>(std::stod(v) * 1000.0);
        } else if (a == L"--inflight" && next(v)) {
            o.maxInFlight = std::max(1, std::stoi(v));
        } else if (a == L"--bench" && next(v)) {
            o.benchSeconds = std::stoi(v);
        } else if (a == L"--port" && next(v)) {
            o.port = static_cast<uint16_t>(std::stoul(v));
        } else {
            return false;
        }
    }
    return o.codec == L"auto" || o.codec == L"hevc" || o.codec == L"h264";
}

void printOutputs(const std::vector<OutputInfo>& outputs) {
    for (const OutputInfo& o : outputs) {
        std::printf("%s  %ux%u @ %u Hz  at (%ld,%ld)  \"%s\"  on %s\n", narrow(o.gdiName).c_str(), o.width, o.height, o.refreshHz,
                    o.desktop.left, o.desktop.top, narrow(o.friendlyName).c_str(), narrow(o.adapterName).c_str());
    }
}

const OutputInfo* selectOutput(const std::vector<OutputInfo>& outputs, const Options& o) {
    for (const OutputInfo& out : outputs) {
        if (!o.monitor.empty()) {
            std::wstring m = lower(o.monitor);
            if (lower(out.gdiName) == m || lower(out.friendlyName).find(m) != std::wstring::npos) {
                return &out;
            }
            continue;
        }
        std::wstring f = lower(out.friendlyName);
        if (f.find(L"vdd") != std::wstring::npos || f.find(L"virtual") != std::wstring::npos) {
            return &out;
        }
    }
    return nullptr;
}

bool makeFrameTexture(ID3D11Device* device, uint32_t w, uint32_t h, ComPtr<ID3D11Texture2D>& tex) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    tex.Reset();
    return SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &tex));
}

const CodecCaps* findCaps(const ClientHello& hello, Codec codec) {
    for (const CodecCaps& c : hello.codecs) {
        if (c.codec == codec) {
            return &c;
        }
    }
    return nullptr;
}

bool parseClientHello(const std::vector<uint8_t>& payload, ClientHello& hello) {
    Reader r(payload.data(), payload.size());
    char magic[4];
    r.bytes(magic, 4);
    uint16_t version = r.u16();
    hello.width = r.u16();
    hello.height = r.u16();
    hello.maxRefreshHz = r.u16();
    uint8_t count = r.u8();
    for (uint8_t i = 0; i < count && r.ok(); ++i) {
        CodecCaps c{};
        c.codec = static_cast<Codec>(r.u8());
        c.flags = r.u8();
        c.maxFps = r.u16();
        c.maxWidth = r.u16();
        c.maxHeight = r.u16();
        hello.codecs.push_back(c);
    }
    return r.ok() && std::memcmp(magic, kMagic, 4) == 0 && version == kVersion;
}

EncoderParams chooseStream(const ClientHello& hello, const Options& o, uint32_t refreshHz, uint32_t w, uint32_t h) {
    EncoderParams p;
    p.width = w;
    p.height = h;
    p.bitrateKbps = o.bitrateKbps;
    uint32_t desired = o.fps ? o.fps : (refreshHz ? refreshHz : 60);
    if (!o.force && hello.maxRefreshHz) {
        desired = std::min<uint32_t>(desired, hello.maxRefreshHz);
    }
    auto usable = [&](const CodecCaps* c) { return c && c->maxWidth >= w && c->maxHeight >= h && c->maxFps > 0; };
    const CodecCaps* hevc = findCaps(hello, Codec::HEVC);
    const CodecCaps* avc = findCaps(hello, Codec::H264);
    const CodecCaps* chosen = nullptr;
    if (o.codec == L"hevc") {
        p.codec = Codec::HEVC;
        chosen = hevc;
    } else if (o.codec == L"h264") {
        p.codec = Codec::H264;
        chosen = avc;
    } else if (usable(hevc) && hevc->maxFps >= desired) {
        p.codec = Codec::HEVC;
        chosen = hevc;
    } else if (usable(avc) && avc->maxFps >= desired) {
        p.codec = Codec::H264;
        chosen = avc;
    } else if (usable(hevc) || usable(avc)) {
        bool pickHevc = usable(hevc) && (!usable(avc) || hevc->maxFps >= avc->maxFps);
        p.codec = pickHevc ? Codec::HEVC : Codec::H264;
        chosen = pickHevc ? hevc : avc;
    } else {
        p.codec = Codec::HEVC;
    }
    p.fps = desired;
    if (!o.force && usable(chosen) && chosen->maxFps < desired) {
        p.fps = chosen->maxFps;
        std::printf("decoder reports max %u fps at %ux%u, capping (use --force to override)\n", chosen->maxFps, w, h);
    }
    if (!usable(chosen)) {
        std::printf("decoder did not report support for %ux%u with this codec, trying anyway\n", w, h);
    }
    return p;
}

std::vector<uint8_t> videoMessage(uint32_t frameId, uint64_t hostTimeUs, uint8_t flags, const std::vector<uint8_t>& data) {
    return Writer(Msg::VideoFrame, data.size() + 13).u32(frameId).u64(hostTimeUs).u8(flags).raw(data.data(), data.size()).finish();
}

void sendCursor(Connection& conn, const CursorUpdate& c) {
    if (c.shapeChanged && !c.bgra.empty()) {
        conn.send(Writer(Msg::CursorShape, c.bgra.size() + 8)
                      .u16(c.width)
                      .u16(c.height)
                      .u16(c.hotX)
                      .u16(c.hotY)
                      .raw(c.bgra.data(), c.bgra.size())
                      .finish());
    }
    if (c.positionChanged) {
        int16_t x = static_cast<int16_t>(std::clamp(c.x, -32768, 32767));
        int16_t y = static_cast<int16_t>(std::clamp(c.y, -32768, 32767));
        conn.send(Writer(Msg::CursorPos).i16(x).i16(y).u8(c.visible ? 1 : 0).finish());
    }
}

int runBench(const Options& o, Capture& capture, const OutputInfo& out) {
    ComPtr<ID3D11Texture2D> frame;
    if (!makeFrameTexture(capture.device(), capture.width(), capture.height(), frame)) {
        std::fprintf(stderr, "bench: texture creation failed\n");
        return 1;
    }
    EncoderParams p;
    p.codec = o.codec == L"h264" ? Codec::H264 : Codec::HEVC;
    p.width = capture.width();
    p.height = capture.height();
    p.fps = o.fps ? o.fps : (out.refreshHz ? out.refreshHz : 60);
    p.bitrateKbps = o.bitrateKbps;
    NvEncoder enc;
    if (!enc.open(capture.device(), frame.Get(), p)) {
        std::fprintf(stderr, "bench: %s\n", enc.error().c_str());
        return 1;
    }
    std::printf("bench: %s %ux%u @ %u fps, %u Mbps, %d s%s\n", p.codec == Codec::HEVC ? "HEVC" : "H.264", p.width, p.height, p.fps,
                p.bitrateKbps / 1000, o.benchSeconds, o.stress ? ", stress" : "");
    FILE* dump = nullptr;
    if (!o.dumpPath.empty()) {
        if (_wfopen_s(&dump, o.dumpPath.c_str(), L"wb") != 0) {
            dump = nullptr;
        }
        std::vector<uint8_t> seq;
        if (dump && enc.sequenceParams(seq)) {
            std::fwrite(seq.data(), 1, seq.size(), dump);
        }
    }
    Stats stats;
    std::vector<uint8_t> bits;
    uint64_t interval = 1000000ull / p.fps;
    uint64_t end = nowUs() + static_cast<uint64_t>(o.benchSeconds) * 1000000ull;
    uint64_t lastEncode = 0;
    uint64_t totalFrames = 0;
    uint64_t totalEncodeUs = 0;
    bool pending = true;
    bool first = true;
    while (!g_quit && nowUs() < end) {
        CursorUpdate cursor;
        Capture::Result r = capture.acquire((pending || o.stress) ? 0 : 2, frame.Get(), cursor);
        if (r == Capture::Result::Frame) {
            stats.captured();
            pending = true;
        } else if (r == Capture::Result::Lost) {
            if (!capture.reopen()) {
                std::fprintf(stderr, "bench: duplication lost\n");
                return 1;
            }
        }
        uint64_t now = nowUs();
        bool due = now - lastEncode + 500 >= interval;
        if (due && (pending || o.stress)) {
            bool idr = false;
            uint64_t t0 = nowUs();
            if (!enc.encode(first, t0, bits, idr)) {
                std::fprintf(stderr, "bench: %s\n", enc.error().c_str());
                return 1;
            }
            uint64_t took = nowUs() - t0;
            first = false;
            pending = false;
            lastEncode = now;
            stats.encoded(took, bits.size());
            totalFrames++;
            totalEncodeUs += took;
            if (dump) {
                std::fwrite(bits.data(), 1, bits.size(), dump);
            }
        }
        stats.report("bench", 0);
    }
    if (dump) {
        std::fclose(dump);
    }
    std::printf("bench done: %llu frames, avg encode %.2f ms\n", static_cast<unsigned long long>(totalFrames),
                totalFrames ? totalEncodeUs / 1000.0 / totalFrames : 0.0);
    return 0;
}

bool handshake(Connection& conn, ClientHello& hello) {
    if (!conn.send(Writer(Msg::Hello).raw(kMagic, 4).u16(kVersion).finish())) {
        return false;
    }
    Msg type;
    std::vector<uint8_t> payload;
    if (!conn.receive(type, payload, 3000) || type != Msg::ClientHello) {
        return false;
    }
    return parseClientHello(payload, hello);
}

class InputLog {
public:
    void touch(const std::vector<TouchPoint>& points) {
        for (const TouchPoint& p : points) {
            if (p.id >= 16) {
                continue;
            }
            bool contact = (p.flags & kTouchContact) != 0;
            uint16_t bit = static_cast<uint16_t>(1u << p.id);
            if (contact && !(active_ & bit)) {
                std::printf("[touch] down id %u at %.3f,%.3f\n", p.id, p.x / 65535.0, p.y / 65535.0);
                active_ |= bit;
            } else if (!contact && (active_ & bit)) {
                std::printf("[touch] up   id %u%s\n", p.id, (p.flags & kTouchCanceled) ? " canceled" : "");
                active_ &= static_cast<uint16_t>(~bit);
            }
        }
        touchMessages_++;
        std::fflush(stdout);
    }

    void pen(const PenSample& s) {
        if (s.flags != lastFlags_) {
            std::printf("[pen] %s%s%s%s at %.3f,%.3f pressure %u tilt %d,%d rot %u\n", (s.flags & kPenInRange) ? "range " : "out ",
                        (s.flags & kPenContact) ? "contact " : "", (s.flags & kPenBarrel) ? "barrel " : "", (s.flags & kPenEraser) ? "eraser " : "",
                        s.x / 65535.0, s.y / 65535.0, s.pressure, s.tiltX, s.tiltY, s.rotation);
            lastFlags_ = s.flags;
            std::fflush(stdout);
        }
        penSamples_++;
        if (s.flags & kPenContact) {
            pressureMin_ = std::min<int>(pressureMin_, s.pressure);
            pressureMax_ = std::max<int>(pressureMax_, s.pressure);
        }
        tiltXMin_ = std::min<int>(tiltXMin_, s.tiltX);
        tiltXMax_ = std::max<int>(tiltXMax_, s.tiltX);
        tiltYMin_ = std::min<int>(tiltYMin_, s.tiltY);
        tiltYMax_ = std::max<int>(tiltYMax_, s.tiltY);
    }

    void flush(bool force) {
        uint64_t now = nowUs();
        if (!force && now - last_ < 1000000) {
            return;
        }
        last_ = now;
        if (penSamples_ || touchMessages_) {
            std::printf("[input] pen %u samples/s", penSamples_);
            if (pressureMax_ >= 0) {
                std::printf(" pressure %d..%d", pressureMin_, pressureMax_);
            }
            if (penSamples_) {
                std::printf(" tiltX %d..%d tiltY %d..%d", tiltXMin_, tiltXMax_, tiltYMin_, tiltYMax_);
            }
            std::printf(" | touch %u msg/s\n", touchMessages_);
            std::fflush(stdout);
        }
        penSamples_ = 0;
        touchMessages_ = 0;
        pressureMin_ = 1 << 30;
        pressureMax_ = -1;
        tiltXMin_ = tiltYMin_ = 127;
        tiltXMax_ = tiltYMax_ = -128;
    }

private:
    uint16_t active_ = 0;
    uint8_t lastFlags_ = 0;
    uint32_t penSamples_ = 0;
    uint32_t touchMessages_ = 0;
    int pressureMin_ = 1 << 30;
    int pressureMax_ = -1;
    int tiltXMin_ = 127;
    int tiltXMax_ = -128;
    int tiltYMin_ = 127;
    int tiltYMax_ = -128;
    uint64_t last_ = 0;
};

void receiveLoop(Connection& conn, Injector& injector, Stats& stats, std::atomic<int>& inFlight, std::atomic<bool>& keyRequest,
                 std::atomic<uint64_t>& lastAck, std::atomic<uint64_t>& lastSeen, std::atomic<bool>& alive) {
    Msg type;
    std::vector<uint8_t> payload;
    std::vector<TouchPoint> points;
    InputLog input;
    while (alive) {
        if (!conn.receive(type, payload, -1)) {
            break;
        }
        if (g_logInput) {
            input.flush(false);
        }
        lastSeen = nowUs();
        Reader r(payload.data(), payload.size());
        switch (type) {
            case Msg::FrameAck: {
                r.u32();
                uint64_t hostTime = r.u64();
                uint32_t decodeUs = r.u32();
                if (!r.ok()) {
                    break;
                }
                int v = inFlight.load();
                while (v > 0 && !inFlight.compare_exchange_weak(v, v - 1)) {
                }
                uint64_t now = nowUs();
                lastAck = now;
                stats.acked(now > hostTime ? now - hostTime : 0, decodeUs);
                break;
            }
            case Msg::Touch: {
                uint8_t count = r.u8();
                points.clear();
                for (uint8_t i = 0; i < count && r.ok(); ++i) {
                    TouchPoint tp{};
                    tp.id = r.u8();
                    tp.flags = r.u8();
                    tp.x = r.u16();
                    tp.y = r.u16();
                    points.push_back(tp);
                }
                if (r.ok()) {
                    if (g_logInput) {
                        input.touch(points);
                    }
                    injector.touch(points);
                }
                break;
            }
            case Msg::Pen: {
                PenSample s{};
                s.flags = r.u8();
                s.x = r.u16();
                s.y = r.u16();
                s.pressure = r.u16();
                s.tiltX = r.i8();
                s.tiltY = r.i8();
                s.rotation = r.u16();
                if (r.ok()) {
                    if (g_logInput) {
                        input.pen(s);
                    }
                    injector.pen(s);
                }
                break;
            }
            case Msg::RequestKeyframe:
                inFlight = 0;
                keyRequest = true;
                break;
            default:
                break;
        }
    }
    alive = false;
}

int runStream(const Options& o, Capture& capture, const OutputInfo& out) {
    HANDLE instance = CreateMutexW(nullptr, TRUE, L"Local\\TabDisplayHost");
    if (instance && GetLastError() == ERROR_ALREADY_EXISTS) {
        std::fprintf(stderr, "another tabdisplay-host is already running; close it first\n");
        CloseHandle(instance);
        return 1;
    }
    NetInit net;
    std::wstring adb;
    if (!o.noAdb) {
        adb = findAdb();
        if (adb.empty()) {
            std::fprintf(stderr, "adb.exe not found (PATH or %%LOCALAPPDATA%%\\Android\\Sdk\\platform-tools); use --no-adb if you forward the port yourself\n");
            return 1;
        }
    }
    ComPtr<ID3D11Texture2D> frame;
    if (!makeFrameTexture(capture.device(), capture.width(), capture.height(), frame)) {
        std::fprintf(stderr, "texture creation failed\n");
        return 1;
    }
    std::string lastAdbMessage;
    bool waitingShown = false;
    CursorUpdate lastCursor;
    while (!g_quit) {
        if (!o.noAdb) {
            std::string output;
            if (!adbForward(adb, o.serial, o.port, output)) {
                if (output != lastAdbMessage) {
                    std::printf("adb forward failed: %s\n", output.empty() ? "(no output)" : output.c_str());
                    lastAdbMessage = output;
                }
                Sleep(1000);
                continue;
            }
            lastAdbMessage.clear();
        }
        Connection conn;
        ClientHello hello;
        if (!conn.connect(o.port) || !handshake(conn, hello)) {
            if (!waitingShown) {
                std::printf("waiting for the tablet app on port %u...\n", o.port);
                waitingShown = true;
            }
            conn.close();
            Sleep(1000);
            continue;
        }
        waitingShown = false;
        std::printf("tablet: %ux%u, max %u Hz\n", hello.width, hello.height, hello.maxRefreshHz);
        for (const CodecCaps& c : hello.codecs) {
            std::printf("  %s: max %u fps at native, max size %ux%u%s%s\n", c.codec == Codec::HEVC ? "HEVC " : "H.264", c.maxFps, c.maxWidth,
                        c.maxHeight, (c.flags & kCapsHardware) ? ", hardware" : "", (c.flags & kCapsLowLatency) ? ", low-latency" : "");
        }
        if (hello.width != capture.width() || hello.height != capture.height()) {
            std::printf("note: monitor is %ux%u but the tablet panel is %ux%u; set the virtual monitor to %ux%u for 1:1 pixels\n",
                        capture.width(), capture.height(), hello.width, hello.height, hello.width, hello.height);
        }
        EncoderParams params = chooseStream(hello, o, out.refreshHz, capture.width(), capture.height());
        NvEncoder enc;
        if (!enc.open(capture.device(), frame.Get(), params)) {
            std::fprintf(stderr, "encoder: %s\n", enc.error().c_str());
            return 1;
        }
        std::printf("streaming %s %ux%u @ %u fps, %u Mbps, inflight %d\n", params.codec == Codec::HEVC ? "HEVC" : "H.264", params.width,
                    params.height, params.fps, params.bitrateKbps / 1000, o.maxInFlight);
        conn.send(Writer(Msg::StreamConfig)
                      .u8(static_cast<uint8_t>(params.codec))
                      .u16(static_cast<uint16_t>(params.width))
                      .u16(static_cast<uint16_t>(params.height))
                      .u16(static_cast<uint16_t>(params.fps))
                      .u32(params.bitrateKbps)
                      .finish());
        std::vector<uint8_t> bits;
        if (enc.sequenceParams(bits)) {
            conn.send(videoMessage(0, nowUs(), kFrameCodecConfig, bits));
        }
        Injector injector;
        injector.open(out.gdiName);
        injector.setRestoreCursor(!o.noCursorRestore);
        if (!lastCursor.bgra.empty()) {
            CursorUpdate replay = lastCursor;
            replay.shapeChanged = true;
            replay.positionChanged = true;
            sendCursor(conn, replay);
        }
        Stats stats;
        std::atomic<int> inFlight{0};
        std::atomic<bool> keyRequest{true};
        std::atomic<bool> alive{true};
        std::atomic<uint64_t> lastAck{nowUs()};
        std::atomic<uint64_t> lastSeen{nowUs()};
        uint64_t lastHeartbeat = 0;
        std::thread receiver(receiveLoop, std::ref(conn), std::ref(injector), std::ref(stats), std::ref(inFlight), std::ref(keyRequest),
                             std::ref(lastAck), std::ref(lastSeen), std::ref(alive));
        uint64_t interval = 1000000ull / params.fps;
        uint64_t nextDue = 0;
        uint32_t frameId = 1;
        bool pending = true;
        while (alive && !g_quit) {
            CursorUpdate cursor;
            Capture::Result r = capture.acquire(pending ? 1 : 4, frame.Get(), cursor);
            if (r == Capture::Result::Frame) {
                stats.captured();
                if (pending) {
                    stats.skipped();
                }
                pending = true;
            } else if (r == Capture::Result::Lost) {
                uint32_t w = capture.width();
                uint32_t h = capture.height();
                if (!capture.reopen()) {
                    std::fprintf(stderr, "desktop duplication lost\n");
                    g_quit = true;
                    break;
                }
                if (w != capture.width() || h != capture.height()) {
                    std::printf("monitor mode changed, restarting session\n");
                    makeFrameTexture(capture.device(), capture.width(), capture.height(), frame);
                    break;
                }
                injector.refresh();
                pending = true;
                keyRequest = true;
            } else if (r == Capture::Result::Error) {
                Sleep(2);
            }
            if (cursor.shapeChanged) {
                lastCursor.bgra = cursor.bgra;
                lastCursor.width = cursor.width;
                lastCursor.height = cursor.height;
                lastCursor.hotX = cursor.hotX;
                lastCursor.hotY = cursor.hotY;
            }
            if (cursor.positionChanged) {
                lastCursor.visible = cursor.visible;
                lastCursor.x = cursor.x;
                lastCursor.y = cursor.y;
            }
            sendCursor(conn, cursor);
            injector.tick();
            uint64_t now = nowUs();
            if (inFlight >= o.maxInFlight && now - lastAck > 500000) {
                inFlight = 0;
                keyRequest = true;
            }
            if (keyRequest && !pending) {
                pending = true;
            }
            if (pending && inFlight < o.maxInFlight && now + 250 >= nextDue) {
                bool idr = false;
                uint64_t t0 = nowUs();
                if (!enc.encode(keyRequest.exchange(false), t0, bits, idr)) {
                    std::fprintf(stderr, "encoder: %s\n", enc.error().c_str());
                    break;
                }
                uint64_t took = nowUs() - t0;
                if (!conn.send(videoMessage(frameId++, t0, idr ? kFrameIdr : 0, bits))) {
                    break;
                }
                inFlight++;
                nextDue = now > nextDue + interval ? now + interval : nextDue + interval;
                pending = false;
                stats.encoded(took, bits.size());
            }
            uint64_t tickNow = nowUs();
            if (tickNow - lastHeartbeat >= kHeartbeatUs) {
                lastHeartbeat = tickNow;
                if (!conn.send(Writer(Msg::Heartbeat).finish())) {
                    break;
                }
            }
            if (tickNow > lastSeen + kTimeoutUs) {
                std::printf("tablet silent for %llu s, reconnecting\n", static_cast<unsigned long long>(kTimeoutUs / 1000000));
                break;
            }
            stats.report("stream", inFlight.load());
        }
        alive = false;
        conn.close();
        receiver.join();
        injector.close();
        enc.close();
        std::printf("session ended\n");
    }
    return 0;
}

BOOL WINAPI onCtrl(DWORD) {
    g_quit = true;
    return TRUE;
}

}

int wmain(int argc, wchar_t** argv) {
    Options o;
    if (!parseArgs(argc, argv, o)) {
        usage();
        return 2;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetConsoleCtrlHandler(onCtrl, TRUE);
    timeBeginPeriod(1);
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

    std::vector<OutputInfo> outputs = listOutputs();
    if (o.list) {
        printOutputs(outputs);
        return 0;
    }
    const OutputInfo* out = selectOutput(outputs, o);
    if (!out) {
        std::fprintf(stderr, "no matching monitor; use --monitor. Available:\n");
        printOutputs(outputs);
        return 1;
    }
    if (out->vendorId != 0x10DE) {
        std::fprintf(stderr, "monitor %s is on %s, NVENC needs it on the NVIDIA GPU\n", narrow(out->gdiName).c_str(),
                     narrow(out->adapterName).c_str());
        return 1;
    }
    std::printf("monitor %s \"%s\" %ux%u @ %u Hz\n", narrow(out->gdiName).c_str(), narrow(out->friendlyName).c_str(), out->width,
                out->height, out->refreshHz);
    Capture capture;
    if (!capture.open(*out)) {
        return 1;
    }
    int rc = o.benchSeconds > 0 ? runBench(o, capture, *out) : runStream(o, capture, *out);
    timeEndPeriod(1);
    return rc;
}
