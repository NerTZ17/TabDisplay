#include <winsock2.h>
#include <windows.h>
#include <timeapi.h>

#include "engine.h"

#include <algorithm>
#include <cstdio>
#include <cwctype>

#include <objbase.h>

#include "adb.h"
#include "display.h"
#include "encoder.h"
#include "inject.h"
#include "net.h"
#include "stats.h"

namespace td {

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

std::wstring monitorMatch(const Options& o) {
    if (!o.monitor.empty() && o.monitor.rfind(L"\\\\.\\", 0) != 0) {
        return o.monitor;
    }
    return L"VDD";
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
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    tex.Reset();
    return SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &tex));
}

bool acquireSingleInstance() {
    HANDLE instance = CreateMutexW(nullptr, TRUE, L"Local\\TabDisplayHost");
    if (!instance) {
        return true;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(instance);
        return false;
    }
    return true;
}

namespace {

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
    }

private:
    uint16_t active_ = 0;
    uint8_t lastFlags_ = 0;
};

struct SessionShared {
    Connection& conn;
    Injector& injector;
    Stats& stats;
    std::atomic<int> inFlight{0};
    std::atomic<bool> keyRequest{true};
    std::atomic<bool> alive{true};
    std::atomic<uint64_t> lastAck{0};
    std::atomic<uint64_t> lastSeen{0};
    bool logInput = false;
};

void receiveLoop(SessionShared& s) {
    Msg type;
    std::vector<uint8_t> payload;
    std::vector<TouchPoint> points;
    InputLog log;
    while (s.alive) {
        if (!s.conn.receive(type, payload, -1)) {
            break;
        }
        s.lastSeen = nowUs();
        Reader r(payload.data(), payload.size());
        switch (type) {
            case Msg::FrameAck: {
                r.u32();
                uint64_t hostTime = r.u64();
                uint32_t decodeUs = r.u32();
                if (!r.ok()) {
                    break;
                }
                int v = s.inFlight.load();
                while (v > 0 && !s.inFlight.compare_exchange_weak(v, v - 1)) {
                }
                uint64_t now = nowUs();
                s.lastAck = now;
                s.stats.acked(now > hostTime ? now - hostTime : 0, decodeUs);
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
                    if (s.logInput) {
                        log.touch(points);
                    }
                    s.injector.touch(points);
                }
                break;
            }
            case Msg::Pen: {
                PenSample p{};
                p.flags = r.u8();
                p.x = r.u16();
                p.y = r.u16();
                p.pressure = r.u16();
                p.tiltX = r.i8();
                p.tiltY = r.i8();
                p.rotation = r.u16();
                if (r.ok()) {
                    if (s.logInput) {
                        log.pen(p);
                    }
                    s.injector.pen(p);
                }
                break;
            }
            case Msg::RequestKeyframe:
                s.inFlight = 0;
                s.keyRequest = true;
                break;
            default:
                break;
        }
    }
    s.alive = false;
}

}

std::unique_ptr<VideoEncoder> openBestEncoder(ID3D11Device* device, ID3D11Texture2D* frame, uint32_t vendorId, const Options& o,
                                              const ClientHello* hello, EncoderParams& params) {
    std::vector<EncoderKind> kinds;
    if (o.encoder == L"nvenc") {
        kinds = {EncoderKind::Nvenc};
    } else if (o.encoder == L"mf") {
        kinds = {EncoderKind::MediaFoundation};
    } else if (vendorId == 0x10DE) {
        kinds = {EncoderKind::Nvenc, EncoderKind::MediaFoundation};
    } else {
        kinds = {EncoderKind::MediaFoundation};
    }
    std::vector<Codec> codecs = {params.codec};
    if (o.codec == L"auto" && params.codec == Codec::HEVC) {
        codecs.push_back(Codec::H264);
    }
    for (EncoderKind kind : kinds) {
        for (Codec codec : codecs) {
            EncoderParams p = params;
            p.codec = codec;
            if (codec == Codec::H264 && params.codec != Codec::H264 && hello && !o.force) {
                const CodecCaps* avc = findCaps(*hello, Codec::H264);
                if (avc && avc->maxFps > 0 && avc->maxFps < p.fps) {
                    p.fps = avc->maxFps;
                }
            }
            std::unique_ptr<VideoEncoder> enc = createEncoder(kind);
            if (enc->open(device, frame, p)) {
                params = p;
                std::printf("encoder: %s %s\n", enc->name(), codec == Codec::HEVC ? "HEVC" : "H.264");
                return enc;
            }
            std::printf("encoder: %s %s unavailable: %s\n", enc->name(), codec == Codec::HEVC ? "HEVC" : "H.264", enc->error().c_str());
        }
    }
    return nullptr;
}

