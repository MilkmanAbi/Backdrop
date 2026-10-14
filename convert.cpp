// VidWall converter. Everything stays on the GPU where the hardware allows it:
//   video:  Source Reader (DXVA decode) -> D3D11 video processor (crop/scale/rotate -> NV12) -> Sink Writer (HW H.264)
//   GIF:    WIC decode + GIF disposal compositing -> upload -> video processor -> encoder
//   WebP:   libwebp WebPAnimDecoder (composited) -> upload -> video processor -> encoder
// Falls back to the software H.264 encoder when the hardware path refuses.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "convert.h"
#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <codecapi.h>
#include <strmif.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <vector>
#include "webp/demux.h"

#pragma comment(lib, "mfplat")
#pragma comment(lib, "mfreadwrite")
#pragma comment(lib, "mfuuid")
#pragma comment(lib, "windowscodecs")
#pragma comment(lib, "ole32")
#pragma comment(lib, "oleaut32")
#pragma comment(lib, "d3d11")
#pragma comment(lib, "libwebp")
#pragma comment(lib, "libwebpdemux")
#pragma comment(lib, "libsharpyuv")

using Microsoft::WRL::ComPtr;

static const HRESULT VW_E_SOURCE = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x201);   // bad input: don't retry

static void Msg(wchar_t* e, size_t n, const wchar_t* f, ...) {
    if (!e || !n) return;
    va_list a;
    va_start(a, f);
    _vsnwprintf_s(e, n, _TRUNCATE, f, a);
    va_end(a);
}

// ------------------------------------------------------------------ geometry
static int Even(double v) { int i = int(v + 0.5) & ~1; return i < 2 ? 2 : i; }

void BakedSize(int w, int h, int sw, int sh, Fit fit, int* ow, int* oh) {
    double W, H;
    if (fit == FIT_FIT) { double s = std::min(double(sw) / w, double(sh) / h); W = w * s; H = h * s; }
    else if (fit == FIT_CENTER) { W = std::min(w, sw); H = std::min(h, sh); }
    else { W = sw; H = sh; }
    // H.264 level 5.1/5.2 (what hardware encoders/decoders take): <= 4096 per side, <= 36864 macroblocks
    double k = std::min({1.0, 4096.0 / W, 4096.0 / H, std::sqrt(9437184.0 / (W * H))});
    *ow = Even(W * k);
    *oh = Even(H * k);
}

double Upscale(int w, int h, int sw, int sh, Fit fit) {
    double ax = double(sw) / w, ay = double(sh) / h;
    if (fit == FIT_FIT) return std::min(ax, ay);
    if (fit == FIT_CENTER) return 1.0;
    return std::max(ax, ay);
}

// centered fraction of the (upright) source that ends up on screen
static void CropFraction(int w, int h, int sw, int sh, Fit fit, double* fx, double* fy) {
    *fx = *fy = 1.0;
    if (fit == FIT_FILL) {
        double s = std::max(double(sw) / w, double(sh) / h);
        *fx = std::min(1.0, sw / (s * w));
        *fy = std::min(1.0, sh / (s * h));
    } else if (fit == FIT_CENTER) {
        *fx = std::min(1.0, double(sw) / w);
        *fy = std::min(1.0, double(sh) / h);
    }
}

static RECT CenteredCrop(const RECT& area, double fx, double fy) {
    LONG w = area.right - area.left, h = area.bottom - area.top;
    LONG cw = std::max<LONG>(2, LONG(w * fx + 0.5)), ch = std::max<LONG>(2, LONG(h * fy + 0.5));
    LONG x = area.left + (w - cw) / 2, y = area.top + (h - ch) / 2;
    return RECT{x, y, x + cw, y + ch};
}

// ------------------------------------------------------------------ file sniffing
enum Kind { K_VIDEO, K_GIF, K_WEBP };

static Kind Sniff(const wchar_t* path) {
    BYTE b[12] = {};
    DWORD n = 0;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) { ReadFile(f, b, sizeof(b), &n, nullptr); CloseHandle(f); }
    if (n >= 6 && !memcmp(b, "GIF8", 4)) return K_GIF;
    if (n >= 12 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WEBP", 4)) return K_WEBP;
    return K_VIDEO;
}

// ------------------------------------------------------------------ animated images -> opaque BGRX frames
struct ImageSource {
    int w = 0, h = 0;
    std::vector<int> delays;   // ms per frame (browser rule: <= 10 ms means 100 ms)
    virtual ~ImageSource() {}
    virtual bool Open(const wchar_t* path, wchar_t* err, size_t n) = 0;
    virtual bool Frame(int i, uint8_t* bgrx) = 0;   // i = 0, 1, 2, ... in order (0 restarts)
};

static int FixDelay(int ms) { return ms <= 10 ? 100 : ms; }

struct GifSource : ImageSource {
    struct Info { int x, y, w, h, disposal; };
    ComPtr<IWICImagingFactory> fac;
    ComPtr<IWICBitmapDecoder> dec;
    std::vector<Info> info;
    std::vector<uint8_t> canvas, saved, px;   // canvas: premultiplied BGRA

