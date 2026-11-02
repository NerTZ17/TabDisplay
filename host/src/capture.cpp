#include "capture.h"

#include <cstdio>
#include <map>

namespace td {

namespace {

std::map<std::wstring, std::wstring> friendlyNames() {
    std::map<std::wstring, std::wstring> names;
    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) {
        return names;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS) {
        return names;
    }
    for (UINT32 i = 0; i < pathCount; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof source;
        source.header.adapterId = paths[i].sourceInfo.adapterId;
        source.header.id = paths[i].sourceInfo.id;
        DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
        target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size = sizeof target;
        target.header.adapterId = paths[i].targetInfo.adapterId;
        target.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) == ERROR_SUCCESS &&
            DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS) {
            names[source.viewGdiDeviceName] = target.monitorFriendlyDeviceName;
        }
    }
    return names;
}

void convertShape(const DXGI_OUTDUPL_POINTER_SHAPE_INFO& si, const uint8_t* buf, CursorUpdate& c) {
    uint32_t w = si.Width;
    uint32_t h = si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME ? si.Height / 2 : si.Height;
    c.width = static_cast<uint16_t>(w);
    c.height = static_cast<uint16_t>(h);
    c.hotX = static_cast<uint16_t>(si.HotSpot.x);
    c.hotY = static_cast<uint16_t>(si.HotSpot.y);
    c.bgra.assign(static_cast<size_t>(w) * h * 4, 0);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t* d = &c.bgra[(static_cast<size_t>(y) * w + x) * 4];
            if (si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
                uint8_t bit = static_cast<uint8_t>(0x80 >> (x % 8));
                bool andBit = (buf[y * si.Pitch + x / 8] & bit) != 0;
                bool xorBit = (buf[(y + h) * si.Pitch + x / 8] & bit) != 0;
                if (andBit && !xorBit) {
                    continue;
                }
                uint8_t v = (!andBit && xorBit) ? 255 : 0;
                d[0] = v;
                d[1] = v;
                d[2] = v;
                d[3] = 255;
                continue;
            }
            const uint8_t* s = buf + y * si.Pitch + x * 4;
            if (si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) {
                d[0] = s[0];
                d[1] = s[1];
                d[2] = s[2];
                d[3] = s[3];
                continue;
            }
            bool xorPixel = s[3] != 0;
            bool black = s[0] == 0 && s[1] == 0 && s[2] == 0;
            if (xorPixel && black) {
                continue;
            }
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
            d[3] = 255;
        }
    }
}

}

std::vector<OutputInfo> listOutputs() {
    std::vector<OutputInfo> result;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return result;
    }
    auto names = friendlyNames();
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        DXGI_ADAPTER_DESC1 ad{};
        adapter->GetDesc1(&ad);
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC od{};
            output->GetDesc(&od);
            if (!od.AttachedToDesktop) {
                continue;
            }
            OutputInfo info;
            info.gdiName = od.DeviceName;
            info.adapterName = ad.Description;
            info.vendorId = ad.VendorId;
            info.adapterLuid = ad.AdapterLuid;
            info.desktop = od.DesktopCoordinates;
            info.width = static_cast<uint32_t>(od.DesktopCoordinates.right - od.DesktopCoordinates.left);
            info.height = static_cast<uint32_t>(od.DesktopCoordinates.bottom - od.DesktopCoordinates.top);
            auto it = names.find(info.gdiName);
            if (it != names.end()) {
                info.friendlyName = it->second;
            }
            DEVMODEW dm{};
            dm.dmSize = sizeof dm;
            if (EnumDisplaySettingsW(od.DeviceName, ENUM_CURRENT_SETTINGS, &dm)) {
                info.refreshHz = dm.dmDisplayFrequency;
            }
            result.push_back(info);
        }
    }
    return result;
}

