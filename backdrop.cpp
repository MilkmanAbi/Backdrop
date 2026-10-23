// Backdrop - video wallpaper + whole-screen colour filters for Windows 10/11, as one small tray app.
// Tray icon: Colour filter on/off, Colour filters... (filter.cpp / panel.cpp), Video wallpaper on/off, Choose video or GIF...,
// Fit, Uninstall, Exit. Win+Alt+C (or a fallback combo) toggles the colour filter. Settings in HKCU\Software\Backdrop.
// Desktop icons on/off (tray menu, Win+Alt+H or a fallback combo, /icons) uses Explorer's own "Show desktop icons" switch.
// Blur (blur.cpp): a slight whole-screen blur alongside the filter (tray menu, Win+Alt+Z or a fallback combo, /blur); always
// starts off.
// Command line: /on /off /toggle /exit /filter /filters /icons /blur (forwarded to the running copy; no argument pops its menu),
//               /install /uninstall (used by the installer: logon task, cleanup).
// Media is converted once (convert.cpp) into an H.264 MP4 baked at the display's resolution for the chosen fit;
// playback is Media Foundation IMFMediaEngine (hardware decode) -> D3D11 flip swap chain in a window that sits
// between the desktop icons and the wallpaper.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define SECURITY_WIN32
#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>
#include <commctrl.h>
#include <wtsapi32.h>
#include <dwmapi.h>
#include <security.h>
#include <taskschd.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfmediaengine.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwchar>
#include <string>
#include <uxtheme.h>
#include "blur.h"
#include "convert.h"
#include "filter.h"
#include "panel.h"
#include "theme.h"

#pragma comment(lib, "user32")
#pragma comment(lib, "gdi32")
#pragma comment(lib, "shell32")
#pragma comment(lib, "comdlg32")
#pragma comment(lib, "comctl32")
#pragma comment(lib, "advapi32")
#pragma comment(lib, "ole32")
#pragma comment(lib, "oleaut32")
#pragma comment(lib, "wtsapi32")
#pragma comment(lib, "dwmapi")
#pragma comment(lib, "uxtheme")
#pragma comment(lib, "secur32")
#pragma comment(lib, "taskschd")
#pragma comment(lib, "d3d11")
#pragma comment(lib, "dxgi")
#pragma comment(lib, "mfplat")
#pragma comment(lib, "mfuuid")

using Microsoft::WRL::ComPtr;

#ifdef VW_LOG
#include <cstdio>
#define LOG(...) do { wchar_t lp[MAX_PATH]; FILE* lf; GetTempPathW(MAX_PATH, lp); wcscat_s(lp, L"backdrop.log"); \
    if (!_wfopen_s(&lf, lp, L"a")) { fwprintf(lf, L"%lu ", GetTickCount()); fwprintf(lf, __VA_ARGS__); fputwc(L'\n', lf); fclose(lf); } } while (0)
#else
#define LOG(...) do {} while (0)
#endif

enum : UINT { WM_TRAY = WM_APP + 1, WM_SHOWMENU, WM_MFERROR, WM_SETSTATE, WM_CONVPROG, WM_CONVDONE, WM_CHOOSE, WM_BLURFAIL };   // WM_SETSTATE: 0 off, 1 on, 2 toggle, 3 exit, 4 filter toggle, 5 filter window, 6 desktop icons toggle, 7 blur toggle
enum : UINT { ID_TOGGLE = 1, ID_CHOOSE, ID_EXIT, ID_UNINSTALL, ID_CANCEL, ID_FILTER, ID_FILTERS, ID_ICONS, ID_APP, ID_BLUR, ID_FIT0 = 20 };
enum : int { HK_FILTER = 1, HK_ICONS, HK_BLUR };
static const wchar_t* g_iconsKeyName = nullptr;   // shortcut that toggles desktop icons, if one could be registered
enum : UINT_PTR { T_WATCH = 1, T_PIN, T_WIDGETS };
static const UINT IPC_COMMAND = 0xBD01, IPC_STATE = 0xBD02;   // WM_COPYDATA from / to backdrop-ui.exe
static const DWORD CONVERTER_VERSION = 1;   // bump to invalidate every cached conversion

static HINSTANCE g_inst;
static HANDLE g_mutex;
static HWND g_main, g_host, g_video, g_parent;
static UINT g_taskbarCreated;
static NOTIFYICONDATAW g_nid;
static bool g_enabled = true, g_locked = false, g_displayOff = false;
static DWORD g_pinned = 0, g_asked = 0, g_playFor = 0, g_playFit = 0;
static int g_pinTries = 0;
static wchar_t g_src[MAX_PATH];    // what the user picked
static wchar_t g_play[MAX_PATH];   // what actually plays (the baked conversion, or the source itself)
static std::atomic<int> g_fit{FIT_FILL};

// shared with the render thread (guarded by g_cs)
static CRITICAL_SECTION g_cs;
static ComPtr<ID3D11Device> g_dev;
static ComPtr<IMFDXGIDeviceManager> g_dxman;
static ComPtr<IMFMediaEngine> g_engine;
static ComPtr<IDXGISwapChain1> g_swap;
static ComPtr<IDXGIOutput> g_output;
static UINT g_w, g_h;
static std::atomic<bool> g_playing{false};
static std::atomic<bool> g_redraw{false};   // draw the current frame even while paused (first frame right at attach)
static std::atomic<bool> g_ready{false};    // engine has a real decoded frame (never present black before that)
static HANDLE g_wake;

// ------------------------------------------------------------------ small helpers
static const wchar_t* KEY = L"Software\\Backdrop";

static bool Exists(const wchar_t* p) { return p[0] && GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }

static void ExePath(wchar_t* out) { GetModuleFileNameW(nullptr, out, MAX_PATH); }

static void DataDir(wchar_t* out, const wchar_t* sub) {   // %LOCALAPPDATA%\Backdrop[\sub]
    GetEnvironmentVariableW(L"LOCALAPPDATA", out, MAX_PATH);
    wcscat_s(out, MAX_PATH, L"\\Backdrop");
    CreateDirectoryW(out, nullptr);
    if (sub) { wcscat_s(out, MAX_PATH, L"\\"); wcscat_s(out, MAX_PATH, sub); CreateDirectoryW(out, nullptr); }
}

// physical pixels of the primary display's current mode
static void DisplaySize(int* w, int* h) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmPelsWidth && dm.dmPelsHeight) {
        *w = int(dm.dmPelsWidth); *h = int(dm.dmPelsHeight);
    } else {
        *w = GetSystemMetrics(SM_CXSCREEN); *h = GetSystemMetrics(SM_CYSCREEN);
    }
}

static DWORD PackSize(int w, int h) { return DWORD(w) << 16 | DWORD(h); }

static bool IsImageFile(const wchar_t* p) {
    const wchar_t* e = wcsrchr(p, L'.');
    return e && (!_wcsicmp(e, L".gif") || !_wcsicmp(e, L".webp"));
}

// ------------------------------------------------------------------ settings
static void LoadSettings() {
    DWORD sz = sizeof(g_src), v = 0, vs = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, KEY, L"Video", RRF_RT_REG_SZ, nullptr, g_src, &sz) != ERROR_SUCCESS) g_src[0] = 0;
    sz = sizeof(g_play);
    if (RegGetValueW(HKEY_CURRENT_USER, KEY, L"Play", RRF_RT_REG_SZ, nullptr, g_play, &sz) != ERROR_SUCCESS) g_play[0] = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, KEY, L"Enabled", RRF_RT_REG_DWORD, nullptr, &v, &vs) == ERROR_SUCCESS) g_enabled = v != 0;
    vs = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, KEY, L"Fit", RRF_RT_REG_DWORD, nullptr, &v, &vs) == ERROR_SUCCESS && v <= FIT_CENTER) g_fit = int(v);
    vs = sizeof(DWORD);
    RegGetValueW(HKEY_CURRENT_USER, KEY, L"PlayFor", RRF_RT_REG_DWORD, nullptr, &g_playFor, &vs);
    vs = sizeof(DWORD);
    RegGetValueW(HKEY_CURRENT_USER, KEY, L"PlayFit", RRF_RT_REG_DWORD, nullptr, &g_playFit, &vs);
    vs = sizeof(DWORD);
    RegGetValueW(HKEY_CURRENT_USER, KEY, L"Pinned", RRF_RT_REG_DWORD, nullptr, &g_pinned, &vs);
    vs = sizeof(DWORD);
    RegGetValueW(HKEY_CURRENT_USER, KEY, L"Asked", RRF_RT_REG_DWORD, nullptr, &g_asked, &vs);
}

