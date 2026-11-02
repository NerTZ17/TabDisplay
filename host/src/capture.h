#pragma once

#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

namespace td {

using Microsoft::WRL::ComPtr;

struct OutputInfo {
    std::wstring gdiName;
    std::wstring friendlyName;
    std::wstring adapterName;
    uint32_t vendorId = 0;
    LUID adapterLuid{};
    RECT desktop{};
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t refreshHz = 0;
};

std::vector<OutputInfo> listOutputs();

struct CursorUpdate {
    bool positionChanged = false;
    bool visible = false;
    int x = 0;
    int y = 0;
    bool shapeChanged = false;
    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t hotX = 0;
    uint16_t hotY = 0;
    std::vector<uint8_t> bgra;
};

class Capture {
public:
    enum class Result { Frame, NoFrame, Lost, Error };

    bool open(const OutputInfo& output);
    bool reopen();
    Result acquire(uint32_t timeoutMs, ID3D11Texture2D* dst, CursorUpdate& cursor);

    ID3D11Device* device() const { return device_.Get(); }
    ID3D11DeviceContext* context() const { return context_.Get(); }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

private:
    bool duplicate();
    void readPointer(const DXGI_OUTDUPL_FRAME_INFO& info, CursorUpdate& cursor);

    OutputInfo info_;
    ComPtr<IDXGIAdapter1> adapter_;
    ComPtr<IDXGIOutput1> output_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGIOutputDuplication> dupl_;
    std::vector<uint8_t> shapeBuffer_;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    int hotX_ = 0;
    int hotY_ = 0;
};

}
