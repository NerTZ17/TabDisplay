#pragma once

#include <d3d11.h>
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "encoder.h"
#include "nvEncodeAPI.h"

namespace td {

class NvEncoder : public VideoEncoder {
public:
    ~NvEncoder() override;

    bool open(ID3D11Device* device, ID3D11Texture2D* input, const EncoderParams& params) override;
    void close() override;
    bool sequenceParams(std::vector<uint8_t>& out) override;
    bool encode(bool forceIdr, uint64_t timestamp, std::vector<uint8_t>& out, bool& idr) override;
    const std::string& error() const override { return error_; }
    const char* name() const override { return "NVENC"; }

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