static void SaveSettings() {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, KEY, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    DWORD en = g_enabled, fit = DWORD(g_fit.load());
    RegSetValueExW(k, L"Video", 0, REG_SZ, (const BYTE*)g_src, DWORD((wcslen(g_src) + 1) * sizeof(wchar_t)));
    RegSetValueExW(k, L"Play", 0, REG_SZ, (const BYTE*)g_play, DWORD((wcslen(g_play) + 1) * sizeof(wchar_t)));
    RegSetValueExW(k, L"Enabled", 0, REG_DWORD, (const BYTE*)&en, sizeof(en));
    RegSetValueExW(k, L"Fit", 0, REG_DWORD, (const BYTE*)&fit, sizeof(fit));
    RegSetValueExW(k, L"PlayFor", 0, REG_DWORD, (const BYTE*)&g_playFor, sizeof(DWORD));
    RegSetValueExW(k, L"PlayFit", 0, REG_DWORD, (const BYTE*)&g_playFit, sizeof(DWORD));
    RegSetValueExW(k, L"Pinned", 0, REG_DWORD, (const BYTE*)&g_pinned, sizeof(DWORD));
    RegSetValueExW(k, L"Asked", 0, REG_DWORD, (const BYTE*)&g_asked, sizeof(DWORD));
    RegCloseKey(k);
}

// ------------------------------------------------------------------ media engine
struct Notify : IMFMediaEngineNotify {
    LONG ref = 1;
    STDMETHODIMP QueryInterface(REFIID r, void** p) override {
        if (r == __uuidof(IUnknown) || r == __uuidof(IMFMediaEngineNotify)) { *p = this; AddRef(); return S_OK; }
        *p = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref); }
    STDMETHODIMP_(ULONG) Release() override { LONG r = InterlockedDecrement(&ref); if (!r) delete this; return r; }
    STDMETHODIMP EventNotify(DWORD ev, DWORD_PTR, DWORD) override {
        if (ev == MF_MEDIA_ENGINE_EVENT_ERROR) PostMessageW(g_main, WM_MFERROR, 0, 0);
        if (ev == MF_MEDIA_ENGINE_EVENT_FIRSTFRAMEREADY) { g_ready = true; g_redraw = true; SetEvent(g_wake); }
        return S_OK;
    }
};

static bool InitD3D() {
    if (g_dev) return true;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &g_dev, nullptr, nullptr)))
        return false;
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(g_dev.As(&mt))) mt->SetMultithreadProtected(TRUE);
    UINT token;
    if (FAILED(MFCreateDXGIDeviceManager(&token, &g_dxman)) || FAILED(g_dxman->ResetDevice(g_dev.Get(), token))) return false;
    ComPtr<IDXGIDevice> dd;
    ComPtr<IDXGIAdapter> ad;
    ComPtr<IDXGIOutput> out;
    if (SUCCEEDED(g_dev.As(&dd)) && SUCCEEDED(dd->GetAdapter(&ad))) ad->EnumOutputs(0, &out);
    EnterCriticalSection(&g_cs);
    g_output = out;
    LeaveCriticalSection(&g_cs);
    return true;
}

// when off, drop the GPU device too so the idle tray app stays tiny
static void ReleaseD3D() {
    EnterCriticalSection(&g_cs);
    g_output.Reset(); g_dxman.Reset(); g_dev.Reset();
    LeaveCriticalSection(&g_cs);
    SetProcessWorkingSetSize(GetCurrentProcess(), SIZE_T(-1), SIZE_T(-1));
}

static void StopEngine() {
    EnterCriticalSection(&g_cs);
    g_playing = false;
    if (g_engine) { g_engine->Shutdown(); g_engine.Reset(); }
    LeaveCriticalSection(&g_cs);
}

// the baked file if there is one, else the source itself when the engine can play it directly
static const wchar_t* PlayPath() {
    if (Exists(g_play)) return g_play;
    if (Exists(g_src) && !IsImageFile(g_src)) return g_src;
    return nullptr;
}

static bool StartEngine() {
    StopEngine();
    g_ready = false;
    const wchar_t* path = PlayPath();
    if (!path || !InitD3D()) return false;
    ComPtr<IMFMediaEngineClassFactory> fac;
    ComPtr<IMFAttributes> at;
    ComPtr<IMFMediaEngine> eng;
    if (FAILED(CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac)))) return false;
    MFCreateAttributes(&at, 3);
    Notify* n = new Notify;
    at->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, n);
    n->Release();
    at->SetUnknown(MF_MEDIA_ENGINE_DXGI_MANAGER, g_dxman.Get());
    at->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM);
    if (FAILED(fac->CreateInstance(0, at.Get(), &eng))) return false;
    eng->SetMuted(TRUE);
    eng->SetLoop(TRUE);
    eng->SetAutoPlay(FALSE);
    BSTR url = SysAllocString(path);
    HRESULT hr = eng->SetSource(url);
    SysFreeString(url);
    if (FAILED(hr)) { eng->Shutdown(); return false; }
    EnterCriticalSection(&g_cs);
    g_engine = eng;
    LeaveCriticalSection(&g_cs);
    return true;
}

static void SetPlaying(bool play) {
    EnterCriticalSection(&g_cs);
    if (g_engine) {
        if (play && !g_playing) g_engine->Play();
        if (!play && g_playing) g_engine->Pause();
        g_playing = play;
    }
    LeaveCriticalSection(&g_cs);
    if (play) SetEvent(g_wake);
}

// where a vw x vh frame lands on a W x H screen for the fit; a baked file comes out 1:1
static bool Layout(DWORD vw, DWORD vh, UINT W, UINT H, int fit, MFVideoNormalizedRect* src, RECT* dst) {
    *src = {0, 0, 1, 1};
    *dst = {0, 0, LONG(W), LONG(H)};
    if (fit == FIT_STRETCH) return true;
    double s = fit == FIT_FILL ? std::max(double(W) / vw, double(H) / vh) : fit == FIT_FIT ? std::min(double(W) / vw, double(H) / vh) : 1.0;
    double dw = vw * s, dh = vh * s;
    if (std::fabs(dw - vw) <= 2 && std::fabs(dh - vh) <= 2) { dw = vw; dh = vh; }
    double cw = std::min(1.0, W / dw), ch = std::min(1.0, H / dh);
    *src = {float((1 - cw) / 2), float((1 - ch) / 2), float((1 + cw) / 2), float((1 + ch) / 2)};
    LONG ow = LONG(std::min<double>(dw, W) + 0.5), oh = LONG(std::min<double>(dh, H) + 0.5);
    LONG x = (LONG(W) - ow) / 2, y = (LONG(H) - oh) / 2;
    *dst = {x, y, x + ow, y + oh};
    return ow >= LONG(W) && oh >= LONG(H);   // covers the whole screen
}

// render thread: on every vblank, if the engine has a new frame, put it in the swap chain
static DWORD WINAPI RenderThread(void*) {
    for (;;) {
        if (!g_playing && !(g_redraw && g_ready)) { WaitForSingleObject(g_wake, 1000); continue; }
        ComPtr<IDXGIOutput> out;
        EnterCriticalSection(&g_cs);
        out = g_output;
        LeaveCriticalSection(&g_cs);
        if (!out || FAILED(out->WaitForVBlank())) Sleep(8);
        EnterCriticalSection(&g_cs);
        LONGLONG pts;
        if (!g_engine || !g_swap) g_redraw = false;
        bool fresh = g_engine && g_swap && g_engine->OnVideoStreamTick(&pts) == S_OK;
        if (fresh) g_ready = true;
        if (fresh || (g_redraw && g_ready)) {
            DWORD vw = 0, vh = 0;
            g_engine->GetNativeVideoSize(&vw, &vh);
            ComPtr<ID3D11Texture2D> bb;
            if (vw && vh && SUCCEEDED(g_swap->GetBuffer(0, IID_PPV_ARGS(&bb)))) {
                MFVideoNormalizedRect src;
                RECT dst;
                if (!Layout(vw, vh, g_w, g_h, g_fit, &src, &dst)) {   // bars: clear the back buffer first
                    ComPtr<ID3D11RenderTargetView> rtv;
                    ComPtr<ID3D11DeviceContext> ctx;
                    g_dev->GetImmediateContext(&ctx);
                    const float black[4] = {0, 0, 0, 1};
                    if (SUCCEEDED(g_dev->CreateRenderTargetView(bb.Get(), nullptr, &rtv))) ctx->ClearRenderTargetView(rtv.Get(), black);
                }
                MFARGB border{0, 0, 0, 255};
                if (SUCCEEDED(g_engine->TransferVideoFrame(bb.Get(), &src, &dst, &border))) {
                    g_swap->Present(0, 0);
                    if (g_redraw.exchange(false)) SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);   // started above-normal for login
                }
            }
        }
        LeaveCriticalSection(&g_cs);
    }
}