    static int Meta(IWICMetadataQueryReader* q, const wchar_t* name, int def) {
        PROPVARIANT v;
        PropVariantInit(&v);
        int r = def;
        if (q && SUCCEEDED(q->GetMetadataByName(name, &v))) {
            if (v.vt == VT_UI1) r = v.bVal;
            else if (v.vt == VT_UI2) r = v.uiVal;
            else if (v.vt == VT_UI4) r = int(v.ulVal);
            else if (v.vt == VT_BOOL) r = v.boolVal ? 1 : 0;
        }
        PropVariantClear(&v);
        return r;
    }

    bool Open(const wchar_t* path, wchar_t* err, size_t n) override {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac))) ||
            FAILED(fac->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) {
            Msg(err, n, L"This GIF couldn't be read (the file may be damaged).");
            return false;
        }
        ComPtr<IWICMetadataQueryReader> gq;
        dec->GetMetadataQueryReader(&gq);
        w = Meta(gq.Get(), L"/logscrdesc/Width", 0);
        h = Meta(gq.Get(), L"/logscrdesc/Height", 0);
        bool logical = w > 0 && h > 0;
        UINT count = 0;
        dec->GetFrameCount(&count);
        int ew = 0, eh = 0;
        for (UINT i = 0; i < count; i++) {
            ComPtr<IWICBitmapFrameDecode> fr;
            if (FAILED(dec->GetFrame(i, &fr))) break;
            ComPtr<IWICMetadataQueryReader> q;
            fr->GetMetadataQueryReader(&q);
            UINT fw = 0, fh = 0;
            fr->GetSize(&fw, &fh);
            Info f{Meta(q.Get(), L"/imgdesc/Left", 0), Meta(q.Get(), L"/imgdesc/Top", 0), int(fw), int(fh), Meta(q.Get(), L"/grctlext/Disposal", 0)};
            info.push_back(f);
            delays.push_back(FixDelay(Meta(q.Get(), L"/grctlext/Delay", 0) * 10));
            ew = std::max(ew, f.x + f.w);
            eh = std::max(eh, f.y + f.h);
        }
        if (!logical) { w = ew; h = eh; }
        if (info.empty() || w <= 0 || h <= 0) { Msg(err, n, L"This GIF has no frames."); return false; }
        canvas.assign(size_t(w) * h * 4, 0);
        return true;
    }

    void Clear(const Info& f) {
        for (int y = std::max(0, f.y); y < std::min(h, f.y + f.h); y++)
            for (int x = std::max(0, f.x); x < std::min(w, f.x + f.w); x++) memset(&canvas[(size_t(y) * w + x) * 4], 0, 4);
    }

    bool Frame(int i, uint8_t* out) override {
        if (i == 0) { std::fill(canvas.begin(), canvas.end(), 0); saved.clear(); }
        else {                                        // dispose the previous frame
            const Info& p = info[i - 1];
            if (p.disposal == 2) Clear(p);
            else if (p.disposal == 3 && !saved.empty()) canvas = saved;
        }
        const Info& f = info[i];
        if (f.disposal == 3) saved = canvas;
        ComPtr<IWICBitmapFrameDecode> fr;
        ComPtr<IWICBitmapSource> cv;
        if (FAILED(dec->GetFrame(i, &fr)) || FAILED(WICConvertBitmapSource(GUID_WICPixelFormat32bppPBGRA, fr.Get(), &cv))) return false;
        px.resize(size_t(f.w) * f.h * 4);
        if (FAILED(cv->CopyPixels(nullptr, f.w * 4, UINT(px.size()), px.data()))) return false;
        for (int y = 0; y < f.h; y++) {
            int cy = f.y + y;
            if (cy < 0 || cy >= h) continue;
            for (int x = 0; x < f.w; x++) {
                int cx = f.x + x;
                const uint8_t* s = &px[(size_t(y) * f.w + x) * 4];
                if (cx >= 0 && cx < w && s[3]) memcpy(&canvas[(size_t(cy) * w + cx) * 4], s, 4);
            }
        }
        for (size_t k = 0, n = size_t(w) * h; k < n; k++) {   // premultiplied over black = its RGB
            memcpy(out + k * 4, &canvas[k * 4], 3);
            out[k * 4 + 3] = 255;
        }
        return true;
    }
};

struct WebpSource : ImageSource {
    std::vector<uint8_t> data;
    WebPAnimDecoder* dec = nullptr;
    ~WebpSource() override { if (dec) WebPAnimDecoderDelete(dec); }