bool Capture::open(const OutputInfo& output) {
    info_ = output;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return false;
    }
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        DXGI_ADAPTER_DESC1 ad{};
        adapter->GetDesc1(&ad);
        if (ad.AdapterLuid.LowPart != output.adapterLuid.LowPart || ad.AdapterLuid.HighPart != output.adapterLuid.HighPart) {
            continue;
        }
        ComPtr<IDXGIOutput> out;
        for (UINT o = 0; adapter->EnumOutputs(o, &out) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC od{};
            out->GetDesc(&od);
            if (output.gdiName == od.DeviceName) {
                adapter_ = adapter;
                out.As(&output_);
                break;
            }
        }
        if (output_) {
            break;
        }
    }
    if (!output_) {
        std::fprintf(stderr, "capture: output not found\n");
        return false;
    }
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                   static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &device_, nullptr, &context_);
    if (FAILED(hr)) {
        std::fprintf(stderr, "capture: D3D11CreateDevice failed 0x%08lx\n", static_cast<unsigned long>(hr));
        return false;
    }
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(context_.As(&mt))) {
        mt->SetMultithreadProtected(TRUE);
    }
    return duplicate();
}

bool Capture::duplicate() {
    dupl_.Reset();
    HRESULT hr = E_FAIL;
    ComPtr<IDXGIOutput5> output5;
    if (SUCCEEDED(output_.As(&output5))) {
        const DXGI_FORMAT formats[] = {DXGI_FORMAT_B8G8R8A8_UNORM};
        hr = output5->DuplicateOutput1(device_.Get(), 0, static_cast<UINT>(std::size(formats)), formats, &dupl_);
    }
    if (FAILED(hr)) {
        hr = output_->DuplicateOutput(device_.Get(), &dupl_);
    }
    if (FAILED(hr)) {
        std::fprintf(stderr, "capture: DuplicateOutput failed 0x%08lx\n", static_cast<unsigned long>(hr));
        return false;
    }
    DXGI_OUTDUPL_DESC desc{};
    dupl_->GetDesc(&desc);
    width_ = desc.ModeDesc.Width;
    height_ = desc.ModeDesc.Height;
    if (desc.Rotation != DXGI_MODE_ROTATION_IDENTITY && desc.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED) {
        std::fprintf(stderr, "capture: rotated outputs are not supported\n");
        return false;
    }
    return true;
}

bool Capture::reopen() {
    for (int attempt = 0; attempt < 50; ++attempt) {
        if (duplicate()) {
            return true;
        }
        Sleep(100);
    }
    return false;
}

void Capture::readPointer(const DXGI_OUTDUPL_FRAME_INFO& info, CursorUpdate& cursor) {
    if (info.PointerShapeBufferSize > 0) {
        if (shapeBuffer_.size() < info.PointerShapeBufferSize) {
            shapeBuffer_.resize(info.PointerShapeBufferSize);
        }
        UINT required = 0;
        DXGI_OUTDUPL_POINTER_SHAPE_INFO si{};
        if (SUCCEEDED(dupl_->GetFramePointerShape(info.PointerShapeBufferSize, shapeBuffer_.data(), &required, &si))) {
            convertShape(si, shapeBuffer_.data(), cursor);
            cursor.shapeChanged = true;
            hotX_ = si.HotSpot.x;
            hotY_ = si.HotSpot.y;
        }
    }
    if (info.LastMouseUpdateTime.QuadPart != 0) {
        cursor.positionChanged = true;
        cursor.visible = info.PointerPosition.Visible != FALSE;
        cursor.x = info.PointerPosition.Position.x + hotX_;
        cursor.y = info.PointerPosition.Position.y + hotY_;
    }
}

Capture::Result Capture::acquire(uint32_t timeoutMs, ID3D11Texture2D* dst, CursorUpdate& cursor) {
    if (!dupl_) {
        return Result::Lost;
    }
    DXGI_OUTDUPL_FRAME_INFO info{};
    ComPtr<IDXGIResource> resource;
    HRESULT hr = dupl_->AcquireNextFrame(timeoutMs, &info, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        return Result::NoFrame;
    }
    if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL) {
        dupl_.Reset();
        return Result::Lost;
    }
    if (FAILED(hr)) {
        return Result::Error;
    }
    readPointer(info, cursor);
    Result result = Result::NoFrame;
    if (info.LastPresentTime.QuadPart != 0) {
        ComPtr<ID3D11Texture2D> tex;
        if (SUCCEEDED(resource.As(&tex))) {
            context_->CopyResource(dst, tex.Get());
            result = Result::Frame;
        }
    }
    dupl_->ReleaseFrame();
    return result;
}

}
