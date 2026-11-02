#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>
#include <wrl/client.h>
#include <initguid.h>
#include <codecapi.h>

#include <array>
#include <cstdio>

#include "encoder.h"
#include "nvenc.h"

namespace td {

using Microsoft::WRL::ComPtr;

namespace {

class MfEncoder : public VideoEncoder {
public:
    ~MfEncoder() override { close(); }

    bool open(ID3D11Device* device, ID3D11Texture2D* input, const EncoderParams& params) override;
    void close() override;
    bool sequenceParams(std::vector<uint8_t>& out) override;
    bool encode(bool forceIdr, uint64_t timestamp, std::vector<uint8_t>& out, bool& idr) override;
    const std::string& error() const override { return error_; }
    const char* name() const override { return "Media Foundation"; }

private:
    bool fail(const char* what, HRESULT hr) {
        char buf[256];
        std::snprintf(buf, sizeof buf, "%s failed (0x%08lx)", what, static_cast<unsigned long>(hr));
        error_ = buf;
        return false;
    }
    bool setupConverter(ID3D11Texture2D* input);
    bool setTypes();
    bool pump(DWORD timeoutMs);
    bool drain(std::vector<uint8_t>& out, bool& idr);
    void setCodecValue(const GUID& key, ULONG value);
    void setCodecBool(const GUID& key, bool value);

    bool started_ = false;
    bool async_ = false;
    bool providesSamples_ = false;
    DWORD outputSize_ = 0;
    int needInput_ = 0;
    int haveOutput_ = 0;
    EncoderParams p_;
    std::string error_;
    ComPtr<IMFActivate> activate_;
    ComPtr<IMFTransform> mft_;
    ComPtr<IMFMediaEventGenerator> events_;
    ComPtr<ICodecAPI> codec_;
    ComPtr<IMFDXGIDeviceManager> manager_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11VideoDevice> videoDevice_;
    ComPtr<ID3D11VideoContext> videoContext_;
    ComPtr<ID3D11VideoProcessorEnumerator> vpEnum_;
    ComPtr<ID3D11VideoProcessor> vp_;
    ComPtr<ID3D11VideoProcessorInputView> inputView_;
    std::array<ComPtr<ID3D11Texture2D>, 4> nv12_;
    std::array<ComPtr<ID3D11VideoProcessorOutputView>, 4> outputViews_;
    size_t ring_ = 0;
};

LUID adapterLuid(ID3D11Device* device) {
    LUID luid{};
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) && SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&desc))) {
        luid = desc.AdapterLuid;
    }
    return luid;
}