    bool Open(const wchar_t* path, wchar_t* err, size_t n) override {
        HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        LARGE_INTEGER sz{};
        DWORD got = 0;
        if (f != INVALID_HANDLE_VALUE && GetFileSizeEx(f, &sz) && sz.QuadPart < (1LL << 31)) {
            data.resize(size_t(sz.QuadPart));
            ReadFile(f, data.data(), DWORD(data.size()), &got, nullptr);
        }
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        WebPData wd{data.data(), got};
        WebPAnimDecoderOptions o;
        WebPAnimDecoderOptionsInit(&o);
        o.color_mode = MODE_bgrA;   // premultiplied: over black = its RGB
        o.use_threads = 1;
        if (!got || !(dec = WebPAnimDecoderNew(&wd, &o))) { Msg(err, n, L"This WebP couldn't be read (the file may be damaged)."); return false; }
        WebPAnimInfo ai;
        WebPAnimDecoderGetInfo(dec, &ai);
        w = int(ai.canvas_width);
        h = int(ai.canvas_height);
        WebPIterator it;
        if (WebPDemuxGetFrame(WebPAnimDecoderGetDemuxer(dec), 1, &it)) {
            do delays.push_back(FixDelay(it.duration)); while (WebPDemuxNextFrame(&it));
            WebPDemuxReleaseIterator(&it);
        }
        if (delays.empty()) delays.push_back(100);
        return w > 0 && h > 0;
    }

    bool Frame(int i, uint8_t* out) override {
        if (i == 0) WebPAnimDecoderReset(dec);
        uint8_t* buf;
        int ts;
        if (!WebPAnimDecoderGetNext(dec, &buf, &ts)) return false;
        for (size_t k = 0, n = size_t(w) * h; k < n; k++) {
            memcpy(out + k * 4, buf + k * 4, 3);
            out[k * 4 + 3] = 255;
        }
        return true;
    }
};

static std::unique_ptr<ImageSource> OpenImage(Kind k, const wchar_t* path, wchar_t* err, size_t n) {
    std::unique_ptr<ImageSource> s;
    if (k == K_GIF) s.reset(new GifSource);
    else s.reset(new WebpSource);
    if (!s->Open(path, err, n)) s.reset();
    return s;
}

// ------------------------------------------------------------------ video source
static void CodecName(const GUID& g, wchar_t* out, size_t n) {
    if (g == MFVideoFormat_HEVC || g == MFVideoFormat_HEVC_ES)
        Msg(out, n, L"HEVC (H.265). Install \"HEVC Video Extensions\" from the Microsoft Store, or use an H.264 MP4");
    else if (g == MFVideoFormat_AV1)
        Msg(out, n, L"AV1. Install \"AV1 Video Extension\" from the Microsoft Store, or use an H.264 MP4");
    else if (g == MFVideoFormat_VP90)
        Msg(out, n, L"VP9. Install \"VP9 Video Extensions\" from the Microsoft Store, or use an H.264 MP4");
    else {
        char c[5] = {char(g.Data1), char(g.Data1 >> 8), char(g.Data1 >> 16), char(g.Data1 >> 24), 0};
        for (char& ch : c) if (ch && (ch < 32 || ch > 126)) ch = '?';
        if (!memcmp(c, "ap", 2) || !memcmp(c + 2, "pa", 2))   // QuickTime FourCCs can come through byte-swapped
            Msg(out, n, L"Apple ProRes, which Windows can't decode. Export it as an H.264 MP4 instead");
        else
            Msg(out, n, L"the codec '%hs', which Windows can't decode. Use an H.264 MP4 instead", c);
    }
}

struct VideoInfo {
    ComPtr<IMFSourceReader> r;
    GUID sub{};
    int fw = 0, fh = 0;        // decoded frame (texture) size
    RECT ap{};                 // visible area inside it
    int rotation = 0;
    UINT fpsN = 30, fpsD = 1;
    LONGLONG duration = 0;
    bool bt709 = true, fullRange = false;
};

static void ReadFormat(VideoInfo& v) {
    ComPtr<IMFMediaType> t;
    if (FAILED(v.r->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &t))) return;
    UINT32 w = 0, h = 0;
    MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &w, &h);
    v.fw = int(w); v.fh = int(h);
    v.ap = RECT{0, 0, LONG(w), LONG(h)};
    MFVideoArea a;
    if (SUCCEEDED(t->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8*)&a, sizeof(a), nullptr)) ||
        SUCCEEDED(t->GetBlob(MF_MT_GEOMETRIC_APERTURE, (UINT8*)&a, sizeof(a), nullptr))) {
        RECT r{a.OffsetX.value, a.OffsetY.value, a.OffsetX.value + LONG(a.Area.cx), a.OffsetY.value + LONG(a.Area.cy)};
        if (r.right > r.left && r.bottom > r.top && r.right <= LONG(w) && r.bottom <= LONG(h)) v.ap = r;
    }
}

