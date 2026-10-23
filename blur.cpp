// Backdrop blur. Windows has no "blur the screen" switch, so this copies the screen (DXGI desktop duplication), blurs
// the copy on the GPU and shows it in a click-through, never-activated, topmost window per monitor. That window is
// excluded from capture (WDA_EXCLUDEFROMCAPTURE), so the copy never contains itself; screenshots and screen shares
// show the sharp screen. The colour filter (Magnification) is already baked into the copy and DWM applies it once
// more on top of this window, so the last pass maps each colour back to its closest preimage under that matrix.
// Everything lives on one thread that exists only once the blur has been used, and holds nothing while it's off.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "blur.h"
#include "filter.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <shellscalingapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <cstdio>
#include <vector>
#include "blur_vs.h"    // fxc output of blur.hlsl (build.bat)
#include "blur_psh.h"
#include "blur_psv.h"
#include "blur_psd.h"

#pragma comment(lib, "d3d11")
#pragma comment(lib, "dxgi")
#pragma comment(lib, "dcomp")
#pragma comment(lib, "shcore")

using Microsoft::WRL::ComPtr;

bool g_blurOn = false;
int g_blurStrength = 50;
bool g_blurKeyOk = false;
const wchar_t* g_blurKeyName = L"";

static const float kMaxSigmaDip = 1.6f;   // the cap: at 100% small text is soft but still readable
static const float kFadeMs = 300;
#ifdef VW_BLURTEST
static const bool kTest = true;    // test build: overlay visible to screenshots, frozen after its first frame
#else
static const bool kTest = false;
#endif
static const wchar_t* KEY = L"Software\\Backdrop";

static HWND g_notify;
static UINT g_failMsg;
static HANDLE g_thread, g_wakeEvt;
static std::atomic<bool> g_want{false}, g_rebuild{false};
static std::atomic<int> g_pct{50};

struct Pass {
    float mapU[4], mapV[4];
    float stepUV[2];
    int taps, pad;
    float tap[12][4];
    float fix[3][4];
};

// Our own presents count as screen updates for desktop duplication (with our window's area as dirty), even though
// the window itself isn't captured. Blurring every reported update would keep re-blurring the whole screen at the
// refresh rate, so each update is compared with the previous copy, tile by tile, and only real changes count.
static const int kTilesX = 12, kTilesY = 8;

struct Out {
    ComPtr<IDXGIOutput1> out;
    ComPtr<IDXGIOutputDuplication> dup;
    RECT rc{};
    DXGI_MODE_ROTATION rot = DXGI_MODE_ROTATION_IDENTITY;
    UINT dpi = 96;
    HWND hwnd = nullptr;
    ComPtr<IDXGISwapChain1> swap;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual> visual;
    ComPtr<ID3D11Texture2D> frame;   // latest screen copy, in the output's own (unrotated) orientation
    ComPtr<ID3D11ShaderResourceView> frameSrv;
    ComPtr<ID3D11Texture2D> incoming;   // only if the driver's frames can't be read by a shader: a copy of the new frame
    ComPtr<ID3D11ShaderResourceView> incomingSrv;
    ComPtr<ID3D11RenderTargetView> maskRtv;   // throwaway target for the comparison
    ComPtr<ID3D11RenderTargetView> tmpRtv;   // after the horizontal pass, screen orientation, 10 bits
    ComPtr<ID3D11ShaderResourceView> tmpSrv;
    std::vector<BYTE> meta;                  // move / dirty rects of the latest frame
    RECT box{}, prevBox{};                   // changed since the last render / changed in the last present (screen px)
    bool have = false, dirty = false, masked = false, shown = false, fresh = true;
    DWORD lastFrame = 0, retryAt = 0, lastReset = 0;
    int unsupported = 0;   // DuplicateOutput refusals in a row that waiting won't fix
};

struct Gpu {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IDCompositionDevice> dcomp;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> psh, psv, psd;
    ComPtr<ID3D11SamplerState> smp;
    ComPtr<ID3D11Buffer> cb;
    ComPtr<ID3D11RasterizerState> rs;
    ComPtr<ID3D11Query> q[kTilesX * kTilesY];
    std::vector<std::unique_ptr<Out>> outs;
};

static std::vector<std::unique_ptr<Gpu>> g_gpus;   // blur thread only