// ------------------------------------------------------------------ desktop placement
static HWND Progman() { return FindWindowW(L"Progman", nullptr); }

// 24H2+ "raised desktop": Progman hosts icons + wallpaper itself and has no redirection bitmap
static bool RaisedDesktop(HWND pm) { return (GetWindowLongPtrW(pm, GWL_EXSTYLE) & WS_EX_NOREDIRECTIONBITMAP) != 0; }

static HWND FindDefView() {
    HWND found = nullptr;
    EnumWindows([](HWND w, LPARAM p) -> BOOL {
        if (FindWindowExW(w, nullptr, L"SHELLDLL_DefView", nullptr)) { *(HWND*)p = w; return FALSE; }
        return TRUE;
    }, (LPARAM)&found);
    return found;   // top-level window that holds the desktop icons
}

// ---- desktop icons: Explorer's own "View > Show desktop icons" switch (same as the right-click menu, so it stays in
// sync with Windows and survives restarts). 0x7402 is that menu command on the desktop's SHELLDLL_DefView.
static HWND DesktopIconView(HWND* defView) {
    HWND host = FindDefView();
    HWND dv = host ? FindWindowExW(host, nullptr, L"SHELLDLL_DefView", nullptr) : nullptr;
    if (defView) *defView = dv;
    return dv ? FindWindowExW(dv, nullptr, L"SysListView32", nullptr) : nullptr;
}

static bool DesktopIconsShown() {
    HWND lv = DesktopIconView(nullptr);
    return !lv || IsWindowVisible(lv);
}

static void ToggleDesktopIcons() {
    HWND dv = nullptr;
    DesktopIconView(&dv);
    if (dv) SendMessageTimeoutW(dv, WM_COMMAND, 0x7402, 0, SMTO_ABORTIFHUNG, 2000, nullptr);
}

static void Detach() {
    EnterCriticalSection(&g_cs);
    g_swap.Reset();
    LeaveCriticalSection(&g_cs);
    HWND h = g_host;
    g_host = g_video = g_parent = nullptr;
    if (h && IsWindow(h)) DestroyWindow(h);
}

static RECT PrimaryRectIn(HWND parent) {
    RECT r{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    MapWindowPoints(nullptr, parent, (POINT*)&r, 2);
    return r;
}

// keep the wallpaper image (WorkerW children of Progman) underneath our window
static void CALLBACK WorkersToBottom(HWND pm, UINT, ULONG_PTR, LRESULT) {
    for (HWND wk = nullptr; (wk = FindWindowExW(pm, wk, L"WorkerW", nullptr));)
        SetWindowPos(wk, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static bool Attach() {
    HWND pm = Progman();
    if (!pm) return false;
    bool raised = RaisedDesktop(pm);
    HWND parent = nullptr, icons = nullptr;
    DWORD_PTR res;
    if (raised) {
        if (!(icons = FindWindowExW(pm, nullptr, L"SHELLDLL_DefView", nullptr))) return false;   // desktop not up yet
        parent = pm;
    } else {                                                                  // Win10 / Win11 before 24H2
        if (!FindDefView()) return false;
        SendMessageTimeoutW(pm, 0x052C, 0, 0, SMTO_NORMAL, 1000, &res);       // split icons and wallpaper into two WorkerWs
        HWND iconHost = FindDefView();
        parent = iconHost ? FindWindowExW(nullptr, iconHost, L"WorkerW", nullptr) : nullptr;   // the one right behind the icons
        if (!parent) { parent = pm; icons = FindWindowExW(pm, nullptr, L"SHELLDLL_DefView", nullptr); }
    }
    Detach();
    RECT r = PrimaryRectIn(parent);
    int w = r.right - r.left, h = r.bottom - r.top;
    if (raised) {   // the raised desktop only composes layered children
        g_host = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"BackdropHost", L"", WS_POPUP, 0, 0, w, h, nullptr, nullptr, g_inst, nullptr);
        SetLayeredWindowAttributes(g_host, 0, 255, LWA_ALPHA);
        SetParent(g_host, parent);
        SetWindowLongPtrW(g_host, GWL_STYLE, WS_CHILD | WS_CLIPCHILDREN);
        SetWindowPos(g_host, nullptr, r.left, r.top, w, h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    } else {
        g_host = CreateWindowExW(WS_EX_NOACTIVATE, L"BackdropHost", L"", WS_CHILD | WS_CLIPCHILDREN, r.left, r.top, w, h, parent, nullptr, g_inst, nullptr);
    }
    if (!g_host) return false;
    g_video = CreateWindowExW(0, L"BackdropHost", L"", WS_CHILD | WS_VISIBLE, 0, 0, w, h, g_host, nullptr, g_inst, nullptr);

    ComPtr<IDXGIDevice> dd;
    ComPtr<IDXGIAdapter> ad;
    ComPtr<IDXGIFactory2> f;
    g_dev.As(&dd);
    dd->GetAdapter(&ad);
    ad->GetParent(IID_PPV_ARGS(&f));
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = UINT(w);
    sd.Height = UINT(h);
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> sc;
    if (FAILED(f->CreateSwapChainForHwnd(g_dev.Get(), g_video, &sd, nullptr, nullptr, &sc))) { Detach(); return false; }
    f->MakeWindowAssociation(g_video, DXGI_MWA_NO_ALT_ENTER);
    EnterCriticalSection(&g_cs);
    g_swap = sc;
    g_w = UINT(w);
    g_h = UINT(h);
    LeaveCriticalSection(&g_cs);

    ShowWindow(g_host, SW_SHOWNA);
    if (icons) SetWindowPos(g_host, icons, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);   // just below the icons
    if (raised) {
        WorkersToBottom(pm, 0, 0, 0);
        // spawn the wallpaper WorkerW (child of Progman) without waiting on Explorer, which is busy right after login
        if (!FindWindowExW(pm, nullptr, L"WorkerW", nullptr)) SendMessageCallbackW(pm, 0x052C, 0xD, 1, WorkersToBottom, 0);
    }
    g_parent = parent;
    g_redraw = true;
    SetEvent(g_wake);
    return true;
}

static bool Attached() {
    return g_host && IsWindow(g_host) && g_parent && IsWindow(g_parent) && GetParent(g_host) == g_parent && Progman();
}

// pause while a maximized / fullscreen window covers the desktop
static bool DesktopCovered() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId()) return false;   // our own windows (the host can end up foreground after the file dialog)
    fg = GetAncestor(fg, GA_ROOT);                    // children of the desktop count as the desktop
    wchar_t cls[64];
    GetClassNameW(fg, cls, 64);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd")) return false;
    BOOL cloaked = FALSE;
    DwmGetWindowAttribute(fg, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked || !IsWindowVisible(fg) || IsIconic(fg)) return false;
    if (IsZoomed(fg)) return true;
    RECT wr;
    MONITORINFO mi{sizeof(mi)};
    GetWindowRect(fg, &wr);
    GetMonitorInfoW(MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST), &mi);
    return wr.left <= mi.rcMonitor.left && wr.top <= mi.rcMonitor.top && wr.right >= mi.rcMonitor.right && wr.bottom >= mi.rcMonitor.bottom;
}

static void Watch() {
    if (!g_enabled || !g_engine) return;
    if (!Attached()) {
        if (!Attach()) { SetTimer(g_main, T_WATCH, 10, nullptr); return; }   // desktop not ready (login): poll fast
        SetTimer(g_main, T_WATCH, 1000, nullptr);
    }
    SetPlaying(!g_locked && !g_displayOff && !DesktopCovered());
}

// ------------------------------------------------------------------ on / off
struct Job {
    wchar_t src[MAX_PATH], dst[MAX_PATH];
    int sw, sh;
    Fit fit;
    bool silent;
    volatile LONG cancel;
    HRESULT hr;
    wchar_t err[512];
    int pct;
};
static Job* g_job;