Engine::Engine(Options options) : o_(std::move(options)) {}

Engine::~Engine() { stop(); }

void Engine::start() {
    if (thread_.joinable()) {
        return;
    }
    stop_ = false;
    thread_ = std::thread([this] { run(); });
}

void Engine::stop() {
    stop_ = true;
    if (thread_.joinable()) {
        thread_.join();
    }
}

EngineStatus Engine::status() const {
    std::lock_guard<std::mutex> lock(statusMutex_);
    return status_;
}

void Engine::setState(EngineState state, const std::wstring& detail) {
    std::lock_guard<std::mutex> lock(statusMutex_);
    if (status_.state != state || status_.detail != detail) {
        std::printf("[state] %d %s\n", static_cast<int>(state), narrow(detail).c_str());
        std::fflush(stdout);
    }
    status_.state = state;
    status_.detail = detail;
    if (state != EngineState::Streaming) {
        status_.ackFps = 0;
        status_.rttMs = 0;
        status_.mbps = 0;
    }
}

void Engine::setMonitor(bool active) {
    if (!o_.manageMonitor) {
        return;
    }
    MonitorState current = virtualMonitorState(monitorMatch(o_));
    if (current == MonitorState::Missing) {
        return;
    }
    if ((current == MonitorState::Active) == active) {
        if (active && !monitorAttachedByUs_) {
            monitorAttachedByUs_ = true;
        }
        return;
    }
    if (!active) {
        releaseCapture();
    }
    std::wstring error;
    if (setVirtualMonitorActive(monitorMatch(o_), active, error)) {
        std::printf("[display] virtual monitor %s\n", active ? "attached" : "detached");
        monitorAttachedByUs_ = active;
        if (active) {
            Sleep(1500);
        }
    } else {
        std::printf("[display] %s failed: %s\n", active ? "attach" : "detach", narrow(error).c_str());
    }
    std::fflush(stdout);
}

void Engine::releaseCapture() {
    frame_.Reset();
    capture_.close();
    captureOpen_ = false;
}

void Engine::run() {
    inRun_ = true;
    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    timeBeginPeriod(1);
    {
        NetInit net;
        loop();
    }
    releaseCapture();
    if (o_.manageMonitor) {
        setMonitor(false);
    }
    timeEndPeriod(1);
    if (SUCCEEDED(com)) {
        CoUninitialize();
    }
    setState(EngineState::Off);
    inRun_ = false;
}