// ------------------------------------------------------------------ settings
static void LoadStrength() {
    DWORD d, sz = sizeof(d);
    if (RegGetValueW(HKEY_CURRENT_USER, KEY, L"BlurStrength", RRF_RT_REG_DWORD, nullptr, &d, &sz) == ERROR_SUCCESS) g_blurStrength = int(d);
    g_blurStrength = std::clamp(g_blurStrength, 10, 100);
    g_pct = g_blurStrength;
}

static void SaveStrength() {
    DWORD d = DWORD(g_blurStrength);
    RegSetKeyValueW(HKEY_CURRENT_USER, KEY, L"BlurStrength", REG_DWORD, &d, sizeof(d));
}

// ------------------------------------------------------------------ maths
// fix = the affine map that, for a blurred captured colour c (already through the filter F(x) = M x + b), gives the
// colour G closest to c with F(G) = c: G = c + P (c - b - M c), P = (M'M + lambda I)^-1 M'. Exactly F^-1 when M is
// invertible; for greyscale / sepia / night red at 100% it keeps whatever F can't see from c itself.
static void ColourFix(float fix[3][4]) {
    float mf[3][3], of[3];
    FilterCurrent(mf, of);
    double M[3][3], N[3][3], inv[3][3], P[3][3];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) M[i][j] = mf[i][j];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            N[i][j] = i == j ? 1e-3 : 0;
            for (int k = 0; k < 3; k++) N[i][j] += M[k][i] * M[k][j];
        }
    double det = N[0][0] * (N[1][1] * N[2][2] - N[1][2] * N[2][1]) - N[0][1] * (N[1][0] * N[2][2] - N[1][2] * N[2][0]) +
                 N[0][2] * (N[1][0] * N[2][1] - N[1][1] * N[2][0]);
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            int a = (j + 1) % 3, b = (j + 2) % 3, c = (i + 1) % 3, d = (i + 2) % 3;
            inv[i][j] = (N[a][c] * N[b][d] - N[a][d] * N[b][c]) / det;   // cofactor, transposed
        }
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            P[i][j] = 0;
            for (int k = 0; k < 3; k++) P[i][j] += inv[i][k] * M[j][k];
        }
    for (int i = 0; i < 3; i++) {
        double off = 0;
        for (int j = 0; j < 3; j++) {
            double pm = 0;
            for (int k = 0; k < 3; k++) pm += P[i][k] * M[k][j];
            fix[i][j] = float((i == j) - pm + P[i][j]);
            off -= P[i][j] * of[j];
        }
        fix[i][3] = float(off);
    }
}

static void Add(RECT& a, const RECT& b) {
    if (IsRectEmpty(&b)) return;
    if (IsRectEmpty(&a)) a = b;
    else UnionRect(&a, &a, &b);
}

static int Kernel(float sigma, Pass* p) {   // returns the radius in pixels
    p->taps = 1;
    p->tap[0][0] = 1;
    p->tap[0][1] = 0;
    if (sigma < 0.05f) return 0;
    int r = std::min(22, int(std::ceil(sigma * 3)));
    float g[24], sum = 0;
    for (int k = 0; k <= r; k++) sum += (g[k] = std::exp(-k * k / (2 * sigma * sigma))) * (k ? 2 : 1);
    p->tap[0][0] = g[0] / sum;
    for (int j = 1; j <= r; j += 2) {   // two neighbouring taps in one bilinear fetch
        float a = g[j] / sum, b = j + 1 <= r ? g[j + 1] / sum : 0;
        p->tap[p->taps][0] = a + b;
        p->tap[p->taps][1] = (j * a + (j + 1) * b) / (a + b);
        p->taps++;
    }
    return r;
}

// ------------------------------------------------------------------ windows
static LRESULT CALLBACK OverlayProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_DISPLAYCHANGE: case WM_DPICHANGED: g_rebuild = true; return 0;
    case WM_ERASEBKGND: return 1;
    }
    return DefWindowProcW(h, m, w, l);
}