static void UpdateTip() {
    wchar_t f[64];
    if (g_filterOn) swprintf_s(f, L"%s %d%%", FilterName(g_fs.type), g_fs.intensity);
    else wcscpy_s(f, L"off");
    if (g_blurOn) swprintf_s(f + wcslen(f), 64 - wcslen(f), L"\nBlur: %d%%", g_blurStrength);
    if (g_job) swprintf_s(g_nid.szTip, L"Backdrop\nPreparing wallpaper %d%%\nColour filter: %s", g_job->pct, f);
    else swprintf_s(g_nid.szTip, L"Backdrop\nWallpaper: %s\nColour filter: %s", g_enabled ? L"on" : L"off", f);
    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void TurnOn() {
    if (!StartEngine()) return;
    Watch();
}

static void TurnOff() {
    KillTimer(g_main, T_WATCH);
    StopEngine();
    Detach();
    ReleaseD3D();
}

static void EnsureBaked();

static void ApplyState() {
    if (g_enabled && PlayPath()) TurnOn(); else TurnOff();
    UpdateTip();
    EnsureBaked();   // switched on after a display change: fix the bake now
}

// ------------------------------------------------------------------ conversion jobs
static HWND g_prog, g_progBar, g_progText;
static HFONT g_font;
static HBRUSH g_progBrush;

// progress window colours follow the light / dark app theme
static void ThemeProgress() {
    if (!g_prog) return;
    bool dark = ThemeIsDark();
    if (g_progBrush) DeleteObject(g_progBrush);
    g_progBrush = CreateSolidBrush(dark ? RGB(0x20, 0x20, 0x20) : RGB(0xF3, 0xF3, 0xF3));
    ThemeTitleBar(g_prog);
    SetWindowTheme(g_progBar, L"", L"");   // classic look so the colours below apply
    SendMessageW(g_progBar, PBM_SETBARCOLOR, 0, ThemeAccent(dark));
    SendMessageW(g_progBar, PBM_SETBKCOLOR, 0, dark ? RGB(0x3A, 0x3A, 0x3A) : RGB(0xDD, 0xDD, 0xDD));
    if (HWND b = GetDlgItem(g_prog, ID_CANCEL)) SetWindowTheme(b, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    InvalidateRect(g_prog, nullptr, TRUE);
}

static void CachePath(const wchar_t* src, int sw, int sh, Fit fit, wchar_t* out) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    GetFileAttributesExW(src, GetFileExInfoStandard, &fa);
    unsigned long long hsh = 1469598103934665603ULL;   // FNV-1a over path + size + mtime + target + version
    auto mix = [&](const void* p, size_t n) { for (size_t i = 0; i < n; i++) { hsh ^= ((const BYTE*)p)[i]; hsh *= 1099511628211ULL; } };
    for (const wchar_t* c = src; *c; c++) { wchar_t l = towlower(*c); mix(&l, sizeof(l)); }
    DWORD extra[] = {fa.nFileSizeLow, fa.nFileSizeHigh, fa.ftLastWriteTime.dwLowDateTime, fa.ftLastWriteTime.dwHighDateTime, CONVERTER_VERSION};
    mix(extra, sizeof(extra));
    wchar_t dir[MAX_PATH];
    DataDir(dir, L"cache");
    swprintf_s(out, MAX_PATH, L"%s\\%016llx_%dx%d_%d.mp4", dir, hsh, sw, sh, int(fit));
}

// keep every bake of the current file (switching fit back is instant), drop other files' bakes
static void PruneCache(const wchar_t* keep) {
    const wchar_t* name = wcsrchr(keep, L'\\');
    size_t prefix = name ? 17 : 0;   // "<16 hex>_"
    wchar_t dir[MAX_PATH], pat[MAX_PATH], f[MAX_PATH];
    DataDir(dir, L"cache");
    swprintf_s(pat, L"%s\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        swprintf_s(f, L"%s\\%s", dir, fd.cFileName);
        bool same = prefix && !_wcsnicmp(fd.cFileName, name + 1, prefix) && !wcsstr(fd.cFileName, L".part");
        if (!same) DeleteFileW(f);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void CloseProgress() {
    if (g_prog) { HWND p = g_prog; g_prog = nullptr; DestroyWindow(p); }
}

static void CancelJob() {
    if (g_job) { InterlockedExchange(&g_job->cancel, 1); g_job = nullptr; }   // its thread finishes and the done message frees it
    CloseProgress();
}

static void ShowProgress(const Job* j) {
    if (g_prog) return;
    UINT dpi = GetDpiForSystem();
    auto S = [dpi](int v) { return MulDiv(v, int(dpi), 96); };
    NONCLIENTMETRICSW nm{sizeof(nm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(nm), &nm, 0, dpi);
    if (!g_font) g_font = CreateFontIndirectW(&nm.lfMessageFont);
    RECT rc{0, 0, S(400), S(118)};
    AdjustWindowRectExForDpi(&rc, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME, dpi);
    int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    g_prog = CreateWindowExW(WS_EX_DLGMODALFRAME, L"BackdropProgress", L"Backdrop", WS_CAPTION | WS_SYSMENU | WS_POPUP,
                             (wa.left + wa.right - ww) / 2, (wa.top + wa.bottom - wh) / 2, ww, wh, nullptr, nullptr, g_inst, nullptr);
    wchar_t t[160];
    swprintf_s(t, L"Preparing your wallpaper for this %d × %d display…", j->sw, j->sh);
    g_progText = CreateWindowExW(0, L"STATIC", t, WS_CHILD | WS_VISIBLE | SS_LEFT, S(16), S(16), S(368), S(22), g_prog, nullptr, g_inst, nullptr);
    g_progBar = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE | PBS_SMOOTH, S(16), S(44), S(368), S(16), g_prog, nullptr, g_inst, nullptr);
    HWND b = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, S(296), S(76), S(88), S(28), g_prog, (HMENU)ID_CANCEL, g_inst, nullptr);
    for (HWND c : {g_progText, b}) SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    ThemeProgress();
    ShowWindow(g_prog, SW_SHOWNORMAL);
    SetForegroundWindow(g_prog);
}

static DWORD WINAPI JobThread(void* p) {
    Job* j = (Job*)p;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);   // never fight the desktop for the CPU
    ConvertArgs a{j->src, j->dst, j->sw, j->sh, j->fit, g_main, WM_CONVPROG, &j->cancel};
    j->hr = ConvertMedia(a, j->err, 512);
    CoUninitialize();
    PostMessageW(g_main, WM_CONVDONE, 0, (LPARAM)j);
    return 0;
}

// convert src for the current display + fit (or reuse the cache / the file itself), then switch to it
static void StartJob(const wchar_t* src, Fit fit, bool silent, const Probe* probe) {
    CancelJob();
    Job* j = new Job{};
    wcscpy_s(j->src, src);
    DisplaySize(&j->sw, &j->sh);
    j->fit = fit;
    j->silent = silent;
    Probe p;
    wchar_t err[256];
    if (!probe && ProbeMedia(src, p, err, 256)) probe = &p;
    if (probe && PlayableAsIs(src, *probe, j->sw, j->sh, fit)) wcscpy_s(j->dst, src);   // already perfect: play the file itself
    else CachePath(src, j->sw, j->sh, fit, j->dst);
    if (Exists(j->dst)) { j->hr = S_OK; g_job = j; PostMessageW(g_main, WM_CONVDONE, 0, (LPARAM)j); return; }
    g_job = j;
    if (!silent) ShowProgress(j);
    UpdateTip();
    CloseHandle(CreateThread(nullptr, 0, JobThread, j, 0, nullptr));
}

static void JobDone(Job* j) {
    if (j != g_job) { delete j; return; }   // cancelled or superseded
    g_job = nullptr;
    CloseProgress();
    if (SUCCEEDED(j->hr)) {
        wcscpy_s(g_src, j->src);
        wcscpy_s(g_play, j->dst);
        g_playFor = PackSize(j->sw, j->sh);
        g_playFit = DWORD(j->fit);
        g_fit = j->fit;
        if (!j->silent) g_enabled = true;   // a background re-bake must never switch the wallpaper back on
        SaveSettings();
        PruneCache(g_play);
        ApplyState();
    } else if (j->hr != E_ABORT && !j->silent) {
        wchar_t m[700];
        swprintf_s(m, L"Couldn't prepare this file as a wallpaper.\n\n%s", j->err);
        MessageBoxW(g_main, m, L"Backdrop", MB_ICONWARNING);
    }
    UpdateTip();
    delete j;
}

// at start-up / display change: make sure the baked file matches this display and fit
static void EnsureBaked() {
    if (!g_enabled || !g_src[0] || g_job || !Exists(g_src)) return;
    int sw, sh;
    DisplaySize(&sw, &sh);
    if (Exists(g_play) && g_playFor == PackSize(sw, sh) && g_playFit == DWORD(g_fit.load())) return;
    StartJob(g_src, Fit(g_fit.load()), true, nullptr);
}

static void Choose() {
    wchar_t buf[MAX_PATH];
    wcscpy_s(buf, g_src);
    OPENFILENAMEW of{sizeof(of)};
    of.hwndOwner = g_main;
    of.lpstrFilter = L"Videos and animations\0*.mp4;*.mov;*.avi;*.gif;*.webp;*.m4v;*.mkv;*.wmv\0All files\0*.*\0";
    of.lpstrFile = buf;
    of.nMaxFile = MAX_PATH;
    of.lpstrTitle = L"Choose a video or GIF for the wallpaper";
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;   // never lock the user's folder
    if (!GetOpenFileNameW(&of)) return;
    Probe p;
    wchar_t err[512];
    if (!ProbeMedia(buf, p, err, 512)) {
        wchar_t m[700];
        swprintf_s(m, L"Couldn't use this file as a wallpaper.\n\n%s", err);
        MessageBoxW(g_main, m, L"Backdrop", MB_ICONWARNING);
        return;
    }
    int sw, sh;
    DisplaySize(&sw, &sh);
    if (Upscale(p.w, p.h, sw, sh, Fit(g_fit.load())) >= 2.0) {
        wchar_t m[400];
        swprintf_s(m, L"Resolution low, proceed? Visual quality will be poor.\n\n"
                      L"This file is %d × %d.\nThe ideal resolution is your display's: %d × %d.", p.w, p.h, sw, sh);
        if (MessageBoxW(g_main, m, L"Backdrop", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    }
    StartJob(buf, Fit(g_fit.load()), false, &p);
}

static void SetFit(Fit f) {
    if (f == g_fit && !g_job) return;
    if (!g_src[0]) { g_fit = f; SaveSettings(); return; }
    StartJob(g_src, f, false, nullptr);   // g_fit switches when the new bake is ready
}

// ------------------------------------------------------------------ install / uninstall / tray pin
struct Bstr {
    BSTR b;
    explicit Bstr(const wchar_t* s) : b(SysAllocString(s)) {}
    ~Bstr() { SysFreeString(b); }
    operator BSTR() const { return b; }
};

static void TaskName(wchar_t* out, size_t n) {
    wchar_t u[128];
    DWORD l = 128;
    if (!GetUserNameW(u, &l)) wcscpy_s(u, L"user");
    swprintf_s(out, n, L"Backdrop (%s)", u);
}

// logon task: fires the moment this user signs in (no Run-key startup delay), above-normal priority
static bool SetLogonTask(bool add) {
    ComPtr<ITaskService> svc;
    ComPtr<ITaskFolder> root;
    VARIANT none;
    VariantInit(&none);
    if (FAILED(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&svc))) ||
        FAILED(svc->Connect(none, none, none, none)) || FAILED(svc->GetFolder(Bstr(L"\\"), &root)))
        return false;
    wchar_t name[200];
    TaskName(name, 200);
    root->DeleteTask(Bstr(L"VidWall"), 0);   // tasks from when the app was called VidWall
    {
        wchar_t old[200], u[128];
        DWORD l = 128;
        if (GetUserNameW(u, &l)) { swprintf_s(old, L"VidWall (%s)", u); root->DeleteTask(Bstr(old), 0); }
    }
    if (!add) { root->DeleteTask(Bstr(name), 0); return true; }
    wchar_t user[256], exe[MAX_PATH];
    ULONG ul = 256;
    if (!GetUserNameExW(NameSamCompatible, user, &ul)) return false;
    ExePath(exe);
    ComPtr<ITaskDefinition> def;
    ComPtr<IRegistrationInfo> ri;
    ComPtr<IPrincipal> pr;
    ComPtr<ITaskSettings> st;
    ComPtr<IIdleSettings> idle;
    ComPtr<ITriggerCollection> tc;
    ComPtr<ITrigger> tr;
    ComPtr<ILogonTrigger> lt;
    ComPtr<IActionCollection> ac;
    ComPtr<IAction> act;
    ComPtr<IExecAction> ex;
    if (FAILED(svc->NewTask(0, &def))) return false;
    if (SUCCEEDED(def->get_RegistrationInfo(&ri))) ri->put_Description(Bstr(L"Backdrop video wallpaper - starts the moment you sign in"));
    if (SUCCEEDED(def->get_Principal(&pr))) {
        pr->put_UserId(Bstr(user));
        pr->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);
        pr->put_RunLevel(TASK_RUNLEVEL_LUA);
    }
    if (SUCCEEDED(def->get_Settings(&st))) {
        st->put_StartWhenAvailable(VARIANT_FALSE);
        st->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
        st->put_StopIfGoingOnBatteries(VARIANT_FALSE);
        st->put_ExecutionTimeLimit(Bstr(L"PT0S"));
        st->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW);
        st->put_Priority(2);   // above normal: the highest a standard user may set (1 needs admin)
        if (SUCCEEDED(st->get_IdleSettings(&idle))) idle->put_StopOnIdleEnd(VARIANT_FALSE);
    }
    if (FAILED(def->get_Triggers(&tc)) || FAILED(tc->Create(TASK_TRIGGER_LOGON, &tr)) || FAILED(tr.As(&lt))) return false;
    lt->put_UserId(Bstr(user));
    if (FAILED(def->get_Actions(&ac)) || FAILED(ac->Create(TASK_ACTION_EXEC, &act)) || FAILED(act.As(&ex))) return false;
    ex->put_Path(Bstr(exe));
    VARIANT vu;
    VariantInit(&vu);
    vu.vt = VT_BSTR;
    vu.bstrVal = SysAllocString(user);
    VARIANT sddl;
    VariantInit(&sddl);
    sddl.vt = VT_BSTR;
    sddl.bstrVal = SysAllocString(L"");
    ComPtr<IRegisteredTask> rt;
    HRESULT hr = root->RegisterTaskDefinition(Bstr(name), def.Get(), TASK_CREATE_OR_UPDATE, vu, none, TASK_LOGON_INTERACTIVE_TOKEN, sddl, &rt);
    VariantClear(&vu);
    VariantClear(&sddl);
    return SUCCEEDED(hr);
}

