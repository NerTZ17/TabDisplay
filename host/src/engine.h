#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "capture.h"
#include "protocol.h"

namespace td {

struct Options {
    std::wstring monitor;
    std::wstring serial;
    std::wstring codec = L"auto";
    std::wstring encoder = L"auto";
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
    bool logInput = false;
    bool manageMonitor = true;
    bool launchApp = true;
};

enum class EngineState { Off, Stopping, NoTablet, TabletProblem, OpeningApp, Streaming, Error };

struct EngineStatus {
    EngineState state = EngineState::Off;
    std::wstring detail;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps = 0;
    bool hevc = true;
    std::string encoder;
    double ackFps = 0;
    double rttMs = 0;
    double mbps = 0;
};

std::wstring lower(std::wstring s);
std::string narrow(const std::wstring& w);
std::wstring monitorMatch(const Options& o);
const OutputInfo* selectOutput(const std::vector<OutputInfo>& outputs, const Options& o);
bool makeFrameTexture(ID3D11Device* device, uint32_t w, uint32_t h, ComPtr<ID3D11Texture2D>& tex);
bool acquireSingleInstance();

class VideoEncoder;
struct EncoderParams;
std::unique_ptr<VideoEncoder> openBestEncoder(ID3D11Device* device, ID3D11Texture2D* frame, uint32_t vendorId, const Options& o,
                                              const ClientHello* hello, EncoderParams& params);

class Engine {
public:
    explicit Engine(Options options);
    ~Engine();

    void start();
    void stop();
    void run();
    bool running() const { return thread_.joinable() || inRun_; }
    EngineStatus status() const;

private:
    void loop();
    bool streamSession();
    void setState(EngineState state, const std::wstring& detail = {});
    void setMonitor(bool active);
    void releaseCapture();

    Options o_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> inRun_{false};
    std::thread thread_;
    mutable std::mutex statusMutex_;
    EngineStatus status_;
    std::wstring adb_;
    Capture capture_;
    bool captureOpen_ = false;
    OutputInfo output_;
    ComPtr<ID3D11Texture2D> frame_;
    CursorUpdate lastCursor_;
    bool launchedForDevice_ = false;
    bool monitorAttachedByUs_ = false;
};

}