static void Show(Out& o, bool show) {
    if (o.shown == show) return;
    o.shown = show;
    if (show) SetWindowPos(o.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    else ShowWindow(o.hwnd, SW_HIDE);
}

// something new came to the front (a topmost app, the taskbar): stay above it
static void CALLBACK OnForeground(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
    for (auto& g : g_gpus)
        for (auto& o : g->outs)
            if (o->shown) SetWindowPos(o->hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

static void Teardown() {
    for (auto& g : g_gpus)
        for (auto& o : g->outs)
            if (o->hwnd) DestroyWindow(o->hwnd);
    g_gpus.clear();
}

// 0 ok, 1 retry later (no outputs / device trouble), 2 this PC can't exclude windows from capture
static int Build() {
    ComPtr<IDXGIFactory1> f;
    ComPtr<IDXGIFactory2> f2;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f))) || FAILED(f.As(&f2))) return 1;
    ComPtr<IDXGIAdapter1> ad;
    for (UINT a = 0; f->EnumAdapters1(a, &ad) != DXGI_ERROR_NOT_FOUND; a++, ad.Reset()) {
        std::unique_ptr<Gpu> g;
        ComPtr<IDXGIOutput> out;
        for (UINT i = 0; ad->EnumOutputs(i, &out) != DXGI_ERROR_NOT_FOUND; i++, out.Reset()) {
            DXGI_OUTPUT_DESC od;
            if (FAILED(out->GetDesc(&od)) || !od.AttachedToDesktop) continue;
            if (!g) {
                g.reset(new Gpu);
                const D3D_FEATURE_LEVEL fl[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
                ComPtr<IDXGIDevice> xd;
                if (FAILED(D3D11CreateDevice(ad.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, fl, 3,
                                             D3D11_SDK_VERSION, &g->dev, nullptr, &g->ctx)) ||
                    FAILED(g->dev.As(&xd)) || FAILED(DCompositionCreateDevice(xd.Get(), IID_PPV_ARGS(&g->dcomp))))
                    return 1;
                g->dev->CreateVertexShader(g_vs, sizeof(g_vs), nullptr, &g->vs);
                g->dev->CreatePixelShader(g_psh, sizeof(g_psh), nullptr, &g->psh);
                g->dev->CreatePixelShader(g_psv, sizeof(g_psv), nullptr, &g->psv);
                g->dev->CreatePixelShader(g_psd, sizeof(g_psd), nullptr, &g->psd);
                D3D11_QUERY_DESC qd{D3D11_QUERY_OCCLUSION};
                for (auto& q : g->q)
                    if (FAILED(g->dev->CreateQuery(&qd, &q))) return 1;
                D3D11_SAMPLER_DESC sd{D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP};
                sd.MaxLOD = D3D11_FLOAT32_MAX;
                g->dev->CreateSamplerState(&sd, &g->smp);
                D3D11_BUFFER_DESC bd{(sizeof(Pass) + 15) & ~15u, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE};
                g->dev->CreateBuffer(&bd, nullptr, &g->cb);
                D3D11_RASTERIZER_DESC rd{D3D11_FILL_SOLID, D3D11_CULL_NONE};
                rd.DepthClipEnable = TRUE;
                rd.ScissorEnable = TRUE;   // only what changed gets blurred again
                g->dev->CreateRasterizerState(&rd, &g->rs);
                if (!g->vs || !g->psh || !g->psv || !g->psd || !g->smp || !g->cb || !g->rs) return 1;
            }
            auto o = std::make_unique<Out>();
            out.As(&o->out);
            if (!o->out) continue;
            o->rc = od.DesktopCoordinates;
            o->rot = od.Rotation;
            UINT dx = 96, dy = 96;
            if (SUCCEEDED(GetDpiForMonitor(od.Monitor, MDT_EFFECTIVE_DPI, &dx, &dy))) o->dpi = dx;
            int w = o->rc.right - o->rc.left, h = o->rc.bottom - o->rc.top;
            o->hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_NOREDIRECTIONBITMAP,
                                      L"BackdropBlur", L"", WS_POPUP, o->rc.left, o->rc.top, w, h, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (!o->hwnd) return 1;
            SetPropW(o->hwnd, L"NonRudeHWND", (HANDLE)TRUE);   // not a full-screen app: notifications keep coming
            Out& r = *o;
            g->outs.push_back(std::move(o));   // owned from here on, so Teardown destroys the window
            // without this the copy would contain the blurred copy, blurring itself a bit more every frame
            if (!kTest && !SetWindowDisplayAffinity(r.hwnd, WDA_EXCLUDEFROMCAPTURE)) return 2;
            DXGI_SWAP_CHAIN_DESC1 sc{};
            sc.Width = UINT(w);
            sc.Height = UINT(h);
            sc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            sc.SampleDesc.Count = 1;
            sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sc.BufferCount = 2;
            sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            sc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;   // opaque: DWM needn't draw what's underneath for the screen
            ComPtr<ID3D11Texture2D> bb;
            if (FAILED(f2->CreateSwapChainForComposition(g->dev.Get(), &sc, nullptr, &r.swap)) || FAILED(r.swap->GetBuffer(0, IID_PPV_ARGS(&bb))) ||
                FAILED(g->dev->CreateRenderTargetView(bb.Get(), nullptr, &r.rtv)) ||
                FAILED(g->dcomp->CreateTargetForHwnd(r.hwnd, TRUE, &r.target)) || FAILED(g->dcomp->CreateVisual(&r.visual)))
                return 1;
            r.visual->SetContent(r.swap.Get());
            r.target->SetRoot(r.visual.Get());
            D3D11_TEXTURE2D_DESC td{UINT(w), UINT(h), 1, 1, DXGI_FORMAT_R10G10B10A2_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                                    D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE};
            ComPtr<ID3D11Texture2D> tmp;
            if (FAILED(g->dev->CreateTexture2D(&td, nullptr, &tmp)) || FAILED(g->dev->CreateRenderTargetView(tmp.Get(), nullptr, &r.tmpRtv)) ||
                FAILED(g->dev->CreateShaderResourceView(tmp.Get(), nullptr, &r.tmpSrv)))
                return 1;
        }
        if (g) {
            g->dcomp->Commit();
            g_gpus.push_back(std::move(g));
        }
    }
    return g_gpus.empty() ? 1 : 0;
}