static void DeleteTreeFiles(const wchar_t* dir) {
    wchar_t pat[MAX_PATH], f[MAX_PATH];
    swprintf_s(pat, L"%s\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            swprintf_s(f, L"%s\\%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) DeleteTreeFiles(f); else DeleteFileW(f);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

// ---- the optional UI process (backdrop-ui.exe: the Backdrop app window + desktop widgets). Never loaded in here.
static bool StartUI(const wchar_t* args, DWORD priority) {
    wchar_t exe[MAX_PATH];
    ExePath(exe);
    if (wchar_t* s = wcsrchr(exe, L'\\')) wcscpy_s(s + 1, MAX_PATH - (s + 1 - exe), L"backdrop-ui.exe");
    if (!Exists(exe)) return false;
    wchar_t cmd[MAX_PATH + 32];
    swprintf_s(cmd, L"\"%s\" %s", exe, args);
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe, cmd, nullptr, nullptr, FALSE, priority, nullptr, nullptr, &si, &pi)) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

static void CloseUI() {   // install / uninstall: the UI process holds sciter.dll open
    HWND o = FindWindowW(L"BackdropUI", nullptr);
    if (!o) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(o, &pid);
    HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, pid);
    PostMessageW(o, WM_CLOSE, 0, 0);
    if (p) { WaitForSingleObject(p, 5000); CloseHandle(p); }
}

static void CloseRunningCopy() {
    CloseUI();
    HWND o = FindWindowW(L"BackdropMain", nullptr);
    if (!o) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(o, &pid);
    HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, pid);
    PostMessageW(o, WM_SETSTATE, 3, 0);
    if (p) { WaitForSingleObject(p, 5000); CloseHandle(p); }
}

static void UninstallData() {
    CloseRunningCopy();
    SetLogonTask(false);
    RegDeleteTreeW(HKEY_CURRENT_USER, KEY);
    wchar_t dir[MAX_PATH];
    GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH);
    wcscat_s(dir, L"\\Backdrop");
    DeleteTreeFiles(dir);
    // Windows' own tray-icon entry (NotifyIconSettings) is left alone: Explorer caches it
}

