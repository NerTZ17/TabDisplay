#pragma once

#include <d3d11.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "protocol.h"

namespace td {

struct EncoderParams {
    Codec codec = Codec::HEVC;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps = 60;
    uint32_t bitrateKbps = 80000;
};

class VideoEncoder {
public:
    virtual ~VideoEncoder() = default;
    virtual bool open(ID3D11Device* device, ID3D11Texture2D* input, const EncoderParams& params) = 0;
    virtual void close() = 0;
    virtual bool sequenceParams(std::vector<uint8_t>& out) = 0;
    virtual bool encode(bool forceIdr, uint64_t timestamp, std::vector<uint8_t>& out, bool& idr) = 0;
    virtual const std::string& error() const = 0;
    virtual const char* name() const = 0;
};

enum class EncoderKind { Nvenc, MediaFoundation };

std::unique_ptr<VideoEncoder> createEncoder(EncoderKind kind);

}