// ------------------------------------------------------------------ frames
static void Lost(Out& o, DWORD now, DWORD retryMs) {
    o.dup.Reset();
    o.fresh = true;
    o.have = false;
    o.retryAt = now + retryMs;
    Show(o, false);   // never leave an old picture over a live screen
}

// which part of the reported area r really differs between the new frame (src) and frame (copy orientation, pixels);
// those tiles are copied into frame
static RECT Changed(Gpu& g, Out& o, const RECT& r, UINT w, UINT h, ID3D11Resource* src, ID3D11ShaderResourceView* srcSrv) {
    D3D11_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.ctx->IASetInputLayout(nullptr);
    g.ctx->VSSetShader(g.vs.Get(), nullptr, 0);
    g.ctx->PSSetShader(g.psd.Get(), nullptr, 0);
    g.ctx->RSSetViewports(1, &vp);
    g.ctx->RSSetState(g.rs.Get());
    g.ctx->OMSetRenderTargets(1, o.maskRtv.GetAddressOf(), nullptr);
    ID3D11ShaderResourceView* srv[2] = {srcSrv, o.frameSrv.Get()};
    g.ctx->PSSetShaderResources(0, 2, srv);
    RECT tiles[kTilesX * kTilesY];
    bool used[kTilesX * kTilesY]{};
    for (int ty = 0; ty < kTilesY; ty++)
        for (int tx = 0; tx < kTilesX; tx++) {
            int k = ty * kTilesX + tx;
            RECT t{LONG(w * tx / kTilesX), LONG(h * ty / kTilesY), LONG(w * (tx + 1) / kTilesX), LONG(h * (ty + 1) / kTilesY)};
            if (!IntersectRect(&tiles[k], &t, &r)) continue;
            used[k] = true;
            g.ctx->RSSetScissorRects(1, &tiles[k]);
            g.ctx->Begin(g.q[k].Get());
            g.ctx->Draw(3, 0);   // pixels that differ survive; identical ones are discarded
            g.ctx->End(g.q[k].Get());
        }
    ID3D11ShaderResourceView* none[2] = {};
    g.ctx->PSSetShaderResources(0, 2, none);
    g.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    g.ctx->Flush();
    RECT out{};
    for (int k = 0; k < kTilesX * kTilesY; k++) {
        if (!used[k]) continue;
        UINT64 n = 1;
        HRESULT hr;
        while ((hr = g.ctx->GetData(g.q[k].Get(), &n, sizeof(n), 0)) == S_FALSE) SwitchToThread();
        if (FAILED(hr) || n) {   // take this tile over; unchanged tiles keep what was last blurred, so slow fades add up
            D3D11_BOX bx{UINT(tiles[k].left), UINT(tiles[k].top), 0, UINT(tiles[k].right), UINT(tiles[k].bottom), 1};
            g.ctx->CopySubresourceRegion(o.frame.Get(), 0, tiles[k].left, tiles[k].top, 0, src, 0, &bx);
            Add(out, tiles[k]);
        }
    }
    return out;
}

// the colour filter is changing: frames still arrive composed with older matrices for a moment, and the change per
// frame can be smaller than the compare's threshold, so take everything reported as it comes until it settles
static DWORD g_fixAt;
static const DWORD kSettleMs = 250;