// Windows 11 hides new tray icons under the ^ arrow; pin ours next to the clock once (the user may un-pin it later)
static bool TryPin() {
    wchar_t exe[MAX_PATH];
    ExePath(exe);
    HKEY root;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\NotifyIconSettings", 0, KEY_READ, &root) != ERROR_SUCCESS) return false;
    bool done = false;
    wchar_t sub[64];
    for (DWORD i = 0; !done; i++) {
        DWORD n = 64;
        if (RegEnumKeyExW(root, i, sub, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        wchar_t path[MAX_PATH];
        DWORD sz = sizeof(path);
        if (RegGetValueW(root, sub, L"ExecutablePath", RRF_RT_REG_SZ, nullptr, path, &sz) != ERROR_SUCCESS) continue;
        const wchar_t* tail = path;   // Explorer may store "{KnownFolderGUID}\rest"
        if (path[0] == L'{') if (const wchar_t* c = wcschr(path, L'}')) tail = c + 1;
        size_t lt = wcslen(tail), le = wcslen(exe);
        if (lt <= le && !_wcsicmp(exe + le - lt, tail)) {
            HKEY k;
            DWORD one = 1;
            if (RegOpenKeyExW(root, sub, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
                RegSetValueExW(k, L"IsPromoted", 0, REG_DWORD, (const BYTE*)&one, sizeof(one));
                RegCloseKey(k);
            }
            done = true;
        }
    }
    RegCloseKey(root);
    return done;
}

static void UninstallFromMenu() {
    if (MessageBoxW(g_main, L"Remove Backdrop from this PC?\n\nThis stops the video wallpaper, removes it from startup and deletes the app. Your normal wallpaper stays.",
                    L"Uninstall Backdrop", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
        return;
    wchar_t unins[MAX_PATH];
    ExePath(unins);
    if (wchar_t* s = wcsrchr(unins, L'\\')) wcscpy_s(s + 1, MAX_PATH - (s + 1 - unins), L"unins000.exe");
    CancelJob();
    CloseUI();
    if (Exists(unins)) {   // the installer's uninstaller (it calls /uninstall, then deletes the files)
        CloseHandle(g_mutex);
        g_mutex = nullptr;
        ShellExecuteW(nullptr, L"open", unins, L"/SILENT", nullptr, SW_SHOWNORMAL);
    } else {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        TurnOff();
        SetLogonTask(false);
        RegDeleteTreeW(HKEY_CURRENT_USER, KEY);
        wchar_t dir[MAX_PATH];
        GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH);
        wcscat_s(dir, L"\\Backdrop");
        DeleteTreeFiles(dir);
    }
    DestroyWindow(g_main);
}

// ------------------------------------------------------------------ tray
// Lucide "image" glyph: white on a dark taskbar, near-black on a light one
static HICON TrayIcon() {
    return (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(ThemeTaskbarDark() ? 2 : 3), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
}

static void AddTray() {
    g_nid = {sizeof(g_nid)};
    g_nid.hWnd = g_main;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = TrayIcon();
    wcscpy_s(g_nid.szTip, L"Backdrop");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    UpdateTip();
}

static void ShowMenu() {
    HMENU m = CreatePopupMenu(), fm = CreatePopupMenu();
    static const wchar_t* fits[] = {L"Fill (crop to the screen)", L"Fit (whole picture, black bars)", L"Stretch", L"Center (original size)"};
    for (int i = 0; i < 4; i++) AppendMenuW(fm, MF_STRING, ID_FIT0 + i, fits[i]);
    int shown = g_job && !g_job->silent && g_src[0] ? int(g_job->fit) : g_fit.load();
    CheckMenuRadioItem(fm, ID_FIT0, ID_FIT0 + 3, ID_FIT0 + shown, MF_BYCOMMAND);
    if (g_job) {
        wchar_t t[64];
        swprintf_s(t, L"Preparing wallpaper… %d%%", g_job->pct);
        AppendMenuW(m, MF_STRING | MF_GRAYED, 0, t);
        AppendMenuW(m, MF_STRING, ID_CANCEL, L"Cancel preparing");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(m, MF_STRING, ID_APP, L"Open Backdrop…");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    wchar_t ft[120];
    swprintf_s(ft, L"Colour filter: %s %d%%%s%s", FilterName(g_fs.type), g_fs.intensity, g_hotkeyOk ? L"\t" : L"", g_hotkeyOk ? g_hotkeyName : L"");
    AppendMenuW(m, MF_STRING | (g_filterOn ? MF_CHECKED : 0), ID_FILTER, ft);
    AppendMenuW(m, MF_STRING, ID_FILTERS, L"Colour filters…");
    swprintf_s(ft, L"Blur: %d%%%s%s", g_blurStrength, g_blurKeyOk ? L"\t" : L"", g_blurKeyOk ? g_blurKeyName : L"");
    AppendMenuW(m, MF_STRING | (g_blurOn ? MF_CHECKED : 0), ID_BLUR, ft);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    wchar_t it[96];
    swprintf_s(it, L"Show desktop icons%s%s", g_iconsKeyName ? L"\t" : L"", g_iconsKeyName ? g_iconsKeyName : L"");
    AppendMenuW(m, MF_STRING | (DesktopIconsShown() ? MF_CHECKED : 0), ID_ICONS, it);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_enabled ? MF_CHECKED : 0) | (PlayPath() ? 0 : MF_GRAYED), ID_TOGGLE, L"Video wallpaper on");
    AppendMenuW(m, MF_STRING, ID_CHOOSE, L"Choose video or GIF…");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)fm, L"Fit");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_UNINSTALL, L"Uninstall Backdrop…");
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Exit");
    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(g_main);
    UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, p.x, p.y, 0, g_main, nullptr);
    DestroyMenu(m);   // also destroys the submenu
    if (cmd == ID_TOGGLE) { g_enabled = !g_enabled; SaveSettings(); ApplyState(); }
    else if (cmd == ID_FILTER) FilterSetOn(!g_filterOn, 300);
    else if (cmd == ID_FILTERS) PanelShow(g_inst);
    else if (cmd == ID_BLUR) { BlurSetOn(!g_blurOn); g_filterChanged(); }
    else if (cmd == ID_ICONS) ToggleDesktopIcons();
    else if (cmd == ID_CHOOSE) Choose();
    else if (cmd >= ID_FIT0 && cmd <= ID_FIT0 + 3) SetFit(Fit(cmd - ID_FIT0));
    else if (cmd == ID_CANCEL) { CancelJob(); UpdateTip(); }
    else if (cmd == ID_UNINSTALL) UninstallFromMenu();
    else if (cmd == ID_APP) StartUI(L"/app", NORMAL_PRIORITY_CLASS);
    else if (cmd == ID_EXIT) DestroyWindow(g_main);
}

// ------------------------------------------------------------------ windows
static LRESULT CALLBACK HostProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_PAINT) { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); return 0; }
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    if (m == WM_DESTROY && h == g_host) {   // explorer took the desktop down with us: re-attach asap
        EnterCriticalSection(&g_cs);
        g_swap.Reset();
        LeaveCriticalSection(&g_cs);
        g_host = g_video = g_parent = nullptr;
        if (g_enabled) SetTimer(g_main, T_WATCH, 10, nullptr);
    }
    return DefWindowProcW(h, m, w, l);
}

static LRESULT CALLBACK ProgressProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if ((m == WM_COMMAND && LOWORD(w) == ID_CANCEL) || m == WM_CLOSE) { CancelJob(); UpdateTip(); return 0; }
    if (m == WM_CTLCOLORSTATIC || m == WM_CTLCOLORBTN) {
        SetBkMode((HDC)w, TRANSPARENT);
        SetTextColor((HDC)w, ThemeIsDark() ? RGB(0xFF, 0xFF, 0xFF) : RGB(0x1B, 0x1B, 0x1B));
        return (LRESULT)(g_progBrush ? g_progBrush : GetSysColorBrush(COLOR_WINDOW));
    }
    if (m == WM_ERASEBKGND && g_progBrush) {
        RECT r;
        GetClientRect(h, &r);
        FillRect((HDC)w, &r, g_progBrush);
        return 1;
    }
    if (m == WM_SETTINGCHANGE && ThemeChanged(l)) { ThemeProgress(); return 0; }
    return DefWindowProcW(h, m, w, l);
}

// ---- commands from the Backdrop app (backdrop-ui.exe), "key=value" over WM_COPYDATA. "state?" is answered with a
// WM_COPYDATA (IPC_STATE) JSON reply to the sender's window, sent before this returns.
static std::wstring JsonEsc(const wchar_t* s) {
    std::wstring o;
    for (; *s; s++) {
        if (*s == L'"' || *s == L'\\') { o += L'\\'; o += *s; }
        else if (*s < 32) o += L' ';
        else o += *s;
    }
    return o;
}