bool MfEncoder::open(ID3D11Device* device, ID3D11Texture2D* input, const EncoderParams& params) {
    close();
    p_ = params;
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) {
        return fail("MFStartup", hr);
    }
    started_ = true;
    device_ = device;
    device_->GetImmediateContext(&context_);

    MFT_REGISTER_TYPE_INFO inInfo{MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO outInfo{MFMediaType_Video, params.codec == Codec::HEVC ? MFVideoFormat_HEVC : MFVideoFormat_H264};
    IMFActivate** list = nullptr;
    UINT32 count = 0;
    hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, &inInfo, &outInfo, &list, &count);
    if (FAILED(hr) || count == 0) {
        CoTaskMemFree(list);
        error_ = params.codec == Codec::HEVC ? "no hardware HEVC encoder for this GPU" : "no hardware H.264 encoder for this GPU";
        return false;
    }
    LUID luid = adapterLuid(device);
    UINT64 want = (static_cast<UINT64>(static_cast<uint32_t>(luid.HighPart)) << 32) | luid.LowPart;
    int pick = -1;
    for (UINT32 i = 0; i < count; ++i) {
        UINT64 value = 0;
        if (SUCCEEDED(list[i]->GetUINT64(MFT_ENUM_ADAPTER_LUID, &value))) {
            if (value == want && pick < 0) {
                pick = static_cast<int>(i);
            }
        }
    }
    if (pick < 0) {
        pick = 0;
    }
    activate_ = list[pick];
    for (UINT32 i = 0; i < count; ++i) {
        list[i]->Release();
    }
    CoTaskMemFree(list);

    hr = activate_->ActivateObject(IID_PPV_ARGS(&mft_));
    if (FAILED(hr)) {
        return fail("ActivateObject", hr);
    }
    ComPtr<IMFAttributes> attrs;
    if (SUCCEEDED(mft_->GetAttributes(&attrs))) {
        UINT32 isAsync = 0;
        if (SUCCEEDED(attrs->GetUINT32(MF_TRANSFORM_ASYNC, &isAsync)) && isAsync) {
            attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            async_ = true;
            mft_.As(&events_);
        }
        attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
    }
    UINT token = 0;
    hr = MFCreateDXGIDeviceManager(&token, &manager_);
    if (SUCCEEDED(hr)) {
        hr = manager_->ResetDevice(device, token);
    }
    if (FAILED(hr)) {
        return fail("MFCreateDXGIDeviceManager", hr);
    }
    hr = mft_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(manager_.Get()));
    if (FAILED(hr)) {
        return fail("MFT_MESSAGE_SET_D3D_MANAGER", hr);
    }
    mft_.As(&codec_);
    setCodecValue(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
    setCodecValue(CODECAPI_AVEncCommonMeanBitRate, params.bitrateKbps * 1000u);
    setCodecBool(CODECAPI_AVLowLatencyMode, true);
    setCodecValue(CODECAPI_AVEncMPVDefaultBPictureCount, 0);
    setCodecValue(CODECAPI_AVEncMPVGOPSize, params.fps * 30u);
    setCodecValue(CODECAPI_AVEncCommonQualityVsSpeed, 30);
    if (!setTypes()) {
        return false;
    }
    MFT_OUTPUT_STREAM_INFO info{};
    if (SUCCEEDED(mft_->GetOutputStreamInfo(0, &info))) {
        providesSamples_ = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        outputSize_ = info.cbSize;
    }
    if (!setupConverter(input)) {
        return false;
    }
    mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
}

void MfEncoder::setCodecValue(const GUID& key, ULONG value) {
    if (!codec_) {
        return;
    }
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_UI4;
    v.ulVal = value;
    codec_->SetValue(&key, &v);
}

void MfEncoder::setCodecBool(const GUID& key, bool value) {
    if (!codec_) {
        return;
    }
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BOOL;
    v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    codec_->SetValue(&key, &v);
}

bool MfEncoder::setTypes() {
    ComPtr<IMFMediaType> out;
    MFCreateMediaType(&out);
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, p_.codec == Codec::HEVC ? MFVideoFormat_HEVC : MFVideoFormat_H264);
    out->SetUINT32(MF_MT_AVG_BITRATE, p_.bitrateKbps * 1000u);
    MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, p_.width, p_.height);
    MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, p_.fps, 1);
    MFSetAttributeRatio(out.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    out->SetUINT32(MF_MT_MPEG2_PROFILE, p_.codec == Codec::HEVC ? static_cast<UINT32>(eAVEncH265VProfile_Main_420_8)
                                                                : static_cast<UINT32>(eAVEncH264VProfile_High));
    HRESULT hr = mft_->SetOutputType(0, out.Get(), 0);
    if (FAILED(hr)) {
        return fail("SetOutputType", hr);
    }
    ComPtr<IMFMediaType> in;
    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> candidate;
        if (FAILED(mft_->GetInputAvailableType(0, i, &candidate))) {
            break;
        }
        GUID sub{};
        candidate->GetGUID(MF_MT_SUBTYPE, &sub);
        if (sub == MFVideoFormat_NV12) {
            in = candidate;
            break;
        }
    }
    if (!in) {
        MFCreateMediaType(&in);
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    }
    MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, p_.width, p_.height);
    MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, p_.fps, 1);
    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    hr = mft_->SetInputType(0, in.Get(), 0);
    if (FAILED(hr)) {
        return fail("SetInputType", hr);
    }
    return true;
}

