#pragma once

#include <d3d11.h>
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "nvEncodeAPI.h"
#include "protocol.h"

namespace td {

struct EncoderParams {
    Codec codec = Codec::HEVC;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps = 60;
    uint32_t bitrateKbps = 80000;
};

class NvEncoder {
public:
    ~NvEncoder();

    bool open(ID3D11Device* device, ID3D11Texture2D* input, const EncoderParams& params);
    void close();
    bool sequenceParams(std::vector<uint8_t>& out);
    bool encode(bool forceIdr, uint64_t timestamp, std::vector<uint8_t>& out, bool& idr);

    const std::string& error() const { return error_; }

private:
    bool fail(const char* what, NVENCSTATUS status);

    HMODULE lib_ = nullptr;
    NV_ENCODE_API_FUNCTION_LIST fn_{};
    void* enc_ = nullptr;
    NV_ENC_CONFIG config_{};
    NV_ENC_REGISTERED_PTR registered_ = nullptr;
    NV_ENC_OUTPUT_PTR bitstream_ = nullptr;
    EncoderParams params_;
    std::string error_;
};

}