// gpuMan == nullptr: probe only (no decoder output needed beyond checking the codec exists)
static HRESULT OpenVideo(const wchar_t* path, IMFDXGIDeviceManager* gpuMan, VideoInfo& v, wchar_t* err, size_t n) {
    ComPtr<IMFAttributes> at;
    MFCreateAttributes(&at, 4);
    at->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    if (gpuMan) {
        at->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, gpuMan);
        at->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    }
    if (FAILED(MFCreateSourceReaderFromURL(path, at.Get(), &v.r))) {
        Msg(err, n, L"Windows can't open this file. Supported: MP4, MOV, AVI, MKV, WMV, GIF, WebP.");
        return VW_E_SOURCE;
    }
    v.r->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    ComPtr<IMFMediaType> nt;
    if (FAILED(v.r->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE)) ||
        FAILED(v.r->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &nt))) {
        Msg(err, n, L"This file has no video in it.");
        return VW_E_SOURCE;
    }
    nt->GetGUID(MF_MT_SUBTYPE, &v.sub);
    v.rotation = int(MFGetAttributeUINT32(nt.Get(), MF_MT_VIDEO_ROTATION, 0)) % 360;
    MFGetAttributeRatio(nt.Get(), MF_MT_FRAME_RATE, &v.fpsN, &v.fpsD);
    if (!v.fpsN || !v.fpsD) { v.fpsN = 30; v.fpsD = 1; }
    UINT32 nw = 0, nh = 0;
    MFGetAttributeSize(nt.Get(), MF_MT_FRAME_SIZE, &nw, &nh);
    UINT32 m = MFGetAttributeUINT32(nt.Get(), MF_MT_YUV_MATRIX, 0);
    v.bt709 = m ? m != MFVideoTransferMatrix_BT601 : nh >= 720;
    v.fullRange = MFGetAttributeUINT32(nt.Get(), MF_MT_VIDEO_NOMINAL_RANGE, 0) == MFNominalRange_0_255;

    ComPtr<IMFMediaType> want;
    MFCreateMediaType(&want);
    want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    want->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    if (FAILED(v.r->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, want.Get()))) {
        wchar_t c[200];
        CodecName(v.sub, c, 200);
        Msg(err, n, L"This video uses %s.", c);
        return VW_E_SOURCE;
    }
    ReadFormat(v);
    PROPVARIANT pv;
    PropVariantInit(&pv);
    if (SUCCEEDED(v.r->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &pv))) v.duration = LONGLONG(pv.uhVal.QuadPart);
    PropVariantClear(&pv);
    return S_OK;
}

// ------------------------------------------------------------------ probe
bool ProbeMedia(const wchar_t* path, Probe& p, wchar_t* err, size_t n) {
    p = Probe{};
    Kind k = Sniff(path);
    if (k != K_VIDEO) {
        auto s = OpenImage(k, path, err, n);
        if (!s) return false;
        p.image = true;
        p.w = s->w; p.h = s->h;
        return true;
    }
    VideoInfo v;
    if (FAILED(OpenVideo(path, nullptr, v, err, n))) return false;
    int aw = v.ap.right - v.ap.left, ah = v.ap.bottom - v.ap.top;
    bool swap = v.rotation == 90 || v.rotation == 270;
    p.w = swap ? ah : aw;
    p.h = swap ? aw : ah;
    p.rotation = v.rotation;
    p.h264 = v.sub == MFVideoFormat_H264;
    p.fps = double(v.fpsN) / v.fpsD;
    return p.w > 0 && p.h > 0;
}

bool PlayableAsIs(const wchar_t* path, const Probe& p, int sw, int sh, Fit fit) {
    if (p.image || !p.h264 || p.rotation || p.fps > 61) return false;
    const wchar_t* ext = wcsrchr(path, L'.');
    if (!ext || (_wcsicmp(ext, L".mp4") && _wcsicmp(ext, L".m4v") && _wcsicmp(ext, L".mov"))) return false;
    int ow, oh;
    BakedSize(p.w, p.h, sw, sh, fit, &ow, &oh);
    return ow == p.w && oh == p.h;
}

// ------------------------------------------------------------------ GPU pieces
struct Gpu {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11VideoDevice> vdev;
    ComPtr<ID3D11VideoContext> vctx;
    ComPtr<IMFDXGIDeviceManager> man;

    bool Init() {
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                     nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx)))
            return false;
        ComPtr<ID3D10Multithread> mt;
        if (SUCCEEDED(dev.As(&mt))) mt->SetMultithreadProtected(TRUE);
        UINT token;
        return SUCCEEDED(dev.As(&vdev)) && SUCCEEDED(ctx.As(&vctx)) && SUCCEEDED(MFCreateDXGIDeviceManager(&token, &man)) &&
               SUCCEEDED(man->ResetDevice(dev.Get(), token));
    }
};

// D3D11 video processor: crop + scale + rotate + colour-convert any input into an NV12 frame of the output size
struct Scaler {
    ComPtr<ID3D11VideoProcessorEnumerator> en;
    ComPtr<ID3D11VideoProcessor> vp;
    ComPtr<ID3D11Texture2D> out;
    ComPtr<ID3D11VideoProcessorOutputView> ov;