bool MfEncoder::setupConverter(ID3D11Texture2D* input) {
    HRESULT hr = device_.As(&videoDevice_);
    if (SUCCEEDED(hr)) {
        hr = context_.As(&videoContext_);
    }
    if (FAILED(hr)) {
        return fail("ID3D11VideoDevice", hr);
    }
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
    desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    desc.InputFrameRate = {p_.fps, 1};
    desc.InputWidth = p_.width;
    desc.InputHeight = p_.height;
    desc.OutputFrameRate = {p_.fps, 1};
    desc.OutputWidth = p_.width;
    desc.OutputHeight = p_.height;
    desc.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
    hr = videoDevice_->CreateVideoProcessorEnumerator(&desc, &vpEnum_);
    if (SUCCEEDED(hr)) {
        hr = videoDevice_->CreateVideoProcessor(vpEnum_.Get(), 0, &vp_);
    }
    if (FAILED(hr)) {
        return fail("CreateVideoProcessor", hr);
    }
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inDesc{};
    inDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    hr = videoDevice_->CreateVideoProcessorInputView(input, vpEnum_.Get(), &inDesc, &inputView_);
    if (FAILED(hr)) {
        return fail("CreateVideoProcessorInputView", hr);
    }
    for (size_t i = 0; i < nv12_.size(); ++i) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = p_.width;
        td.Height = p_.height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_NV12;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_VIDEO_ENCODER;
        hr = device_->CreateTexture2D(&td, nullptr, &nv12_[i]);
        if (FAILED(hr)) {
            td.BindFlags = D3D11_BIND_RENDER_TARGET;
            hr = device_->CreateTexture2D(&td, nullptr, &nv12_[i]);
        }
        if (FAILED(hr)) {
            return fail("CreateTexture2D NV12", hr);
        }
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outDesc{};
        outDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        hr = videoDevice_->CreateVideoProcessorOutputView(nv12_[i].Get(), vpEnum_.Get(), &outDesc, &outputViews_[i]);
        if (FAILED(hr)) {
            return fail("CreateVideoProcessorOutputView", hr);
        }
    }
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE inSpace{};
    inSpace.RGB_Range = 0;
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE outSpace{};
    outSpace.YCbCr_Matrix = 1;
    outSpace.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    videoContext_->VideoProcessorSetStreamColorSpace(vp_.Get(), 0, &inSpace);
    videoContext_->VideoProcessorSetOutputColorSpace(vp_.Get(), &outSpace);
    videoContext_->VideoProcessorSetStreamAutoProcessingMode(vp_.Get(), 0, FALSE);
    return true;
}

void MfEncoder::close() {
    if (mft_) {
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        mft_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
    }
    for (auto& v : outputViews_) {
        v.Reset();
    }
    for (auto& t : nv12_) {
        t.Reset();
    }
    inputView_.Reset();
    vp_.Reset();
    vpEnum_.Reset();
    videoContext_.Reset();
    videoDevice_.Reset();
    codec_.Reset();
    events_.Reset();
    mft_.Reset();
    if (activate_) {
        activate_->ShutdownObject();
        activate_.Reset();
    }
    manager_.Reset();
    context_.Reset();
    device_.Reset();
    needInput_ = 0;
    haveOutput_ = 0;
    async_ = false;
    if (started_) {
        MFShutdown();
        started_ = false;
    }
}

bool MfEncoder::sequenceParams(std::vector<uint8_t>& out) {
    out.clear();
    ComPtr<IMFMediaType> type;
    if (FAILED(mft_->GetOutputCurrentType(0, &type))) {
        return false;
    }
    UINT32 size = 0;
    if (FAILED(type->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) || size == 0) {
        return false;
    }
    out.resize(size);
    if (FAILED(type->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, out.data(), size, &size))) {
        out.clear();
        return false;
    }
    out.resize(size);
    return true;
}

bool MfEncoder::pump(DWORD timeoutMs) {
    ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        ComPtr<IMFMediaEvent> ev;
        HRESULT hr = events_->GetEvent(MF_EVENT_FLAG_NO_WAIT, &ev);
        if (hr == MF_E_NO_EVENTS_AVAILABLE) {
            if (GetTickCount64() >= deadline) {
                return false;
            }
            Sleep(0);
            continue;
        }
        if (FAILED(hr)) {
            fail("GetEvent", hr);
            return false;
        }
        MediaEventType type = MEUnknown;
        ev->GetType(&type);
        if (type == METransformNeedInput) {
            needInput_++;
        } else if (type == METransformHaveOutput) {
            haveOutput_++;
        }
        return true;
    }
}

