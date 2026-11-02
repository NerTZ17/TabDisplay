#include "stats.h"

#include <windows.h>

#include <cstdio>

namespace td {

uint64_t nowUs() {
    static const int64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<uint64_t>((c.QuadPart / freq) * 1000000 + (c.QuadPart % freq) * 1000000 / freq);
}

void Stats::encoded(uint64_t encodeUs, uint64_t bytes) {
    encoded_++;
    bytes_ += bytes;
    encodeUs_ += encodeUs;
    uint64_t prev = encodeMaxUs_.load();
    while (encodeUs > prev && !encodeMaxUs_.compare_exchange_weak(prev, encodeUs)) {
    }
}

void Stats::acked(uint64_t rttUs, uint64_t decodeUs) {
    acks_++;
    rttUs_ += rttUs;
    decodeUs_ += decodeUs;
}

void Stats::report(const char* label, int inFlight) {
    uint64_t now = nowUs();
    if (lastReport_ == 0) {
        lastReport_ = now;
        return;
    }
    if (now - lastReport_ < 1000000) {
        return;
    }
    double seconds = static_cast<double>(now - lastReport_) / 1e6;
    lastReport_ = now;
    uint32_t cap = captured_.exchange(0);
    uint32_t skip = skipped_.exchange(0);
    uint32_t enc = encoded_.exchange(0);
    uint32_t acks = acks_.exchange(0);
    uint64_t bytes = bytes_.exchange(0);
    uint64_t encUs = encodeUs_.exchange(0);
    uint64_t encMax = encodeMaxUs_.exchange(0);
    uint64_t rtt = rttUs_.exchange(0);
    uint64_t dec = decodeUs_.exchange(0);
    ackFps_ = acks / seconds;
    rttMs_ = acks ? rtt / 1000.0 / acks : 0.0;
    mbps_ = bytes * 8.0 / 1e6 / seconds;
    std::printf("[%s] cap %5.1f fps | enc %5.1f fps avg %5.2f ms max %5.2f ms | %6.1f Mbps %6.1f KB/f | skip %3u | inflight %d",
                label, cap / seconds, enc / seconds, enc ? encUs / 1000.0 / enc : 0.0, encMax / 1000.0,
                bytes * 8.0 / 1e6 / seconds, enc ? bytes / 1024.0 / enc : 0.0, skip, inFlight);
    if (acks) {
        std::printf(" | ack %5.1f fps rtt %5.2f ms decode %5.2f ms", acks / seconds, rtt / 1000.0 / acks, dec / 1000.0 / acks);
    }
    std::printf("\n");
    std::fflush(stdout);
}

}
