#pragma once

#include <atomic>
#include <cstdint>

namespace td {

uint64_t nowUs();

class Stats {
public:
    void captured() { captured_++; }
    void skipped() { skipped_++; }
    void encoded(uint64_t encodeUs, uint64_t bytes);
    void acked(uint64_t rttUs, uint64_t decodeUs);
    void report(const char* label, int inFlight);
    double ackFps() const { return ackFps_.load(); }
    double rttMs() const { return rttMs_.load(); }
    double mbps() const { return mbps_.load(); }

private:
    std::atomic<uint32_t> captured_{0};
    std::atomic<uint32_t> skipped_{0};
    std::atomic<uint32_t> encoded_{0};
    std::atomic<uint32_t> acks_{0};
    std::atomic<uint64_t> bytes_{0};
    std::atomic<uint64_t> encodeUs_{0};
    std::atomic<uint64_t> encodeMaxUs_{0};
    std::atomic<uint64_t> rttUs_{0};
    std::atomic<uint64_t> decodeUs_{0};
    std::atomic<double> ackFps_{0};
    std::atomic<double> rttMs_{0};
    std::atomic<double> mbps_{0};
    uint64_t lastReport_ = 0;
};

}
