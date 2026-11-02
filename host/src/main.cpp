#include <winsock2.h>
#include <windows.h>
#include <timeapi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

#include "capture.h"
#include "display.h"
#include "engine.h"
#include "encoder.h"
#include "stats.h"

using namespace td;

namespace {

std::atomic<bool> g_quit{false};
Engine* g_engine = nullptr;

void usage() {
    std::printf(
        "tabdisplay-host [options]\n"
        "  --list                 list monitors and exit\n"
        "  --monitor <name>       \\\\.\\DISPLAYn or part of the monitor name (default: first VDD/virtual monitor)\n"
        "  --codec auto|hevc|h264 codec choice (default auto)\n"
        "  --encoder auto|nvenc|mf  video encoder (default auto: NVENC on NVIDIA, Media Foundation otherwise)\n"
        "  --fps <n>              target frame rate (default: monitor refresh rate)\n"
        "  --bitrate <mbps>       bitrate in Mbps (default 80)\n"
        "  --inflight <n>         frames allowed without ack (default 3)\n"
        "  --force                ignore decoder limits reported by the tablet\n"
        "  --serial <id>          adb device serial\n"
        "  --no-adb               do not use adb (port forwarded by hand)\n"
        "  --port <n>             TCP port (default 27183)\n"
        "  --keep-monitor         do not attach/detach the virtual monitor automatically\n"
        "  --no-launch            do not open the tablet app automatically\n"
        "  --monitor-on           attach the virtual monitor and exit\n"
        "  --monitor-off          detach the virtual monitor and exit\n"
        "  --bench <seconds>      capture and encode locally without a tablet\n"
        "  --stress               with --bench: encode at the target rate even when the screen is static\n"
        "  --log-input            print touch and pen events received from the tablet\n"
        "  --no-cursor-restore    leave the mouse cursor where the tablet tap moved it\n"
        "  --dump <file>          with --bench: write the encoded stream to a file\n");
}

int g_monitorCommand = 0;

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
            o.logInput = true;
        } else if (a == L"--no-cursor-restore") {
            o.noCursorRestore = true;
        } else if (a == L"--keep-monitor") {
            o.manageMonitor = false;
        } else if (a == L"--no-launch") {
            o.launchApp = false;
        } else if (a == L"--monitor-on") {
            g_monitorCommand = 1;
        } else if (a == L"--monitor-off") {
            g_monitorCommand = -1;
        } else if (a == L"--monitor" && next(v)) {
            o.monitor = v;
        } else if (a == L"--serial" && next(v)) {
            o.serial = v;
        } else if (a == L"--codec" && next(v)) {
            o.codec = lower(v);
        } else if (a == L"--encoder" && next(v)) {
            o.encoder = lower(v);
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
    bool codecOk = o.codec == L"auto" || o.codec == L"hevc" || o.codec == L"h264";
    bool encoderOk = o.encoder == L"auto" || o.encoder == L"nvenc" || o.encoder == L"mf";
    return codecOk && encoderOk;
}

void printOutputs(const std::vector<OutputInfo>& outputs) {
    for (const OutputInfo& o : outputs) {
        std::printf("%s  %ux%u @ %u Hz  at (%ld,%ld)  \"%s\"  on %s\n", narrow(o.gdiName).c_str(), o.width, o.height, o.refreshHz,
                    o.desktop.left, o.desktop.top, narrow(o.friendlyName).c_str(), narrow(o.adapterName).c_str());
    }
}

int runBench(const Options& o) {
    std::vector<OutputInfo> outputs = listOutputs();
    const OutputInfo* out = selectOutput(outputs, o);
    if (!out) {
        std::fprintf(stderr, "no matching monitor; use --monitor. Available:\n");
        printOutputs(outputs);
        return 1;
    }
    Capture capture;
    if (!capture.open(*out)) {
        return 1;
    }
    ComPtr<ID3D11Texture2D> frame;
    if (!makeFrameTexture(capture.device(), capture.width(), capture.height(), frame)) {
        std::fprintf(stderr, "bench: texture creation failed\n");
        return 1;
    }
    EncoderParams p;
    p.codec = o.codec == L"h264" ? Codec::H264 : Codec::HEVC;
    p.width = capture.width();
    p.height = capture.height();
    p.fps = o.fps ? o.fps : (out->refreshHz ? out->refreshHz : 60);
    p.bitrateKbps = o.bitrateKbps;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::unique_ptr<VideoEncoder> encoder = openBestEncoder(capture.device(), frame.Get(), out->vendorId, o, nullptr, p);
    if (!encoder) {
        std::fprintf(stderr, "bench: no hardware encoder available\n");
        return 1;
    }
    VideoEncoder& enc = *encoder;
    std::printf("bench: %s %s %ux%u @ %u fps, %u Mbps, %d s%s\n", enc.name(), p.codec == Codec::HEVC ? "HEVC" : "H.264", p.width, p.height,
                p.fps, p.bitrateKbps / 1000, o.benchSeconds, o.stress ? ", stress" : "");
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

BOOL WINAPI onCtrl(DWORD) {
    g_quit = true;
    if (g_engine) {
        g_engine->stop();
    }
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
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

    if (o.list) {
        printOutputs(listOutputs());
        return 0;
    }
    if (g_monitorCommand != 0) {
        std::wstring error;
        bool ok = setVirtualMonitorActive(monitorMatch(o), g_monitorCommand > 0, error);
        std::printf("%s: %s\n", g_monitorCommand > 0 ? "attach" : "detach", ok ? "ok" : narrow(error).c_str());
        return ok ? 0 : 1;
    }
    if (o.benchSeconds > 0) {
        SetConsoleCtrlHandler(onCtrl, TRUE);
        timeBeginPeriod(1);
        int rc = runBench(o);
        timeEndPeriod(1);
        return rc;
    }
    if (!acquireSingleInstance()) {
        std::fprintf(stderr, "another tabdisplay-host is already running; close it first\n");
        return 1;
    }
    Engine engine(o);
    g_engine = &engine;
    SetConsoleCtrlHandler(onCtrl, TRUE);
    engine.start();
    while (!g_quit) {
        Sleep(200);
    }
    engine.stop();
    g_engine = nullptr;
    return 0;
}
