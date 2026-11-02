#include "nvenc.h"

#include <cstdio>

namespace td {

namespace {

using CreateInstanceFn = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
using MaxVersionFn = NVENCSTATUS(NVENCAPI*)(uint32_t*);

}

NvEncoder::~NvEncoder() { close(); }

bool NvEncoder::fail(const char* what, NVENCSTATUS status) {
    char buf[512];
    const char* detail = (enc_ && fn_.nvEncGetLastErrorString) ? fn_.nvEncGetLastErrorString(enc_) : "";
    std::snprintf(buf, sizeof buf, "%s failed (NVENCSTATUS %d) %s", what, static_cast<int>(status), detail ? detail : "");
    error_ = buf;
    return false;
}

bool NvEncoder::open(ID3D11Device* device, ID3D11Texture2D* input, const EncoderParams& params) {
    close();
    params_ = params;
    lib_ = LoadLibraryW(L"nvEncodeAPI64.dll");
    if (!lib_) {
        error_ = "nvEncodeAPI64.dll not found (NVIDIA driver missing?)";
        return false;
    }
    auto maxVersion = reinterpret_cast<MaxVersionFn>(GetProcAddress(lib_, "NvEncodeAPIGetMaxSupportedVersion"));
    auto createInstance = reinterpret_cast<CreateInstanceFn>(GetProcAddress(lib_, "NvEncodeAPICreateInstance"));
    if (!maxVersion || !createInstance) {
        error_ = "nvEncodeAPI64.dll exports missing";
        return false;
    }
    uint32_t driverVersion = 0;
    maxVersion(&driverVersion);
    uint32_t headerVersion = (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
    if (headerVersion > driverVersion) {
        char buf[256];
        std::snprintf(buf, sizeof buf, "driver supports NVENC API %u.%u, this build needs %u.%u: update the NVIDIA driver",
                      driverVersion >> 4, driverVersion & 0xF, NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION);
        error_ = buf;
        return false;
    }
    fn_ = {};
    fn_.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    NVENCSTATUS st = createInstance(&fn_);
    if (st != NV_ENC_SUCCESS) {
        return fail("NvEncodeAPICreateInstance", st);
    }

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS session{};
    session.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    session.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    session.device = device;
    session.apiVersion = NVENCAPI_VERSION;
    st = fn_.nvEncOpenEncodeSessionEx(&session, &enc_);
    if (st != NV_ENC_SUCCESS) {
        enc_ = nullptr;
        return fail("nvEncOpenEncodeSessionEx", st);
    }

    GUID codecGuid = params.codec == Codec::HEVC ? NV_ENC_CODEC_HEVC_GUID : NV_ENC_CODEC_H264_GUID;
    NV_ENC_PRESET_CONFIG preset{};
    preset.version = NV_ENC_PRESET_CONFIG_VER;
    preset.presetCfg.version = NV_ENC_CONFIG_VER;
    st = fn_.nvEncGetEncodePresetConfigEx(enc_, codecGuid, NV_ENC_PRESET_P1_GUID, NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &preset);
    if (st != NV_ENC_SUCCESS) {
        return fail("nvEncGetEncodePresetConfigEx", st);
    }
    config_ = preset.presetCfg;
    config_.version = NV_ENC_CONFIG_VER;
    config_.gopLength = NVENC_INFINITE_GOPLENGTH;
    config_.frameIntervalP = 1;
    uint32_t bitrate = params.bitrateKbps * 1000u;
    NV_ENC_RC_PARAMS& rc = config_.rcParams;
    rc.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    rc.multiPass = NV_ENC_MULTI_PASS_DISABLED;
    rc.averageBitRate = bitrate;
    rc.maxBitRate = bitrate;
    rc.vbvBufferSize = bitrate / (params.fps ? params.fps : 60);
    rc.vbvInitialDelay = rc.vbvBufferSize;
    rc.zeroReorderDelay = 1;
    if (params.codec == Codec::HEVC) {
        config_.profileGUID = NV_ENC_HEVC_PROFILE_MAIN_GUID;
        NV_ENC_CONFIG_HEVC& hevc = config_.encodeCodecConfig.hevcConfig;
        hevc.repeatSPSPPS = 1;
        hevc.idrPeriod = NVENC_INFINITE_GOPLENGTH;
        hevc.chromaFormatIDC = 1;
    } else {
        config_.profileGUID = NV_ENC_H264_PROFILE_HIGH_GUID;
        NV_ENC_CONFIG_H264& h264 = config_.encodeCodecConfig.h264Config;
        h264.repeatSPSPPS = 1;
        h264.idrPeriod = NVENC_INFINITE_GOPLENGTH;
        h264.chromaFormatIDC = 1;
    }

    NV_ENC_INITIALIZE_PARAMS init{};
    init.version = NV_ENC_INITIALIZE_PARAMS_VER;
    init.encodeGUID = codecGuid;
    init.presetGUID = NV_ENC_PRESET_P1_GUID;
    init.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    init.encodeWidth = params.width;
    init.encodeHeight = params.height;
    init.darWidth = params.width;
    init.darHeight = params.height;
    init.maxEncodeWidth = params.width;
    init.maxEncodeHeight = params.height;
    init.frameRateNum = params.fps;
    init.frameRateDen = 1;
    init.enablePTD = 1;
    init.encodeConfig = &config_;
    st = fn_.nvEncInitializeEncoder(enc_, &init);
    if (st != NV_ENC_SUCCESS) {
        return fail("nvEncInitializeEncoder", st);
    }

    NV_ENC_REGISTER_RESOURCE reg{};
    reg.version = NV_ENC_REGISTER_RESOURCE_VER;
    reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    reg.width = params.width;
    reg.height = params.height;
    reg.pitch = 0;
    reg.resourceToRegister = input;
    reg.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
    reg.bufferUsage = NV_ENC_INPUT_IMAGE;
    st = fn_.nvEncRegisterResource(enc_, &reg);
    if (st != NV_ENC_SUCCESS) {
        return fail("nvEncRegisterResource", st);
    }
    registered_ = reg.registeredResource;

    NV_ENC_CREATE_BITSTREAM_BUFFER bs{};
    bs.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    st = fn_.nvEncCreateBitstreamBuffer(enc_, &bs);
    if (st != NV_ENC_SUCCESS) {
        return fail("nvEncCreateBitstreamBuffer", st);
    }
    bitstream_ = bs.bitstreamBuffer;
    return true;
}

void NvEncoder::close() {
    if (enc_) {
        if (bitstream_) {
            fn_.nvEncDestroyBitstreamBuffer(enc_, bitstream_);
        }
        if (registered_) {
            fn_.nvEncUnregisterResource(enc_, registered_);
        }
        fn_.nvEncDestroyEncoder(enc_);
    }
    enc_ = nullptr;
    bitstream_ = nullptr;
    registered_ = nullptr;
    if (lib_) {
        FreeLibrary(lib_);
        lib_ = nullptr;
    }
}

bool NvEncoder::sequenceParams(std::vector<uint8_t>& out) {
    out.assign(1024, 0);
    uint32_t size = 0;
    NV_ENC_SEQUENCE_PARAM_PAYLOAD payload{};
    payload.version = NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER;
    payload.inBufferSize = static_cast<uint32_t>(out.size());
    payload.spsppsBuffer = out.data();
    payload.outSPSPPSPayloadSize = &size;
    NVENCSTATUS st = fn_.nvEncGetSequenceParams(enc_, &payload);
    if (st != NV_ENC_SUCCESS) {
        out.clear();
        return fail("nvEncGetSequenceParams", st);
    }
    out.resize(size);
    return true;
}

bool NvEncoder::encode(bool forceIdr, uint64_t timestamp, std::vector<uint8_t>& out, bool& idr) {
    NV_ENC_MAP_INPUT_RESOURCE map{};
    map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    map.registeredResource = registered_;
    NVENCSTATUS st = fn_.nvEncMapInputResource(enc_, &map);
    if (st != NV_ENC_SUCCESS) {
        return fail("nvEncMapInputResource", st);
    }
    NV_ENC_PIC_PARAMS pic{};
    pic.version = NV_ENC_PIC_PARAMS_VER;
    pic.inputWidth = params_.width;
    pic.inputHeight = params_.height;
    pic.inputPitch = params_.width;
    pic.inputBuffer = map.mappedResource;
    pic.bufferFmt = map.mappedBufferFmt;
    pic.outputBitstream = bitstream_;
    pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    pic.inputTimeStamp = timestamp;
    if (forceIdr) {
        pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
    }
    st = fn_.nvEncEncodePicture(enc_, &pic);
    if (st != NV_ENC_SUCCESS) {
        fn_.nvEncUnmapInputResource(enc_, map.mappedResource);
        return fail("nvEncEncodePicture", st);
    }
    NV_ENC_LOCK_BITSTREAM lock{};
    lock.version = NV_ENC_LOCK_BITSTREAM_VER;
    lock.outputBitstream = bitstream_;
    st = fn_.nvEncLockBitstream(enc_, &lock);
    if (st != NV_ENC_SUCCESS) {
        fn_.nvEncUnmapInputResource(enc_, map.mappedResource);
        return fail("nvEncLockBitstream", st);
    }
    auto data = static_cast<const uint8_t*>(lock.bitstreamBufferPtr);
    out.assign(data, data + lock.bitstreamSizeInBytes);
    idr = lock.pictureType == NV_ENC_PIC_TYPE_IDR;
    fn_.nvEncUnlockBitstream(enc_, bitstream_);
    fn_.nvEncUnmapInputResource(enc_, map.mappedResource);
    return true;
}

}