static std::wstring StateJson() {
    wchar_t b[256];
    std::wstring j = L"{\"running\":true";
    swprintf_s(b, L",\"wallpaper\":{\"on\":%s,\"fit\":%d,\"preparing\":%d,\"file\":\"", g_enabled ? L"true" : L"false", g_fit.load(), g_job ? g_job->pct : -1);
    j += b;
    j += JsonEsc(g_src);
    swprintf_s(b, L"\"},\"filter\":{\"on\":%s,\"type\":%d,\"intensity\":%d,\"atLogin\":%s,\"schedule\":%s,\"from\":%d,\"to\":%d,\"hotkey\":\"%s\",\"names\":[",
               g_filterOn ? L"true" : L"false", g_fs.type, g_fs.intensity, g_fs.atLogin ? L"true" : L"false", g_fs.schedule ? L"true" : L"false",
               g_fs.from, g_fs.to, g_hotkeyOk ? g_hotkeyName : L"");
    j += b;
    for (int i = 0; i < FILTER_COUNT; i++) { j += i ? L",\"" : L"\""; j += JsonEsc(FilterName(i)); j += L"\""; }
    swprintf_s(b, L"]},\"blur\":{\"on\":%s,\"strength\":%d,\"hotkey\":\"%s\"}", g_blurOn ? L"true" : L"false", g_blurStrength, g_blurKeyOk ? g_blurKeyName : L"");
    j += b;
    swprintf_s(b, L",\"icons\":{\"shown\":%s,\"hotkey\":\"%s\"}}", DesktopIconsShown() ? L"true" : L"false", g_iconsKeyName ? g_iconsKeyName : L"");
    j += b;
    return j;
}

static bool Command(HWND from, const std::wstring& c) {
    size_t eq = c.find(L'=');
    std::wstring k = c.substr(0, eq), v = eq == std::wstring::npos ? L"" : c.substr(eq + 1);
    int n = _wtoi(v.c_str());
    bool on = v == L"on" || v == L"1" || v == L"true";
    if (k == L"state?") {
        std::wstring j = StateJson();
        COPYDATASTRUCT cds{IPC_STATE, DWORD((j.size() + 1) * sizeof(wchar_t)), (void*)j.c_str()};
        DWORD_PTR r;
        return IsWindow(from) && SendMessageTimeoutW(from, WM_COPYDATA, (WPARAM)g_main, (LPARAM)&cds, SMTO_ABORTIFHUNG, 2000, &r);
    }
    if (k == L"wallpaper") { g_enabled = v == L"toggle" ? !g_enabled : on; SaveSettings(); ApplyState(); }
    else if (k == L"fit" && n >= 0 && n <= FIT_CENTER) SetFit(Fit(n));
    else if (k == L"choose") PostMessageW(g_main, WM_CHOOSE, 1, 0);   // modal dialog: don't block the sender
    else if (k == L"filter") FilterSetOn(v == L"toggle" ? !g_filterOn : on, 300);
    else if (k == L"filter.type" && n >= 0 && n < FILTER_COUNT) { g_fs.type = n; FilterSave(); FilterSetOn(true, 300); }
    else if (k == L"filter.intensity") { g_fs.intensity = std::clamp(n, 0, 100); FilterSave(); if (g_filterOn) FilterUpdate(0); else FilterSetOn(true, 0); }
    else if (k == L"filter.atlogin") { g_fs.atLogin = on; FilterSave(); }
    else if (k == L"filter.schedule") { g_fs.schedule = on; FilterSave(); ScheduleFollow(400); }
    else if ((k == L"filter.from" || k == L"filter.to") && n >= 0 && n < 1440) {
        (k == L"filter.from" ? g_fs.from : g_fs.to) = n;
        FilterSave();
        if (g_fs.schedule) ScheduleFollow(400);
    }
    else if (k == L"filter.panel") PanelShow(g_inst);
    else if (k == L"blur") BlurSetOn(v == L"toggle" ? !g_blurOn : on);
    else if (k == L"blur.strength") BlurSetStrength(n);
    else if (k == L"icons") { if (v == L"toggle" || on != DesktopIconsShown()) ToggleDesktopIcons(); }
    else return false;
    if (g_filterChanged) g_filterChanged();
    return true;
}

static LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_TRAY:
        if (LOWORD(l) == WM_LBUTTONUP || LOWORD(l) == WM_RBUTTONUP) ShowMenu();
        return 0;
    case WM_SHOWMENU: ShowMenu(); return 0;
    case WM_CHOOSE:
        Choose();
        if (w == 1) {   // asked by the Backdrop app: give it the focus back (our own main window would keep it otherwise)
            DWORD uiPid = 0;
            if (HWND ui = FindWindowW(L"BackdropUI", nullptr)) GetWindowThreadProcessId(ui, &uiPid);
            struct Find { DWORD pid; HWND found; } f{uiPid, nullptr};
            if (uiPid) EnumWindows([](HWND h, LPARAM p) -> BOOL {
                auto* f = (Find*)p;
                DWORD pid = 0;
                GetWindowThreadProcessId(h, &pid);
                wchar_t t[16];
                if (pid == f->pid && IsWindowVisible(h) && GetWindowTextW(h, t, 16) && !wcscmp(t, L"Backdrop")) { f->found = h; return FALSE; }
                return TRUE;
            }, (LPARAM)&f);
            if (f.found) SetForegroundWindow(f.found);
        }
        return 0;
    case WM_COPYDATA: {
        auto* cds = (const COPYDATASTRUCT*)l;
        if (!cds || cds->dwData != IPC_COMMAND || !cds->lpData || cds->cbData < sizeof(wchar_t) || cds->cbData > 4096) return FALSE;
        std::wstring c((const wchar_t*)cds->lpData, cds->cbData / sizeof(wchar_t));
        while (!c.empty() && !c.back()) c.pop_back();
        return Command((HWND)w, c);
    }
    case WM_SETSTATE:
        if (w == 3) { DestroyWindow(h); return 0; }
        if (w == 4) { FilterSetOn(!g_filterOn, 300); return 0; }
        if (w == 5) { PanelShow(g_inst); return 0; }
        if (w == 6) { ToggleDesktopIcons(); return 0; }
        if (w == 7) { BlurSetOn(!g_blurOn); g_filterChanged(); return 0; }
        g_enabled = w == 2 ? !g_enabled : w == 1;
        SaveSettings();
        ApplyState();
        return 0;
    case WM_TIMER:
        if (w == T_WIDGETS) {   // widgets at sign-in: only once the wallpaper is up (or after 15 s), at low priority
            static int tries = 0;
            if (!g_enabled || !PlayPath() || Attached() || ++tries > 300) {
                KillTimer(h, T_WIDGETS);
                StartUI(L"", BELOW_NORMAL_PRIORITY_CLASS);
            }
            return 0;
        }
        if (w == T_PIN) {
            if (TryPin() || ++g_pinTries > 40) { KillTimer(h, T_PIN); g_pinned = 1; SaveSettings(); }
            return 0;
        }
        Watch();
        return 0;
    case WM_CONVPROG:
        if (g_job && g_job->pct != int(w)) {
            g_job->pct = int(w);
            if (g_progBar) SendMessageW(g_progBar, PBM_SETPOS, w, 0);
            UpdateTip();
        }
        return 0;
    case WM_CONVDONE: JobDone((Job*)l); return 0;
    case WM_HOTKEY:
        if (w == HK_FILTER) FilterSetOn(!g_filterOn, 300);
        if (w == HK_ICONS) ToggleDesktopIcons();
        if (w == HK_BLUR) { BlurSetOn(!g_blurOn); g_filterChanged(); }
        return 0;
    case WM_BLURFAIL:
        g_blurOn = false;
        g_filterChanged();
        MessageBoxW(nullptr, w == 2 ? L"Blur needs Windows 10 version 2004 or newer." : L"Blur couldn't start on this display, so it's off again.",
                    L"Backdrop", MB_ICONINFORMATION);
        return 0;
    case WM_SETTINGCHANGE:
        if (ThemeChanged(l)) {   // menus and the tray glyph follow light / dark
            ThemeRefresh();
            HICON old = g_nid.hIcon;
            g_nid.hIcon = TrayIcon();
            g_nid.uFlags = NIF_ICON;
            Shell_NotifyIconW(NIM_MODIFY, &g_nid);
            if (old) DestroyIcon(old);
        }
        return 0;
    case WM_TIMECHANGE:
        ScheduleCheck(1000);
        return 0;
    case WM_DISPLAYCHANGE: case WM_DPICHANGED:
        FilterUpdate(0);   // re-assert the colour filter after mode changes
        if (g_enabled && g_engine) { Detach(); Watch(); }
        EnsureBaked();   // new resolution: bake again in the background
        return 0;
    case WM_WTSSESSION_CHANGE:
        if (w == WTS_SESSION_LOCK) g_locked = true;
        if (w == WTS_SESSION_UNLOCK) { g_locked = false; ScheduleCheck(1000); FilterUpdate(0); }
        Watch();
        return 0;
    case WM_POWERBROADCAST:
        if (w == PBT_APMRESUMEAUTOMATIC) { ScheduleCheck(0); FilterUpdate(0); }   // slept through an on/off time?
        if (w == PBT_POWERSETTINGCHANGE) {
            auto* s = (POWERBROADCAST_SETTING*)l;
            if (s->PowerSetting == GUID_CONSOLE_DISPLAY_STATE) {
                g_displayOff = *(const DWORD*)s->Data == 0;
                if (!g_displayOff) ScheduleCheck(0);   // Modern Standby wakes without PBT_APMRESUMEAUTOMATIC
                Watch();
            }
        }
        return TRUE;
    case WM_MFERROR:
        g_enabled = false;   // off for this session only: a missing/offline file shouldn't disable it for good
        ApplyState();
        MessageBoxW(nullptr, L"Windows can't play this wallpaper file anymore. Choose it again or pick another one.", L"Backdrop", MB_ICONWARNING);
        return 0;
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        BlurShutdown();
        FilterShutdown();
        TurnOff();
        PostQuitMessage(0);
        return 0;
    }
    if (m == g_taskbarCreated && m) {   // explorer restarted: new tray (the host re-attaches via its WM_DESTROY)
        AddTray();
        if (!Attached()) Watch();
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmd, int) {
    wchar_t sys[MAX_PATH];   // don't hold the install folder (or any user folder) open as the working directory
    if (GetSystemDirectoryW(sys, MAX_PATH)) SetCurrentDirectoryW(sys);
    if (wcsstr(cmd, L"/install") || wcsstr(cmd, L"/uninstall")) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        bool ok = true;
        if (wcsstr(cmd, L"/uninstall")) UninstallData();
        else ok = SetLogonTask(true);
        CoUninitialize();
        return ok ? 0 : 1;
    }
    int arg = -1;
    if (wcsstr(cmd, L"/off")) arg = 0;
    else if (wcsstr(cmd, L"/on")) arg = 1;
    else if (wcsstr(cmd, L"/toggle")) arg = 2;
    else if (wcsstr(cmd, L"/exit")) arg = 3;
    else if (wcsstr(cmd, L"/icons")) arg = 6;
    else if (wcsstr(cmd, L"/blur")) arg = 7;
    else if (wcsstr(cmd, L"/filters")) arg = 5;
    else if (wcsstr(cmd, L"/filter")) arg = 4;
    g_mutex = CreateMutexW(nullptr, TRUE, L"Backdrop.single");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {   // already running: forward the switch, or pop its menu
        if (HWND o = FindWindowW(L"BackdropMain", nullptr)) {
            AllowSetForegroundWindow(ASFW_ANY);
            PostMessageW(o, arg < 0 ? WM_SHOWMENU : WM_SETSTATE, arg < 0 ? 0 : WPARAM(arg), 0);
        }
        return 0;
    }
    if (arg == 3) return 0;
    FilterStartup();   // first thing: a sign-in / scheduled filter goes on in a few ms, before anything else loads