void Engine::loop() {
    if (!o_.noAdb) {
        adb_ = findAdb();
        if (adb_.empty()) {
            setState(EngineState::Error, L"adb.exe not found: put Android platform-tools next to TabDisplay.exe");
            while (!stop_) {
                Sleep(200);
            }
            return;
        }
    }
    bool waitingShown = false;
    std::string lastAdbMessage;
    while (!stop_) {
        if (!o_.noAdb) {
            std::string state = adbState(adb_, o_.serial);
            if (state != "device") {
                launchedForDevice_ = false;
                releaseCapture();
                setMonitor(false);
                if (state == "offline" || state == "timeout") {
                    setState(EngineState::TabletProblem, L"adb not responding: toggle USB debugging on the tablet");
                } else if (state == "unauthorized") {
                    setState(EngineState::TabletProblem, L"Allow USB debugging on the tablet");
                } else if (state == "multiple") {
                    setState(EngineState::TabletProblem, L"More than one Android device connected");
                } else {
                    setState(EngineState::NoTablet);
                }
                for (int i = 0; i < 10 && !stop_; ++i) {
                    Sleep(100);
                }
                continue;
            }
        }
        setMonitor(true);
        if (stop_) {
            break;
        }
        std::vector<OutputInfo> outputs = listOutputs();
        const OutputInfo* out = selectOutput(outputs, o_);
        if (!out) {
            setState(EngineState::Error, L"Virtual monitor not found: install Virtual Display Driver");
            Sleep(1000);
            continue;
        }
        bool sameOutput = captureOpen_ && output_.gdiName == out->gdiName && output_.width == out->width && output_.height == out->height;
        if (!sameOutput) {
            releaseCapture();
            output_ = *out;
            if (!capture_.open(output_) || !makeFrameTexture(capture_.device(), capture_.width(), capture_.height(), frame_)) {
                releaseCapture();
                setState(EngineState::Error, L"Cannot capture the virtual monitor");
                Sleep(1000);
                continue;
            }
            captureOpen_ = true;
            std::printf("monitor %s \"%s\" %ux%u @ %u Hz\n", narrow(output_.gdiName).c_str(), narrow(output_.friendlyName).c_str(),
                        output_.width, output_.height, output_.refreshHz);
        }
        if (!o_.noAdb) {
            std::string output;
            if (!adbForward(adb_, o_.serial, o_.port, output)) {
                if (output != lastAdbMessage) {
                    std::printf("adb forward failed: %s\n", output.empty() ? "(no output)" : output.c_str());
                    lastAdbMessage = output;
                }
                Sleep(1000);
                continue;
            }
            lastAdbMessage.clear();
        }
        if (!streamSession()) {
            if (!waitingShown) {
                std::printf("waiting for the tablet app on port %u...\n", o_.port);
                waitingShown = true;
            }
            if (!o_.noAdb && o_.launchApp && !launchedForDevice_) {
                setState(EngineState::OpeningApp);
                launchedForDevice_ = adbLaunchApp(adb_, o_.serial);
            } else {
                setState(EngineState::OpeningApp, L"Open TabDisplay on the tablet");
            }
            for (int i = 0; i < 10 && !stop_; ++i) {
                Sleep(100);
            }
            continue;
        }
        waitingShown = false;
        launchedForDevice_ = true;
    }
}