    bool Init(Gpu& g, int inW, int inH, int outW, int outH, RECT src, int rotation, bool rgbIn, bool bt709, bool fullRange) {
        en.Reset(); vp.Reset(); out.Reset(); ov.Reset();
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
        cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        cd.InputFrameRate = {30, 1};
        cd.InputWidth = UINT(inW);
        cd.InputHeight = UINT(inH);
        cd.OutputFrameRate = {30, 1};
        cd.OutputWidth = UINT(outW);
        cd.OutputHeight = UINT(outH);
        cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_QUALITY;
        if (FAILED(g.vdev->CreateVideoProcessorEnumerator(&cd, &en)) || FAILED(g.vdev->CreateVideoProcessor(en.Get(), 0, &vp))) return false;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = UINT(outW); td.Height = UINT(outH);
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_NV12;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC od{D3D11_VPOV_DIMENSION_TEXTURE2D};
        if (FAILED(g.dev->CreateTexture2D(&td, nullptr, &out)) || FAILED(g.vdev->CreateVideoProcessorOutputView(out.Get(), en.Get(), &od, &ov))) return false;
        RECT dst{0, 0, outW, outH};
        g.vctx->VideoProcessorSetStreamFrameFormat(vp.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        g.vctx->VideoProcessorSetStreamSourceRect(vp.Get(), 0, TRUE, &src);
        g.vctx->VideoProcessorSetStreamDestRect(vp.Get(), 0, TRUE, &dst);
        g.vctx->VideoProcessorSetOutputTargetRect(vp.Get(), TRUE, &dst);
        g.vctx->VideoProcessorSetStreamAutoProcessingMode(vp.Get(), 0, FALSE);   // no driver "enhancement": keep the art as is
        if (rotation) g.vctx->VideoProcessorSetStreamRotation(vp.Get(), 0, TRUE, D3D11_VIDEO_PROCESSOR_ROTATION(rotation / 90));
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE in{}, o{};
        if (!rgbIn) {
            in.YCbCr_Matrix = bt709;
            in.Nominal_Range = fullRange ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255 : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        }
        o.YCbCr_Matrix = 1;
        o.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        g.vctx->VideoProcessorSetStreamColorSpace(vp.Get(), 0, &in);
        g.vctx->VideoProcessorSetOutputColorSpace(vp.Get(), &o);
        return true;
    }

    bool Blt(Gpu& g, ID3D11Texture2D* tex, UINT slice) {
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC id{};
        id.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        id.Texture2D.ArraySlice = slice;
        ComPtr<ID3D11VideoProcessorInputView> iv;
        if (FAILED(g.vdev->CreateVideoProcessorInputView(tex, en.Get(), &id, &iv))) return false;
        D3D11_VIDEO_PROCESSOR_STREAM st{};
        st.Enable = TRUE;
        st.pInputSurface = iv.Get();
        return SUCCEEDED(g.vctx->VideoProcessorBlt(vp.Get(), ov.Get(), 0, 1, &st));
    }
};

static void TagBt709(IMFMediaType* t) {
    t->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    t->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    t->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    t->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
}

struct Encoder {
    ComPtr<IMFSinkWriter> sw;
    ComPtr<IMFVideoSampleAllocatorEx> alloc;
    ComPtr<ID3D11Texture2D> staging;
    DWORD stream = 0;
    bool hw = false;
    int w = 0, h = 0;