static void Acquire(Gpu& g, Out& o, UINT timeout, DWORD now) {
    if (kTest && o.have) return;
    if (!o.dup) {
        if (int(now - o.retryAt) < 0) return;
        HRESULT hr = o.out->DuplicateOutput(g.dev.Get(), &o.dup);
        o.unsupported = hr == DXGI_ERROR_UNSUPPORTED || hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE ? o.unsupported + 1 : 0;
        if (FAILED(hr)) { Lost(o, now, 500); return; }   // secure desktop, mode change in progress, ...
        o.lastFrame = now;
    }
    DXGI_OUTDUPL_FRAME_INFO fi;
    ComPtr<IDXGIResource> res;
    HRESULT hr = o.dup->AcquireNextFrame(timeout, &fi, &res);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        // nothing changed on screen. If the user is clicking or typing but no frame has come for a while, get a fresh
        // duplication anyway (it starts with a full frame): insurance against ever showing a stuck picture
        LASTINPUTINFO li{sizeof(li)};
        if (o.shown && now - o.lastFrame > 3000 && now - o.lastReset > 5000 && GetLastInputInfo(&li) && int(li.dwTime - o.lastFrame) > 0) {
            o.lastReset = now;
            o.dup.Reset();
            o.fresh = true;
            o.retryAt = now;
        }
        return;
    }
    if (FAILED(hr)) {
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) g_rebuild = true;
        Lost(o, now, 100);
        return;
    }
    if (fi.LastPresentTime.QuadPart) {   // the picture changed (not just the pointer)
        ComPtr<ID3D11Texture2D> tex;
        res.As(&tex);
        D3D11_TEXTURE2D_DESC d{};
        if (tex) tex->GetDesc(&d);
        D3D11_TEXTURE2D_DESC have{};
        if (o.frame) o.frame->GetDesc(&have);
        if (tex && (!o.frame || have.Width != d.Width || have.Height != d.Height || have.Format != d.Format)) {
            o.frame.Reset();
            o.frameSrv.Reset();
            o.incoming.Reset();
            o.incomingSrv.Reset();
            o.maskRtv.Reset();
            D3D11_TEXTURE2D_DESC td{d.Width, d.Height, 1, 1, d.Format, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE};
            D3D11_TEXTURE2D_DESC md{d.Width, d.Height, 1, 1, DXGI_FORMAT_R8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET};
            ComPtr<ID3D11Texture2D> mask;
            if (FAILED(g.dev->CreateTexture2D(&td, nullptr, &o.frame)) || FAILED(g.dev->CreateShaderResourceView(o.frame.Get(), nullptr, &o.frameSrv)) ||
                FAILED(g.dev->CreateTexture2D(&md, nullptr, &mask)) || FAILED(g.dev->CreateRenderTargetView(mask.Get(), nullptr, &o.maskRtv)))
                o.frame.Reset();
            o.fresh = true;   // a new copy texture: fill all of it
        }
        if (tex && o.frame) {
            // copy only what changed: the move and dirty rects of this frame (in the copy's own orientation)
            RECT b{}, all{0, 0, LONG(d.Width), LONG(d.Height)};
            bool full = o.fresh || o.rot > DXGI_MODE_ROTATION_IDENTITY || !fi.TotalMetadataBufferSize;
            if (!full) {
                if (o.meta.size() < fi.TotalMetadataBufferSize) o.meta.resize(fi.TotalMetadataBufferSize);
                UINT mv = 0, dr = 0, size = UINT(o.meta.size());
                if (SUCCEEDED(o.dup->GetFrameMoveRects(size, (DXGI_OUTDUPL_MOVE_RECT*)o.meta.data(), &mv)) &&
                    SUCCEEDED(o.dup->GetFrameDirtyRects(size - mv, (RECT*)(o.meta.data() + mv), &dr))) {
                    auto* m = (const DXGI_OUTDUPL_MOVE_RECT*)o.meta.data();
                    for (UINT i = 0; i < mv / sizeof(*m); i++) Add(b, m[i].DestinationRect);
                    auto* r = (const RECT*)(o.meta.data() + mv);
                    for (UINT i = 0; i < dr / sizeof(*r); i++) Add(b, r[i]);
                    IntersectRect(&b, &b, &all);
                } else full = true;
            }
            if (full) b = all;
            if (!IsRectEmpty(&b)) {
                D3D11_BOX bx{UINT(b.left), UINT(b.top), 0, UINT(b.right), UINT(b.bottom), 1};
                if (o.fresh || now - g_fixAt < kSettleMs) g.ctx->CopySubresourceRegion(o.frame.Get(), 0, b.left, b.top, 0, tex.Get(), 0, &bx);
                else {
                    ComPtr<ID3D11ShaderResourceView> srv;   // compare straight from the frame when the driver allows it
                    if (!(d.BindFlags & D3D11_BIND_SHADER_RESOURCE) || FAILED(g.dev->CreateShaderResourceView(tex.Get(), nullptr, &srv))) {
                        D3D11_TEXTURE2D_DESC td{d.Width, d.Height, 1, 1, d.Format, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE};
                        if (!o.incoming && (FAILED(g.dev->CreateTexture2D(&td, nullptr, &o.incoming)) ||
                                            FAILED(g.dev->CreateShaderResourceView(o.incoming.Get(), nullptr, &o.incomingSrv))))
                            o.incoming.Reset();
                        if (o.incoming) g.ctx->CopySubresourceRegion(o.incoming.Get(), 0, b.left, b.top, 0, tex.Get(), 0, &bx);
                    }
                    if (srv) b = Changed(g, o, b, d.Width, d.Height, tex.Get(), srv.Get());
                    else if (o.incoming) b = Changed(g, o, b, d.Width, d.Height, o.incoming.Get(), o.incomingSrv.Get());
                    else g.ctx->CopySubresourceRegion(o.frame.Get(), 0, b.left, b.top, 0, tex.Get(), 0, &bx);
                }
            }
            if (!IsRectEmpty(&b)) {
                if (o.rot > DXGI_MODE_ROTATION_IDENTITY) b = {0, 0, o.rc.right - o.rc.left, o.rc.bottom - o.rc.top};
                Add(o.box, b);
                o.have = o.dirty = true;
            }
            o.fresh = false;
        }
        o.masked = fi.ProtectedContentMaskedOut != FALSE;   // DRM video shows black in the copy: step aside for it
        o.lastFrame = now;
    }
    o.dup->ReleaseFrame();
}

