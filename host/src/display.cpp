#include "display.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <vector>

namespace td {

namespace {

struct Config {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
};

bool query(UINT32 flags, Config& c) {
    for (int attempt = 0; attempt < 4; ++attempt) {
        UINT32 pathCount = 0;
        UINT32 modeCount = 0;
        if (GetDisplayConfigBufferSizes(flags, &pathCount, &modeCount) != ERROR_SUCCESS) {
            return false;
        }
        c.paths.resize(pathCount);
        c.modes.resize(modeCount);
        LONG rc = QueryDisplayConfig(flags, &pathCount, c.paths.data(), &modeCount, c.modes.data(), nullptr);
        if (rc == ERROR_INSUFFICIENT_BUFFER) {
            continue;
        }
        if (rc != ERROR_SUCCESS) {
            return false;
        }
        c.paths.resize(pathCount);
        c.modes.resize(modeCount);
        return true;
    }
    return false;
}

std::wstring lowerCopy(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return s;
}

bool targetMatches(const DISPLAYCONFIG_PATH_INFO& p, const std::wstring& match) {
    DISPLAYCONFIG_TARGET_DEVICE_NAME name{};
    name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    name.header.size = sizeof name;
    name.header.adapterId = p.targetInfo.adapterId;
    name.header.id = p.targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&name.header) != ERROR_SUCCESS) {
        return false;
    }
    return lowerCopy(name.monitorFriendlyDeviceName).find(lowerCopy(match)) != std::wstring::npos;
}

bool sameAdapter(const LUID& a, const LUID& b) { return a.LowPart == b.LowPart && a.HighPart == b.HighPart; }

void clearModes(std::vector<DISPLAYCONFIG_PATH_INFO>& paths) {
    for (auto& p : paths) {
        p.sourceInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        p.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
    }
}

bool applyTopology(std::vector<DISPLAYCONFIG_PATH_INFO> paths, std::vector<DISPLAYCONFIG_MODE_INFO> modes, std::wstring& error) {
    std::vector<DISPLAYCONFIG_PATH_INFO> supplied = paths;
    clearModes(supplied);
    LONG rc = SetDisplayConfig(static_cast<UINT32>(supplied.size()), supplied.data(), 0, nullptr,
                               SDC_APPLY | SDC_TOPOLOGY_SUPPLIED | SDC_ALLOW_PATH_ORDER_CHANGES);
    if (rc == ERROR_SUCCESS) {
        return true;
    }
    rc = SetDisplayConfig(static_cast<UINT32>(paths.size()), paths.data(), static_cast<UINT32>(modes.size()), modes.data(),
                          SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE);
    if (rc == ERROR_SUCCESS) {
        return true;
    }
    error = L"SetDisplayConfig failed (" + std::to_wstring(rc) + L")";
    return false;
}

}

MonitorState virtualMonitorState(const std::wstring& match) {
    Config all;
    if (!query(QDC_ALL_PATHS, all)) {
        return MonitorState::Missing;
    }
    bool present = false;
    for (const auto& p : all.paths) {
        if (!targetMatches(p, match)) {
            continue;
        }
        if (p.flags & DISPLAYCONFIG_PATH_ACTIVE) {
            return MonitorState::Active;
        }
        if (p.targetInfo.targetAvailable) {
            present = true;
        }
    }
    return present ? MonitorState::Inactive : MonitorState::Missing;
}

bool setVirtualMonitorActive(const std::wstring& match, bool active, std::wstring& error) {
    Config current;
    if (!query(QDC_ONLY_ACTIVE_PATHS, current)) {
        error = L"QueryDisplayConfig failed";
        return false;
    }
    auto isVirtual = [&](const DISPLAYCONFIG_PATH_INFO& p) { return targetMatches(p, match); };
    bool isActive = std::any_of(current.paths.begin(), current.paths.end(), isVirtual);
    if (isActive == active) {
        return true;
    }
    if (!active) {
        if (current.paths.size() <= 1) {
            error = L"refusing to detach the only active display";
            return false;
        }
        std::vector<DISPLAYCONFIG_PATH_INFO> kept;
        for (const auto& p : current.paths) {
            if (!isVirtual(p)) {
                kept.push_back(p);
            }
        }
        return applyTopology(kept, current.modes, error);
    }
    Config all;
    if (!query(QDC_ALL_PATHS, all)) {
        error = L"QueryDisplayConfig failed";
        return false;
    }
    for (const auto& candidate : all.paths) {
        if (!candidate.targetInfo.targetAvailable || !isVirtual(candidate)) {
            continue;
        }
        bool sourceBusy = std::any_of(current.paths.begin(), current.paths.end(), [&](const DISPLAYCONFIG_PATH_INFO& p) {
            return sameAdapter(p.sourceInfo.adapterId, candidate.sourceInfo.adapterId) && p.sourceInfo.id == candidate.sourceInfo.id;
        });
        if (sourceBusy) {
            continue;
        }
        std::vector<DISPLAYCONFIG_PATH_INFO> paths = current.paths;
        DISPLAYCONFIG_PATH_INFO add = candidate;
        add.flags |= DISPLAYCONFIG_PATH_ACTIVE;
        add.sourceInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        add.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        paths.push_back(add);
        if (applyTopology(paths, current.modes, error)) {
            return true;
        }
    }
    if (error.empty()) {
        error = L"virtual monitor not found";
    }
    return false;
}

}