    HRESULT Open(Gpu& g, const wchar_t* path, int ow, int oh, UINT fpsN, UINT fpsD, UINT bitrate, bool useHw) {
        hw = useHw; w = ow; h = oh;
        ComPtr<IMFAttributes> at;
        MFCreateAttributes(&at, 3);
        at->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, hw);
        at->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
        if (hw) at->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, g.man.Get());
        HRESULT hr = MFCreateSinkWriterFromURL(path, nullptr, at.Get(), &sw);
        if (FAILED(hr)) return hr;
        ComPtr<IMFMediaType> o, in;
        MFCreateMediaType(&o);
        o->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        o->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        o->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
        o->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        o->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
        MFSetAttributeSize(o.Get(), MF_MT_FRAME_SIZE, UINT32(w), UINT32(h));
        MFSetAttributeRatio(o.Get(), MF_MT_FRAME_RATE, fpsN, fpsD);
        MFSetAttributeRatio(o.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        TagBt709(o.Get());
        if (FAILED(hr = sw->AddStream(o.Get(), &stream))) return hr;
        MFCreateMediaType(&in);
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, UINT32(w), UINT32(h));
        MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, fpsN, fpsD);
        MFSetAttributeRatio(in.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        TagBt709(in.Get());
        if (FAILED(hr = sw->SetInputMediaType(stream, in.Get(), nullptr))) return hr;
        ComPtr<ICodecAPI> api;   // best effort: quality-first VBR around the target bitrate
        if (SUCCEEDED(sw->GetServiceForStream(stream, GUID_NULL, IID_PPV_ARGS(&api)))) {
            VARIANT v;
            VariantInit(&v);
            v.vt = VT_UI4;
            v.ulVal = eAVEncCommonRateControlMode_UnconstrainedVBR;
            api->SetValue(&CODECAPI_AVEncCommonRateControlMode, &v);
            v.ulVal = bitrate;
            api->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &v);
            v.ulVal = 100;
            api->SetValue(&CODECAPI_AVEncCommonQualityVsSpeed, &v);
        }
        if (hw) {
            ComPtr<IMFAttributes> aa;
            MFCreateAttributes(&aa, 2);
            aa->SetUINT32(MF_SA_D3D11_BINDFLAGS, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
            aa->SetUINT32(MF_SA_D3D11_USAGE, D3D11_USAGE_DEFAULT);
            if (FAILED(hr = MFCreateVideoSampleAllocatorEx(IID_PPV_ARGS(&alloc))) || FAILED(hr = alloc->SetDirectXManager(g.man.Get())) ||
                FAILED(hr = alloc->InitializeSampleAllocatorEx(4, 16, aa.Get(), in.Get())))
                return hr;
        }
        return sw->BeginWriting();
    }

    HRESULT Write(Gpu& g, ID3D11Texture2D* nv12, LONGLONG t, LONGLONG d) {
        ComPtr<IMFSample> s;
        ComPtr<IMFMediaBuffer> b;
        HRESULT hr;
        if (hw) {
            for (int tries = 0; (hr = alloc->AllocateSample(&s)) == MF_E_SAMPLEALLOCATOR_EMPTY && tries < 5000; tries++) Sleep(1);
            if (FAILED(hr)) return hr;
            ComPtr<IMFDXGIBuffer> db;
            ComPtr<ID3D11Texture2D> tex;
            UINT sub = 0;
            if (FAILED(hr = s->GetBufferByIndex(0, &b)) || FAILED(hr = b.As(&db)) || FAILED(hr = db->GetResource(IID_PPV_ARGS(&tex)))) return hr;
            db->GetSubresourceIndex(&sub);
            g.ctx->CopySubresourceRegion(tex.Get(), sub, 0, 0, 0, nv12, 0, nullptr);
            DWORD ml = 0;
            b->GetMaxLength(&ml);
            b->SetCurrentLength(ml);
        } else {
            if (!staging) {
                D3D11_TEXTURE2D_DESC td{};
                td.Width = UINT(w); td.Height = UINT(h);
                td.MipLevels = td.ArraySize = 1;
                td.Format = DXGI_FORMAT_NV12;
                td.SampleDesc.Count = 1;
                td.Usage = D3D11_USAGE_STAGING;
                td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                if (FAILED(hr = g.dev->CreateTexture2D(&td, nullptr, &staging))) return hr;
            }
            g.ctx->CopyResource(staging.Get(), nv12);
            D3D11_MAPPED_SUBRESOURCE m;
            if (FAILED(hr = g.ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return hr;
            DWORD len = DWORD(w) * h * 3 / 2;
            BYTE* p = nullptr;
            if (SUCCEEDED(hr = MFCreateMemoryBuffer(len, &b)) && SUCCEEDED(hr = b->Lock(&p, nullptr, nullptr))) {
                const BYTE* src = (const BYTE*)m.pData;
                for (int y = 0; y < h; y++) memcpy(p + size_t(y) * w, src + size_t(y) * m.RowPitch, size_t(w));
                const BYTE* uv = src + size_t(m.RowPitch) * h;
                for (int y = 0; y < h / 2; y++) memcpy(p + size_t(w) * h + size_t(y) * w, uv + size_t(y) * m.RowPitch, size_t(w));
                b->Unlock();
                b->SetCurrentLength(len);
            }
            g.ctx->Unmap(staging.Get(), 0);
            if (FAILED(hr) || FAILED(hr = MFCreateSample(&s)) || FAILED(hr = s->AddBuffer(b.Get()))) return hr;
        }
        s->SetSampleTime(t);
        s->SetSampleDuration(d);
        return sw->WriteSample(stream, s.Get());
    }
};

static UINT Bitrate(int w, int h, double fps) {
    // generous at 24-30 fps (wallpapers are looked at up close), sub-linear above: 2.5K60 ~19 Mbps instead of ~37
    double bps = double(w) * h * 0.12 * std::sqrt(std::clamp(fps, 10.0, 60.0) * 24.0);
    return UINT(std::clamp(bps, 2e6, 60e6));
}

// ------------------------------------------------------------------ conversion
static HRESULT ConvertImage(Gpu& g, Kind k, const ConvertArgs& a, const wchar_t* part, bool hw, wchar_t* err, size_t n) {
    auto src = OpenImage(k, a.src, err, n);
    if (!src) return VW_E_SOURCE;
    int ow, oh;
    BakedSize(src->w, src->h, a.sw, a.sh, a.fit, &ow, &oh);
    double fx, fy;
    CropFraction(src->w, src->h, a.sw, a.sh, a.fit, &fx, &fy);
    Scaler sc;
    if (!sc.Init(g, src->w, src->h, ow, oh, CenteredCrop(RECT{0, 0, src->w, src->h}, fx, fy), 0, true, true, false)) {
        Msg(err, n, L"The graphics driver couldn't scale this image.");
        return E_FAIL;
    }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = UINT(src->w); td.Height = UINT(src->h);
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> up;
    if (FAILED(g.dev->CreateTexture2D(&td, nullptr, &up))) { Msg(err, n, L"Out of video memory."); return E_OUTOFMEMORY; }

    size_t frames = src->delays.size();
    LONGLONG total = 0;
    for (int d : src->delays) total += d;
    bool still = frames == 1;
    // short loops get repeated so the player's loop point (a seek) comes around rarely
    size_t reps = still ? 1 : std::max<size_t>(1, size_t((10000 + total - 1) / std::max<LONGLONG>(total, 1)));
    reps = std::max<size_t>(1, std::min(reps, 6000 / frames));
    double fps = still ? 1.0 : frames * 1000.0 / double(total);
    Encoder enc;
    HRESULT hr = enc.Open(g, part, ow, oh, still ? 1 : UINT(std::lround(fps * 1000)), still ? 1 : 1000, Bitrate(ow, oh, fps), hw);
    if (FAILED(hr)) { Msg(err, n, L"The video encoder couldn't start (0x%08X).", hr); return hr; }

    std::vector<uint8_t> px(size_t(src->w) * src->h * 4);
    size_t todo = still ? 10 : frames * reps, done = 0;
    LONGLONG t = 0;
    int lastPct = -1;
    for (size_t r = 0; r < reps; r++) {
        for (size_t i = 0; i < frames; i++) {
            if (*a.cancel) return E_ABORT;
            if (!src->Frame(int(i), px.data())) { Msg(err, n, L"Frame %zu of this image couldn't be decoded.", i + 1); return VW_E_SOURCE; }
            g.ctx->UpdateSubresource(up.Get(), 0, nullptr, px.data(), UINT(src->w) * 4, 0);
            if (!sc.Blt(g, up.Get(), 0)) { Msg(err, n, L"The graphics driver couldn't scale this image."); return E_FAIL; }
            int writes = still ? 10 : 1;   // a still image becomes 10 s of 1 fps video
            for (int k2 = 0; k2 < writes; k2++) {
                LONGLONG d = still ? 10000000LL : LONGLONG(src->delays[i]) * 10000;
                if (FAILED(hr = enc.Write(g, sc.out.Get(), t, d))) { Msg(err, n, L"Encoding failed (0x%08X).", hr); return hr; }
                t += d;
                int pct = int(++done * 100 / todo);
                if (pct != lastPct) { PostMessageW(a.notify, a.progressMsg, pct, 0); lastPct = pct; }
            }
        }
    }
    if (FAILED(hr = enc.sw->Finalize())) { Msg(err, n, L"Couldn't finish the video file (0x%08X).", hr); return hr; }
    return S_OK;
}

static HRESULT ConvertVideo(Gpu& g, const ConvertArgs& a, const wchar_t* part, bool hw, wchar_t* err, size_t n) {
    VideoInfo v;
    HRESULT hr = OpenVideo(a.src, g.man.Get(), v, err, n);
    if (FAILED(hr)) return hr;
    bool swap = v.rotation == 90 || v.rotation == 270;
    auto setup = [&](Scaler& sc, int& ow, int& oh) {
        int aw = v.ap.right - v.ap.left, ah = v.ap.bottom - v.ap.top;
        int dw = swap ? ah : aw, dh = swap ? aw : ah;
        BakedSize(dw, dh, a.sw, a.sh, a.fit, &ow, &oh);
        double fx, fy;
        CropFraction(dw, dh, a.sw, a.sh, a.fit, &fx, &fy);
        if (swap) std::swap(fx, fy);
        return sc.Init(g, v.fw, v.fh, ow, oh, CenteredCrop(v.ap, fx, fy), v.rotation, false, v.bt709, v.fullRange);
    };
    Scaler sc;
    int ow, oh;
    if (!setup(sc, ow, oh)) { Msg(err, n, L"The graphics driver couldn't scale this video."); return E_FAIL; }

    double fps = double(v.fpsN) / v.fpsD;
    UINT fN = v.fpsN, fD = v.fpsD;
    if (fps > 60.5) { fN = 60; fD = 1; }   // more than the screen can show
    LONGLONG minStep = fps > 60.5 ? 10000000LL / 60 - 20000 : 0;
    LONGLONG frameDur = LONGLONG(10000000.0 * fD / fN);
    Encoder enc;
    if (FAILED(hr = enc.Open(g, part, ow, oh, fN, fD, Bitrate(ow, oh, double(fN) / fD), hw))) {
        Msg(err, n, L"The video encoder couldn't start (0x%08X).", hr);
        return hr;
    }
    ComPtr<ID3D11Texture2D> upload, uploadStaging;   // only for decoders that hand back system-memory frames
    LONGLONG t0 = -1, lastT = LLONG_MIN;
    int lastPct = -1;
    for (;;) {
        if (*a.cancel) return E_ABORT;
        DWORD flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> s;
        if (FAILED(hr = v.r->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &ts, &s)) || (flags & MF_SOURCE_READERF_ERROR)) {
            Msg(err, n, L"Decoding failed partway through the video (0x%08X).", hr);
            return VW_E_SOURCE;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            int oldW = ow, oldH = oh;
            ReadFormat(v);
            upload.Reset(); uploadStaging.Reset();
            if (!setup(sc, ow, oh) || ow != oldW || oh != oldH) { Msg(err, n, L"This video changes size midway, which isn't supported."); return VW_E_SOURCE; }
        }
        if (!s) continue;
        if (t0 < 0) t0 = ts;
        LONGLONG t = ts - t0;
        if (minStep && lastT != LLONG_MIN && t - lastT < minStep) continue;
        ComPtr<IMFMediaBuffer> b;
        if (FAILED(s->GetBufferByIndex(0, &b))) continue;
        ComPtr<IMFDXGIBuffer> db;
        bool ok;
        if (SUCCEEDED(b.As(&db))) {
            ComPtr<ID3D11Texture2D> tex;
            UINT sub = 0;
            db->GetResource(IID_PPV_ARGS(&tex));
            db->GetSubresourceIndex(&sub);
            ok = tex && sc.Blt(g, tex.Get(), sub);
        } else {                                       // system-memory NV12: upload it
            if (!upload) {
                D3D11_TEXTURE2D_DESC td{};
                td.Width = UINT(v.fw); td.Height = UINT(v.fh);
                td.MipLevels = td.ArraySize = 1;
                td.Format = DXGI_FORMAT_NV12;
                td.SampleDesc.Count = 1;
                td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                g.dev->CreateTexture2D(&td, nullptr, &upload);
                td.BindFlags = 0;
                td.Usage = D3D11_USAGE_STAGING;
                td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                g.dev->CreateTexture2D(&td, nullptr, &uploadStaging);
            }
            ok = false;
            ComPtr<IMF2DBuffer> b2;
            BYTE* p = nullptr;
            LONG pitch = 0;
            D3D11_MAPPED_SUBRESOURCE m;
            if (upload && uploadStaging && SUCCEEDED(b.As(&b2)) && SUCCEEDED(b2->Lock2D(&p, &pitch))) {
                if (SUCCEEDED(g.ctx->Map(uploadStaging.Get(), 0, D3D11_MAP_WRITE, 0, &m))) {
                    BYTE* d = (BYTE*)m.pData;
                    for (int y = 0; y < v.fh; y++) memcpy(d + size_t(y) * m.RowPitch, p + size_t(y) * pitch, size_t(v.fw));
                    for (int y = 0; y < v.fh / 2; y++)
                        memcpy(d + size_t(m.RowPitch) * v.fh + size_t(y) * m.RowPitch, p + size_t(pitch) * v.fh + size_t(y) * pitch, size_t(v.fw));
                    g.ctx->Unmap(uploadStaging.Get(), 0);
                    g.ctx->CopyResource(upload.Get(), uploadStaging.Get());
                    ok = sc.Blt(g, upload.Get(), 0);
                }
                b2->Unlock2D();
            }
        }
        if (!ok) { Msg(err, n, L"The graphics driver couldn't scale a frame of this video."); return E_FAIL; }
        LONGLONG d = 0;
        if (FAILED(s->GetSampleDuration(&d)) || d <= 0) d = frameDur;
        if (minStep) d = std::max(d, minStep);
        if (FAILED(hr = enc.Write(g, sc.out.Get(), t, d))) { Msg(err, n, L"Encoding failed (0x%08X).", hr); return hr; }
        lastT = t;
        if (v.duration > 0) {
            int pct = int(std::clamp<LONGLONG>(t * 100 / v.duration, 0, 99));
            if (pct != lastPct) { PostMessageW(a.notify, a.progressMsg, pct, 0); lastPct = pct; }
        }
    }
    if (lastT == LLONG_MIN) { Msg(err, n, L"No frames could be decoded from this video."); return VW_E_SOURCE; }
    if (FAILED(hr = enc.sw->Finalize())) { Msg(err, n, L"Couldn't finish the video file (0x%08X).", hr); return hr; }
    PostMessageW(a.notify, a.progressMsg, 100, 0);
    return S_OK;
}