bool MfEncoder::drain(std::vector<uint8_t>& out, bool& idr) {
    for (;;) {
        MFT_OUTPUT_DATA_BUFFER db{};
        ComPtr<IMFSample> own;
        if (!providesSamples_) {
            ComPtr<IMFMediaBuffer> buf;
            DWORD size = outputSize_ ? outputSize_ : p_.width * p_.height;
            MFCreateMemoryBuffer(size, &buf);
            MFCreateSample(&own);
            own->AddBuffer(buf.Get());
            db.pSample = own.Get();
        }
        DWORD status = 0;
        HRESULT hr = mft_->ProcessOutput(0, 1, &db, &status);
        if (db.pEvents) {
            db.pEvents->Release();
        }
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            return true;
        }
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            ComPtr<IMFMediaType> type;
            if (SUCCEEDED(mft_->GetOutputAvailableType(0, 0, &type))) {
                mft_->SetOutputType(0, type.Get(), 0);
            }
            continue;
        }
        if (FAILED(hr)) {
            return fail("ProcessOutput", hr);
        }
        IMFSample* sample = db.pSample;
        if (sample) {
            ComPtr<IMFMediaBuffer> contiguous;
            if (SUCCEEDED(sample->ConvertToContiguousBuffer(&contiguous))) {
                BYTE* data = nullptr;
                DWORD len = 0;
                if (SUCCEEDED(contiguous->Lock(&data, nullptr, &len))) {
                    out.insert(out.end(), data, data + len);
                    contiguous->Unlock();
                }
            }
            UINT32 clean = 0;
            if (SUCCEEDED(sample->GetUINT32(MFSampleExtension_CleanPoint, &clean)) && clean) {
                idr = true;
            }
            if (providesSamples_) {
                sample->Release();
            }
        }
        if (async_) {
            return true;
        }
    }
}

bool MfEncoder::encode(bool forceIdr, uint64_t timestamp, std::vector<uint8_t>& out, bool& idr) {
    out.clear();
    idr = false;
    if (forceIdr) {
        setCodecValue(CODECAPI_AVEncVideoForceKeyFrame, 1);
    }
    size_t slot = ring_++ % nv12_.size();
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = inputView_.Get();
    HRESULT hr = videoContext_->VideoProcessorBlt(vp_.Get(), outputViews_[slot].Get(), 0, 1, &stream);
    if (FAILED(hr)) {
        return fail("VideoProcessorBlt", hr);
    }
    ComPtr<IMFMediaBuffer> buffer;
    hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), nv12_[slot].Get(), 0, FALSE, &buffer);
    if (FAILED(hr)) {
        return fail("MFCreateDXGISurfaceBuffer", hr);
    }
    ComPtr<IMF2DBuffer> buffer2d;
    DWORD length = 0;
    if (SUCCEEDED(buffer.As(&buffer2d)) && SUCCEEDED(buffer2d->GetContiguousLength(&length))) {
        buffer->SetCurrentLength(length);
    }
    ComPtr<IMFSample> sample;
    MFCreateSample(&sample);
    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(static_cast<LONGLONG>(timestamp) * 10);
    sample->SetSampleDuration(10000000LL / (p_.fps ? p_.fps : 60));

    if (async_) {
        while (needInput_ == 0) {
            if (!pump(200)) {
                if (error_.empty()) {
                    error_ = "encoder did not request input";
                }
                return false;
            }
        }
        hr = mft_->ProcessInput(0, sample.Get(), 0);
        if (FAILED(hr)) {
            return fail("ProcessInput", hr);
        }
        needInput_--;
        ULONGLONG deadline = GetTickCount64() + 100;
        while (haveOutput_ == 0 && GetTickCount64() < deadline) {
            if (!pump(static_cast<DWORD>(deadline - GetTickCount64()))) {
                break;
            }
        }
        while (haveOutput_ > 0) {
            haveOutput_--;
            if (!drain(out, idr)) {
                return false;
            }
        }
        return true;
    }
    hr = mft_->ProcessInput(0, sample.Get(), 0);
    if (FAILED(hr)) {
        return fail("ProcessInput", hr);
    }
    return drain(out, idr);
}

}

std::unique_ptr<VideoEncoder> createEncoder(EncoderKind kind) {
    if (kind == EncoderKind::Nvenc) {
        return std::make_unique<NvEncoder>();
    }
    return std::make_unique<MfEncoder>();
}

}
