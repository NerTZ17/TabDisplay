#include "inject.h"

#include <cstdio>

namespace td {

Injector::~Injector() { close(); }

bool Injector::open(const std::wstring& gdiName) {
    close();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        gdiName_ = gdiName;
        refreshLocked(true);
    }
    touchDevice_ = CreateSyntheticPointerDevice(PT_TOUCH, static_cast<ULONG>(active_.size()), POINTER_FEEDBACK_DEFAULT);
    penDevice_ = CreateSyntheticPointerDevice(PT_PEN, 1, POINTER_FEEDBACK_DEFAULT);
    if (!touchDevice_ || !penDevice_) {
        std::fprintf(stderr, "inject: CreateSyntheticPointerDevice failed (%lu)\n", GetLastError());
        return false;
    }
    return true;
}

void Injector::close() {
    if (touchDevice_ || penDevice_) {
        releaseAll();
        std::lock_guard<std::mutex> lock(mutex_);
        restoreLocked();
    }
    if (touchDevice_) {
        DestroySyntheticPointerDevice(touchDevice_);
        touchDevice_ = nullptr;
    }
    if (penDevice_) {
        DestroySyntheticPointerDevice(penDevice_);
        penDevice_ = nullptr;
    }
}

void Injector::refresh() {
    std::lock_guard<std::mutex> lock(mutex_);
    refreshLocked(true);
}

void Injector::refreshLocked(bool force) {
    ULONGLONG now = GetTickCount64();
    if (!force && now - lastRefresh_ < 250) {
        return;
    }
    lastRefresh_ = now;
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    if (!EnumDisplaySettingsExW(gdiName_.c_str(), ENUM_CURRENT_SETTINGS, &dm, 0) || dm.dmPelsWidth == 0 || dm.dmPelsHeight == 0) {
        if (valid_) {
            std::printf("[input] monitor %ls not active, input paused\n", gdiName_.c_str());
            std::fflush(stdout);
        }
        valid_ = false;
        return;
    }
    RECT rect{dm.dmPosition.x, dm.dmPosition.y, dm.dmPosition.x + static_cast<LONG>(dm.dmPelsWidth),
              dm.dmPosition.y + static_cast<LONG>(dm.dmPelsHeight)};
    POINT origin{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN)};
    if (!valid_ || !EqualRect(&rect, &rect_) || origin.x != origin_.x || origin.y != origin_.y) {
        std::printf("[input] mapping %ls at (%ld,%ld) %ldx%ld, virtual origin (%ld,%ld)\n", gdiName_.c_str(), rect.left, rect.top,
                    rect.right - rect.left, rect.bottom - rect.top, origin.x, origin.y);
        std::fflush(stdout);
    }
    rect_ = rect;
    origin_ = origin;
    valid_ = true;
}

POINT Injector::map(uint16_t x, uint16_t y) const {
    LONG w = rect_.right - rect_.left;
    LONG h = rect_.bottom - rect_.top;
    POINT p;
    p.x = rect_.left - origin_.x + static_cast<LONG>((static_cast<int64_t>(x) * (w - 1) + 32767) / 65535);
    p.y = rect_.top - origin_.y + static_cast<LONG>((static_cast<int64_t>(y) * (h - 1) + 32767) / 65535);
    return p;
}

void Injector::touch(const std::vector<TouchPoint>& points) {
    std::lock_guard<std::mutex> lock(mutex_);
    refreshLocked(false);
    if (!touchDevice_ || !valid_) {
        return;
    }
    std::vector<POINTER_TYPE_INFO> infos;
    infos.reserve(points.size());
    for (const TouchPoint& tp : points) {
        if (tp.id >= active_.size()) {
            continue;
        }
        bool contact = (tp.flags & kTouchContact) != 0;
        POINTER_FLAGS flags;
        POINT pt = contact ? map(tp.x, tp.y) : last_[tp.id];
        if (contact && !active_[tp.id]) {
            beginInteractionLocked();
        }
        if (contact) {
            flags = POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | (active_[tp.id] ? POINTER_FLAG_UPDATE : POINTER_FLAG_DOWN);
            active_[tp.id] = true;
            last_[tp.id] = pt;
        } else if (active_[tp.id]) {
            flags = POINTER_FLAG_UP;
            if (tp.flags & kTouchCanceled) {
                flags |= POINTER_FLAG_CANCELED;
            }
            active_[tp.id] = false;
        } else {
            continue;
        }
        POINTER_TYPE_INFO info{};
        info.type = PT_TOUCH;
        POINTER_TOUCH_INFO& ti = info.touchInfo;
        ti.pointerInfo.pointerType = PT_TOUCH;
        ti.pointerInfo.pointerId = tp.id;
        ti.pointerInfo.ptPixelLocation = pt;
        ti.pointerInfo.pointerFlags = flags;
        ti.touchFlags = TOUCH_FLAG_NONE;
        ti.touchMask = TOUCH_MASK_CONTACTAREA | TOUCH_MASK_ORIENTATION | TOUCH_MASK_PRESSURE;
        ti.rcContact = {pt.x - 2, pt.y - 2, pt.x + 2, pt.y + 2};
        ti.orientation = 90;
        ti.pressure = 32000;
        infos.push_back(info);
    }
    if (!infos.empty() && !InjectSyntheticPointerInput(touchDevice_, infos.data(), static_cast<UINT32>(infos.size()))) {
        std::fprintf(stderr, "inject: touch failed (%lu)\n", GetLastError());
    }
    endInteractionLocked();
}