static HRESULT ConvertOnce(const ConvertArgs& a, const wchar_t* part, bool hw, wchar_t* err, size_t n) {
    Gpu g;
    if (!g.Init()) { Msg(err, n, L"This PC's graphics driver doesn't offer Direct3D 11 video processing."); return E_FAIL; }
    Kind k = Sniff(a.src);
    return k == K_VIDEO ? ConvertVideo(g, a, part, hw, err, n) : ConvertImage(g, k, a, part, hw, err, n);
}

HRESULT ConvertMedia(const ConvertArgs& a, wchar_t* err, size_t n) {
    wchar_t part[MAX_PATH + 16];
    swprintf_s(part, L"%s.part.mp4", a.dst);
    DeleteFileW(part);
    HRESULT hr = ConvertOnce(a, part, true, err, n);
    if (FAILED(hr) && hr != E_ABORT && hr != VW_E_SOURCE) {   // hardware encoder refused: software H.264 encoder
        DeleteFileW(part);
        hr = ConvertOnce(a, part, false, err, n);
    }
    if (SUCCEEDED(hr) && !MoveFileExW(part, a.dst, MOVEFILE_REPLACE_EXISTING)) {
        hr = HRESULT_FROM_WIN32(GetLastError());
        Msg(err, n, L"Couldn't save the converted video (0x%08X).", hr);
    }
    if (FAILED(hr)) DeleteFileW(part);
    return hr;
}