bool Engine::streamSession() {
    Connection conn;
    ClientHello hello;
    if (!conn.connect(o_.port) || !handshake(conn, hello)) {
        return false;
    }
    std::printf("tablet: %ux%u, max %u Hz\n", hello.width, hello.height, hello.maxRefreshHz);
    for (const CodecCaps& c : hello.codecs) {
        std::printf("  %s: max %u fps at native, max size %ux%u%s%s\n", c.codec == Codec::HEVC ? "HEVC " : "H.264", c.maxFps, c.maxWidth,
                    c.maxHeight, (c.flags & kCapsHardware) ? ", hardware" : "", (c.flags & kCapsLowLatency) ? ", low-latency" : "");
    }
    if (hello.width != capture_.width() || hello.height != capture_.height()) {
        std::printf("note: monitor is %ux%u but the tablet panel is %ux%u\n", capture_.width(), capture_.height(), hello.width, hello.height);
    }
    EncoderParams params = chooseStream(hello, o_, output_.refreshHz, capture_.width(), capture_.height());
    std::unique_ptr<VideoEncoder> encoder = openBestEncoder(capture_.device(), frame_.Get(), output_.vendorId, o_, &hello, params);
    if (!encoder) {
        setState(EngineState::Error, L"No hardware video encoder available on this GPU");
        Sleep(1000);
        return true;
    }
    VideoEncoder& enc = *encoder;
    std::printf("streaming %s %ux%u @ %u fps, %u Mbps, inflight %d\n", params.codec == Codec::HEVC ? "HEVC" : "H.264", params.width,
                params.height, params.fps, params.bitrateKbps / 1000, o_.maxInFlight);
    std::fflush(stdout);
    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        status_.width = params.width;
        status_.height = params.height;
        status_.fps = params.fps;
        status_.hevc = params.codec == Codec::HEVC;
        status_.encoder = enc.name();
    }
    setState(EngineState::Streaming);
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
    injector.open(output_.gdiName);
    injector.setRestoreCursor(!o_.noCursorRestore);
    if (!lastCursor_.bgra.empty()) {
        CursorUpdate replay = lastCursor_;
        replay.shapeChanged = true;
        replay.positionChanged = true;
        sendCursor(conn, replay);
    }
    Stats stats;
    SessionShared shared{conn, injector, stats};
    shared.lastAck = nowUs();
    shared.lastSeen = nowUs();
    shared.logInput = o_.logInput;
    std::thread receiver(receiveLoop, std::ref(shared));
    uint64_t interval = 1000000ull / params.fps;
    uint64_t nextDue = 0;
    uint64_t lastHeartbeat = 0;
    uint32_t frameId = 1;
    bool pending = true;
    while (shared.alive && !stop_) {
        CursorUpdate cursor;
        Capture::Result r = capture_.acquire(pending ? 1 : 4, frame_.Get(), cursor);
        if (r == Capture::Result::Frame) {
            stats.captured();
            if (pending) {
                stats.skipped();
            }
            pending = true;
        } else if (r == Capture::Result::Lost) {
            uint32_t w = capture_.width();
            uint32_t h = capture_.height();
            if (!capture_.reopen() || w != capture_.width() || h != capture_.height()) {
                std::printf("monitor changed, restarting session\n");
                releaseCapture();
                break;
            }
            injector.refresh();
            pending = true;
            shared.keyRequest = true;
        } else if (r == Capture::Result::Error) {
            Sleep(2);
        }
        if (cursor.shapeChanged) {
            lastCursor_.bgra = cursor.bgra;
            lastCursor_.width = cursor.width;
            lastCursor_.height = cursor.height;
            lastCursor_.hotX = cursor.hotX;
            lastCursor_.hotY = cursor.hotY;
        }
        if (cursor.positionChanged) {
            lastCursor_.visible = cursor.visible;
            lastCursor_.x = cursor.x;
            lastCursor_.y = cursor.y;
        }
        sendCursor(conn, cursor);
        injector.tick();
        uint64_t now = nowUs();
        if (shared.inFlight >= o_.maxInFlight && now - shared.lastAck > 500000) {
            shared.inFlight = 0;
            shared.keyRequest = true;
        }
        if (shared.keyRequest && !pending) {
            pending = true;
        }
        if (pending && shared.inFlight < o_.maxInFlight && now + 250 >= nextDue) {
            bool idr = false;
            uint64_t t0 = nowUs();
            if (!enc.encode(shared.keyRequest.exchange(false), t0, bits, idr)) {
                std::fprintf(stderr, "encoder: %s\n", enc.error().c_str());
                break;
            }
            uint64_t took = nowUs() - t0;
            pending = false;
            if (bits.empty()) {
                continue;
            }
            if (!conn.send(videoMessage(frameId++, t0, idr ? kFrameIdr : 0, bits))) {
                break;
            }
            shared.inFlight++;
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
        if (tickNow > shared.lastSeen + kTimeoutUs) {
            std::printf("tablet silent for %llu s, reconnecting\n", static_cast<unsigned long long>(kTimeoutUs / 1000000));
            break;
        }
        stats.report("stream", shared.inFlight.load());
        {
            std::lock_guard<std::mutex> lock(statusMutex_);
            status_.ackFps = stats.ackFps();
            status_.rttMs = stats.rttMs();
            status_.mbps = stats.mbps();
        }
    }
    shared.alive = false;
    conn.close();
    receiver.join();
    injector.close();
    encoder.reset();
    std::printf("session ended\n");
    std::fflush(stdout);
    return true;
}

}