#ifdef VW_BLURDUMP
// test build: the back buffer as raw BGRA (int32 width, int32 height, rows) in %TEMP%
static void Dump(Gpu& g, Out& o, const wchar_t* name) {
    ComPtr<ID3D11Texture2D> bb, st;
    o.swap->GetBuffer(0, IID_PPV_ARGS(&bb));
    D3D11_TEXTURE2D_DESC d;
    bb->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    if (FAILED(g.dev->CreateTexture2D(&d, nullptr, &st))) return;
    g.ctx->CopyResource(st.Get(), bb.Get());
    D3D11_MAPPED_SUBRESOURCE ms;
    if (FAILED(g.ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &ms))) return;
    wchar_t path[MAX_PATH];
    GetTempPathW(MAX_PATH, path);
    wcscat_s(path, name);
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD n;
    int wh[2] = {int(d.Width), int(d.Height)};
    WriteFile(f, wh, sizeof(wh), &n, nullptr);
    for (UINT y = 0; y < d.Height; y++) WriteFile(f, (BYTE*)ms.pData + y * ms.RowPitch, d.Width * 4, &n, nullptr);
    CloseHandle(f);
    g.ctx->Unmap(st.Get(), 0);
}
#endif

static void Render(Gpu& g, Out& o, float sigmaDip, const float fix[3][4], bool all) {
    int w = o.rc.right - o.rc.left, h = o.rc.bottom - o.rc.top;
    Pass p{};
    int grow = 2 * Kernel(sigmaDip * o.dpi / 96.0f, &p) + 2;
    // what to blur again: the changed area plus what it bleeds into (all of it after a strength or filter change).
    // The back buffer we get holds the picture from two presents ago, so last present's area is redrawn as well.
    RECT full{0, 0, w, h}, b = o.box, b2;
    if (all) b = full;
    InflateRect(&b, grow, grow);
    IntersectRect(&b, &b, &full);
    o.box = {};
    if (IsRectEmpty(&b)) { o.dirty = false; return; }
    b2 = b;
    Add(b2, o.prevBox);
    memcpy(p.fix, fix, sizeof(p.fix));
    // screen uv -> uv in the copy, which comes in the output's unrotated orientation
    static const float maps[4][6] = {{1, 0, 0, 0, 1, 0}, {0, 1, 0, -1, 0, 1}, {-1, 0, 1, 0, -1, 1}, {0, -1, 1, 1, 0, 0}};
    int ri = o.rot == DXGI_MODE_ROTATION_ROTATE90 ? 1 : o.rot == DXGI_MODE_ROTATION_ROTATE180 ? 2 : o.rot == DXGI_MODE_ROTATION_ROTATE270 ? 3 : 0;
    D3D11_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.ctx->IASetInputLayout(nullptr);
    g.ctx->VSSetShader(g.vs.Get(), nullptr, 0);
    g.ctx->PSSetSamplers(0, 1, g.smp.GetAddressOf());
    g.ctx->PSSetConstantBuffers(0, 1, g.cb.GetAddressOf());
    g.ctx->RSSetViewports(1, &vp);
    g.ctx->RSSetState(g.rs.Get());
    auto draw = [&](const RECT& b, const RECT& b2) {
    for (int pass = 0; pass < 2; pass++) {
        g.ctx->RSSetScissorRects(1, pass ? &b2 : &b);
        const float* m = maps[pass ? 0 : ri];
        memcpy(p.mapU, m, 3 * sizeof(float));
        memcpy(p.mapV, m + 3, 3 * sizeof(float));
        p.stepUV[0] = pass ? 0 : m[0] / w;   // one screen pixel along x (pass 0) or y (pass 1), in source uv
        p.stepUV[1] = pass ? 1.0f / h : m[3] / w;
        D3D11_MAPPED_SUBRESOURCE ms;
        if (FAILED(g.ctx->Map(g.cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) return false;
        memcpy(ms.pData, &p, sizeof(p));
        g.ctx->Unmap(g.cb.Get(), 0);
        ID3D11ShaderResourceView* none = nullptr;
        g.ctx->PSSetShaderResources(0, 1, &none);
        g.ctx->OMSetRenderTargets(1, pass ? o.rtv.GetAddressOf() : o.tmpRtv.GetAddressOf(), nullptr);
        g.ctx->PSSetShaderResources(0, 1, pass ? o.tmpSrv.GetAddressOf() : o.frameSrv.GetAddressOf());
        g.ctx->PSSetShader(pass ? g.psv.Get() : g.psh.Get(), nullptr, 0);
        g.ctx->Draw(3, 0);
    }
    ID3D11ShaderResourceView* none = nullptr;
    g.ctx->PSSetShaderResources(0, 1, &none);
    g.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    return true;
    };
    if (!draw(b, b2)) return;
#ifdef VW_BLURDUMP
    wchar_t req[MAX_PATH];
    GetTempPathW(MAX_PATH, req);
    wcscat_s(req, L"blurdump.req");
    if (!all && GetFileAttributesW(req) != INVALID_FILE_ATTRIBUTES) {   // what was just drawn, then the same from scratch
        Dump(g, o, L"blurdump_inc.raw");
        draw(full, full);
        Dump(g, o, L"blurdump_full.raw");
        DeleteFileW(req);
    }
#endif
    DXGI_PRESENT_PARAMETERS pp{1, &b, nullptr, nullptr};
    HRESULT hr = o.swap->Present1(1, 0, &pp);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) { g_rebuild = true; return; }
    o.prevBox = b;
    o.dirty = false;
#ifdef VW_LOG
    static DWORD t0 = GetTickCount(), frames;
    static double area;
    frames++;
    area += double(b2.right - b2.left) * (b2.bottom - b2.top) / (double(w) * h);
    if (GetTickCount() - t0 > 5000) {
        wchar_t lp[MAX_PATH];
        FILE* lf;
        GetTempPathW(MAX_PATH, lp);
        wcscat_s(lp, L"backdrop.log");
        if (!_wfopen_s(&lf, lp, L"a")) { fwprintf(lf, L"blur: %.1f renders/s, %.0f%% of the screen each\n", frames / 5.0, 100 * area / frames); fclose(lf); }
        t0 = GetTickCount(); frames = 0; area = 0;
    }
#endif
    Show(o, true);
}

static void Pump() {
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
}

static DWORD WINAPI BlurThread(void*) {
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);   // the copy should trail the screen as little as possible
    WNDCLASSW wc{};
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"BackdropBlur";
    RegisterClassW(&wc);
    HWINEVENTHOOK hook = nullptr;
    bool built = false;
    float f = 0;   // fade: 0 = sharp (or hidden), 1 = full strength
    DWORD last = GetTickCount(), fails = 0;
    float lastSigma = -1, lastFix[3][4]{};
    for (;;) {
        DWORD now = GetTickCount();
        float dt = float(now - last) / kFadeMs;
        last = now;
        bool want = g_want;
        f = std::clamp(f + (want ? dt : -dt), 0.0f, 1.0f);
        if (!want && f <= 0) {   // off: give everything back and sleep until asked again
            if (hook) { UnhookWinEvent(hook); hook = nullptr; }
            if (built) { Teardown(); built = false; }
            lastSigma = -1;
            MsgWaitForMultipleObjects(1, &g_wakeEvt, FALSE, INFINITE, QS_ALLINPUT);
            Pump();
            last = GetTickCount();
            continue;
        }
        if (!built || g_rebuild.exchange(false)) {
            Teardown();
            int r = Build();
            built = r == 0;
            if (!built) {
                Teardown();
                if (r == 2 || ++fails > 20) {   // can't be done here (or keeps failing): switch off and say so
                    g_want = false;
                    f = 0;
                    PostMessageW(g_notify, g_failMsg, r, 0);
                    fails = 0;
                } else MsgWaitForMultipleObjects(1, &g_wakeEvt, FALSE, 500, QS_ALLINPUT);
                Pump();
                continue;
            }
            fails = 0;
            lastSigma = -1;
            if (!hook) hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, OnForeground, 0, 0, WINEVENT_OUTOFCONTEXT);
        }
        Pump();
        float e = f * f * (3 - 2 * f);
        float sigma = e * kMaxSigmaDip * g_pct / 100.0f;
        float fix[3][4];
        ColourFix(fix);
        bool fixChanged = memcmp(fix, lastFix, sizeof(fix)) != 0, changed = fixChanged || sigma != lastSigma;
        if (fixChanged) g_fixAt = now;
        lastSigma = sigma;
        memcpy(lastFix, fix, sizeof(fix));
        bool fading = (want && f < 1) || (!want && f > 0);
        size_t n = 0;
        for (auto& g : g_gpus) n += g->outs.size();
        UINT timeout = fading ? 0 : UINT(std::max<size_t>(1, 16 / std::max<size_t>(1, n)));
        bool any = false, hopeless = true;
        for (auto& g : g_gpus)
            for (auto& o : g->outs) {
                Acquire(*g, *o, timeout, now);
                any |= o->dup != nullptr;
                hopeless &= o->unsupported >= 10;
                if (o->masked) { Show(*o, false); continue; }
                if (o->have && (o->dirty || changed)) Render(*g, *o, sigma, fix, changed || !o->shown);
            }
        if (hopeless) {   // no display here can be copied (driver / HDR / other capture tools): off, and say so
            g_want = false;
            f = 0;
            PostMessageW(g_notify, g_failMsg, 1, 0);
            continue;
        }
        if (fading) MsgWaitForMultipleObjects(0, nullptr, FALSE, 8, QS_ALLINPUT);
        else if (!any) MsgWaitForMultipleObjects(1, &g_wakeEvt, FALSE, 100, QS_ALLINPUT);
    }
}