#ifdef VW_LOG
    {
        FILETIME c, e, k, u, now;
        GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
        GetSystemTimePreciseAsFileTime(&now);
        LOG(L"filter applied %.1f ms after process creation (on=%d)",
            ((ULARGE_INTEGER{now.dwLowDateTime, now.dwHighDateTime}.QuadPart) - (ULARGE_INTEGER{c.dwLowDateTime, c.dwHighDateTime}.QuadPart)) / 10000.0, int(g_filterOn));
    }
#endif
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_inst = inst;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    InitializeCriticalSection(&g_cs);
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    WNDCLASSW wc{};
    wc.hInstance = inst;
    wc.lpfnWndProc = HostProc;
    wc.lpszClassName = L"BackdropHost";
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassW(&wc);
    wc.lpfnWndProc = MainProc;
    wc.lpszClassName = L"BackdropMain";
    wc.hbrBackground = nullptr;
    RegisterClassW(&wc);
    wc.lpfnWndProc = ProgressProc;
    wc.lpszClassName = L"BackdropProgress";
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    RegisterClassW(&wc);
    g_main = CreateWindowExW(0, L"BackdropMain", L"Backdrop", 0, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    WTSRegisterSessionNotification(g_main, NOTIFY_FOR_THIS_SESSION);
    g_filterChanged = [] { UpdateTip(); PanelUpdate(); };
    static const struct { UINT mod, vk; const wchar_t* name; } keys[] = {   // first free one wins (another app may own a combo)
        {MOD_WIN | MOD_ALT, 'C', L"Win+Alt+C"}, {MOD_CONTROL | MOD_WIN | MOD_ALT, 'F', L"Ctrl+Win+Alt+F"}, {MOD_CONTROL | MOD_SHIFT | MOD_WIN, 'F', L"Ctrl+Shift+Win+F"}};
    for (auto& k : keys)
        if (RegisterHotKey(g_main, HK_FILTER, k.mod | MOD_NOREPEAT, k.vk)) { g_hotkeyOk = true; g_hotkeyName = k.name; break; }
    static const struct { UINT mod, vk; const wchar_t* name; } iconKeys[] = {
        {MOD_WIN | MOD_ALT, 'H', L"Win+Alt+H"}, {MOD_WIN | MOD_ALT, 'I', L"Win+Alt+I"}, {MOD_CONTROL | MOD_WIN | MOD_ALT, 'H', L"Ctrl+Win+Alt+H"}};
    for (auto& k : iconKeys)
        if (RegisterHotKey(g_main, HK_ICONS, k.mod | MOD_NOREPEAT, k.vk)) { g_iconsKeyName = k.name; break; }
    static const struct { UINT mod, vk; const wchar_t* name; } blurKeys[] = {   // not Win+Alt+B: that's Windows' HDR switch
        {MOD_WIN | MOD_ALT, 'Z', L"Win+Alt+Z"}, {MOD_CONTROL | MOD_WIN | MOD_ALT, 'B', L"Ctrl+Win+Alt+B"}, {MOD_CONTROL | MOD_SHIFT | MOD_WIN, 'B', L"Ctrl+Shift+Win+B"}};
    for (auto& k : blurKeys)
        if (RegisterHotKey(g_main, HK_BLUR, k.mod | MOD_NOREPEAT, k.vk)) { g_blurKeyOk = true; g_blurKeyName = k.name; break; }
    BlurInit(g_main, WM_BLURFAIL);   // strength only: the blur itself always starts off
    ThemeInit();
    RegisterPowerSettingNotification(g_main, &GUID_CONSOLE_DISPLAY_STATE, DEVICE_NOTIFY_WINDOW_HANDLE);

    LoadSettings();
    if (arg == 0 || arg == 1) { g_enabled = arg == 1; SaveSettings(); }
    CloseHandle(CreateThread(nullptr, 0, RenderThread, nullptr, 0, nullptr));
    AddTray();
    ApplyState();       // play what we have right away (login speed comes first), re-bake in the background if needed
    if (!g_pinned) SetTimer(g_main, T_PIN, 500, nullptr);
    if (arg == 4) FilterSetOn(!g_filterOn, 0);
    if (arg == 5) PanelShow(inst);
    if (arg == 6) ToggleDesktopIcons();
    if (arg == 7) BlurSetOn(true);
    {   // desktop widgets (a separate process) only if the user turned on "Open widgets at sign-in"; checked after the
        // wallpaper and filter are already up, and started later still (see T_WIDGETS)
        DWORD on = 0, atLogin = 0, sz = sizeof(DWORD);
        RegGetValueW(HKEY_CURRENT_USER, KEY, L"WidgetsEnabled", RRF_RT_REG_DWORD, nullptr, &on, &sz);
        sz = sizeof(DWORD);
        RegGetValueW(HKEY_CURRENT_USER, KEY, L"WidgetsAtLogin", RRF_RT_REG_DWORD, nullptr, &atLogin, &sz);
        if (on && atLogin && !FindWindowW(L"BackdropUI", nullptr)) SetTimer(g_main, T_WIDGETS, 50, nullptr);
    }
    if (!g_src[0] && !g_asked) { g_asked = 1; SaveSettings(); Choose(); }   // first run only, not at every login

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (g_prog && IsDialogMessageW(g_prog, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_mutex) ReleaseMutex(g_mutex);
    ExitProcess(0);   // don't wait for the render / converter threads
}