bool Injector::interactingLocked() const {
    for (bool a : active_) {
        if (a) {
            return true;
        }
    }
    return penContact_;
}

void Injector::beginInteractionLocked() {
    if (!restoreCursor_) {
        return;
    }
    restoreAt_ = 0;
    if (!saved_) {
        saved_ = GetCursorPos(&savedCursor_) != FALSE;
    }
}

void Injector::endInteractionLocked() {
    if (saved_ && !interactingLocked()) {
        restoreAt_ = GetTickCount64() + 80;
    }
}

void Injector::restoreLocked() {
    restoreAt_ = 0;
    if (!saved_) {
        return;
    }
    saved_ = false;
    POINT now{};
    if (GetCursorPos(&now) && PtInRect(&rect_, now)) {
        SetCursorPos(savedCursor_.x, savedCursor_.y);
    }
}

void Injector::tick() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (restoreAt_ != 0 && GetTickCount64() >= restoreAt_) {
        restoreLocked();
    }
}

void Injector::setRestoreCursor(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    restoreCursor_ = enabled;
}

void Injector::pen(const PenSample& s) {
    std::lock_guard<std::mutex> lock(mutex_);
    refreshLocked(false);
    if (!penDevice_ || !valid_) {
        return;
    }
    bool inRange = (s.flags & (kPenInRange | kPenContact)) != 0;
    bool contact = (s.flags & kPenContact) != 0;
    POINT pt = inRange ? map(s.x, s.y) : penLast_;
    POINTER_FLAGS flags;
    if (!inRange) {
        if (!penInRange_) {
            return;
        }
        flags = penContact_ ? POINTER_FLAG_UP : POINTER_FLAG_UPDATE;
    } else if (contact && !penContact_) {
        flags = POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_FIRSTBUTTON;
    } else if (!contact && penContact_) {
        flags = POINTER_FLAG_UP | POINTER_FLAG_INRANGE;
    } else if (contact) {
        flags = POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_FIRSTBUTTON;
    } else {
        flags = POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE;
    }
    POINTER_TYPE_INFO info{};
    info.type = PT_PEN;
    POINTER_PEN_INFO& pi = info.penInfo;
    pi.pointerInfo.pointerType = PT_PEN;
    pi.pointerInfo.pointerId = 0;
    pi.pointerInfo.ptPixelLocation = pt;
    pi.pointerInfo.pointerFlags = flags;
    pi.penFlags = PEN_FLAG_NONE;
    if (s.flags & kPenBarrel) {
        pi.penFlags |= PEN_FLAG_BARREL;
    }
    if (s.flags & kPenEraser) {
        pi.penFlags |= contact ? PEN_FLAG_ERASER : PEN_FLAG_INVERTED;
    }
    pi.penMask = PEN_MASK_PRESSURE | PEN_MASK_ROTATION | PEN_MASK_TILT_X | PEN_MASK_TILT_Y;
    pi.pressure = contact ? (s.pressure > 1024 ? 1024u : s.pressure) : 0u;
    pi.rotation = s.rotation % 360;
    pi.tiltX = s.tiltX;
    pi.tiltY = s.tiltY;
    if (contact && !penContact_) {
        beginInteractionLocked();
    }
    if (!InjectSyntheticPointerInput(penDevice_, &info, 1)) {
        std::fprintf(stderr, "inject: pen failed (%lu)\n", GetLastError());
        penInRange_ = false;
        penContact_ = false;
        endInteractionLocked();
        return;
    }
    if (!inRange && penContact_) {
        penContact_ = false;
        info.penInfo.pointerInfo.pointerFlags = POINTER_FLAG_UPDATE;
        info.penInfo.pressure = 0;
        InjectSyntheticPointerInput(penDevice_, &info, 1);
    }
    penInRange_ = inRange;
    penContact_ = contact;
    penLast_ = pt;
    if (!contact) {
        endInteractionLocked();
    }
}

void Injector::releaseAll() {
    std::vector<TouchPoint> lift;
    for (uint8_t id = 0; id < active_.size(); ++id) {
        if (active_[id]) {
            lift.push_back({id, kTouchCanceled, 0, 0});
        }
    }
    if (!lift.empty()) {
        touch(lift);
    }
    if (penInRange_) {
        pen({0, 0, 0, 0, 0, 0, 0});
    }
}

}
