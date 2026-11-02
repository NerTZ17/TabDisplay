#pragma once

#include <windows.h>

#include <array>
#include <mutex>
#include <string>
#include <vector>

#include "protocol.h"

namespace td {

class Injector {
public:
    ~Injector();

    bool open(const std::wstring& gdiName);
    void close();
    void refresh();
    void touch(const std::vector<TouchPoint>& points);
    void pen(const PenSample& sample);
    void releaseAll();

private:
    POINT map(uint16_t x, uint16_t y) const;
    void refreshLocked(bool force);

    std::wstring gdiName_;
    bool valid_ = false;
    ULONGLONG lastRefresh_ = 0;
    HSYNTHETICPOINTERDEVICE touchDevice_ = nullptr;
    HSYNTHETICPOINTERDEVICE penDevice_ = nullptr;
    RECT rect_{};
    POINT origin_{};
    std::array<bool, 10> active_{};
    std::array<POINT, 10> last_{};
    bool penInRange_ = false;
    bool penContact_ = false;
    POINT penLast_{};
    std::mutex mutex_;
};

}