// ------------------------------------------------------------------ public
void BlurInit(HWND notify, UINT failMsg) {
    g_notify = notify;
    g_failMsg = failMsg;
    LoadStrength();
}

// safety net for turning it off: if the blur thread were ever stuck, its last picture would stay over the screen.
// Hide its windows from here without waiting on that thread.
static void CALLBACK OffCheck(HWND, UINT, UINT_PTR id, DWORD) {
    KillTimer(nullptr, id);
    if (g_want || !g_thread) return;
    EnumThreadWindows(GetThreadId(g_thread), [](HWND h, LPARAM) -> BOOL {
        wchar_t c[32];
        if (IsWindowVisible(h) && GetClassNameW(h, c, 32) && !wcscmp(c, L"BackdropBlur")) ShowWindowAsync(h, SW_HIDE);
        return TRUE;
    }, 0);
}

void BlurSetOn(bool on) {
    g_blurOn = on;
    g_want = on;
    if (!on && g_thread) SetTimer(nullptr, 0, 1500, OffCheck);   // the fade out takes 300 ms
    if (on && !g_thread) {
        g_wakeEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g_thread = CreateThread(nullptr, 0, BlurThread, nullptr, 0, nullptr);
    }
    if (g_wakeEvt) SetEvent(g_wakeEvt);
}

void BlurSetStrength(int pct) {
    g_blurStrength = std::clamp(pct, 10, 100);   // the cap is enforced here, whatever the UI sends
    g_pct = g_blurStrength;
    SaveStrength();
    if (g_wakeEvt) SetEvent(g_wakeEvt);
}

void BlurShutdown() {
    g_want = false;   // the windows go with the process
    if (g_wakeEvt) SetEvent(g_wakeEvt);
}
