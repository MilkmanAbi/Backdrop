// Backdrop "Colour filter" window, drawn with Direct2D / DirectWrite in the Windows 11 style
// (Mica where available, follows light/dark and the accent colour). Everything applies live.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "panel.h"
#include "filter.h"
#include "theme.h"
#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <vector>

#pragma comment(lib, "d2d1")
#pragma comment(lib, "dwrite")
#pragma comment(lib, "dwmapi")
#pragma comment(lib, "shcore")

using Microsoft::WRL::ComPtr;

bool g_hotkeyOk = false;
const wchar_t* g_hotkeyName = L"";

static HWND g_panel;
static UINT g_dpi = 96;
static bool g_mica = false;
static ComPtr<ID2D1Factory> g_d2d;
static ComPtr<IDWriteFactory> g_dw;
static ComPtr<ID2D1HwndRenderTarget> g_rt;
static ComPtr<ID2D1SolidColorBrush> g_br;
static ComPtr<ID2D1BitmapBrush> g_swatch[FILTER_COUNT];
static ComPtr<IDWriteTextFormat> g_fTitle, g_fBody, g_fBodyR, g_fSmall, g_fCard, g_fTime, g_fArrow;

// ------------------------------------------------------------------ layout (DIPs)
static const float W = 440, H = 778, PAD = 24;
enum { E_NONE = -1, E_MAIN = 100, E_SLIDER, E_LOGIN, E_SCHED, E_FROM_DEC, E_FROM_INC, E_TO_DEC, E_TO_INC, E_FROM, E_TO };
enum { TM_REPEAT = 1, TM_APPLY };
static int g_hover = E_NONE, g_press = E_NONE;

static D2D1_RECT_F R(float l, float t, float w, float h) { return D2D1::RectF(l, t, l + w, t + h); }
static bool In(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

static D2D1_RECT_F CardRect(int i) {
    float cw = (W - 2 * PAD - 12) / 2;
    return R(PAD + (i % 2) * (cw + 12), 88 + (i / 2) * 66, cw, 56);
}
static const D2D1_RECT_F kMain = R(W - PAD - 44, 24, 44, 22), kSlider = R(PAD, 516, W - 2 * PAD, 32);
static const D2D1_RECT_F kLoginRow = R(PAD, 574, W - 2 * PAD, 32), kSchedRow = R(PAD, 612, W - 2 * PAD, 32);
static const D2D1_RECT_F kFrom = R(PAD + 42, 654, 146, 36), kTo = R(PAD + 232, 654, 146, 36);
static const float kTrack0 = PAD + 10, kTrack1 = W - PAD - 10, kTrackY = 532;

static int Hit(float x, float y) {
    if (In(R(kMain.left - 6, kMain.top - 6, 56, 34), x, y)) return E_MAIN;
    for (int i = 0; i < FILTER_COUNT; i++)
        if (In(CardRect(i), x, y)) return i;
    if (In(kSlider, x, y)) return E_SLIDER;
    if (In(kLoginRow, x, y)) return E_LOGIN;
    if (In(kSchedRow, x, y)) return E_SCHED;
    if (g_fs.schedule) {
        if (In(kFrom, x, y)) return x < kFrom.left + 34 ? E_FROM_DEC : x >= kFrom.right - 34 ? E_FROM_INC : E_FROM;
        if (In(kTo, x, y)) return x < kTo.left + 34 ? E_TO_DEC : x >= kTo.right - 34 ? E_TO_INC : E_TO;
    }
    return E_NONE;
}

// ------------------------------------------------------------------ palette
struct Pal {
    D2D1_COLOR_F bg, card, cardHover, cardBorder, text, text2, divider, ctrl, track, accent, accentHover, onAccent, thumb, thumbBorder;
};
static D2D1_COLOR_F Rgb(COLORREF c, float a = 1) { return D2D1::ColorF(GetRValue(c) / 255.f, GetGValue(c) / 255.f, GetBValue(c) / 255.f, a); }
static D2D1_COLOR_F Wh(float a) { return D2D1::ColorF(1, 1, 1, a); }
static D2D1_COLOR_F Bk(float a) { return D2D1::ColorF(0, 0, 0, a); }
static D2D1_COLOR_F Fade(D2D1_COLOR_F c, float k) { c.a *= k; return c; }
static D2D1_COLOR_F Mix(D2D1_COLOR_F a, D2D1_COLOR_F b, float k) {
    return D2D1::ColorF(a.r + (b.r - a.r) * k, a.g + (b.g - a.g) * k, a.b + (b.b - a.b) * k, a.a);
}

static Pal Palette() {
    bool d = ThemeIsDark();
    D2D1_COLOR_F acc = Rgb(ThemeAccent(d));
    if (d) return {Rgb(0x202020), Wh(.055f), Wh(.085f), Wh(.07f), Wh(1), Wh(.74f), Wh(.09f), Wh(.62f), Wh(.28f), acc, Mix(acc, Wh(1), .12f), Bk(1), Rgb(0x454545), Wh(.09f)};
    return {Rgb(0xF3F3F3), Wh(.72f), Wh(.95f), Bk(.07f), Rgb(0x1B1B1B), Bk(.62f), Bk(.08f), Bk(.58f), Bk(.28f), acc, Mix(acc, Bk(1), .12f), Wh(1), Wh(1), Bk(.10f)};
}

// ------------------------------------------------------------------ resources
static const wchar_t* Family(const wchar_t* want) {
    ComPtr<IDWriteFontCollection> c;
    UINT32 i;
    BOOL e = FALSE;
    if (SUCCEEDED(g_dw->GetSystemFontCollection(&c))) c->FindFamilyName(want, &i, &e);
    return e ? want : L"Segoe UI";
}

static ComPtr<IDWriteTextFormat> Font(const wchar_t* fam, float size, DWRITE_FONT_WEIGHT wgt, DWRITE_TEXT_ALIGNMENT al, bool center, bool wrap) {
    ComPtr<IDWriteTextFormat> f;
    g_dw->CreateTextFormat(Family(fam), nullptr, wgt, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &f);
    f->SetTextAlignment(al);
    f->SetParagraphAlignment(center ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER : DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    f->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    return f;
}

static bool Factories() {
    if (g_d2d) return true;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, g_d2d.GetAddressOf())) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)g_dw.GetAddressOf())))
        return false;
    const wchar_t *disp = L"Segoe UI Variable Display", *txt = L"Segoe UI Variable Text";
    g_fTitle = Font(disp, 24, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, false, false);
    g_fBody = Font(txt, 14, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true, false);
    g_fBodyR = Font(txt, 14, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_TRAILING, true, false);
    g_fSmall = Font(txt, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, false, true);
    g_fCard = Font(txt, 13.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true, true);
    g_fTime = Font(txt, 14, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, true, false);
    g_fArrow = Font(txt, 18, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, true, false);
    return true;
}

static void Discard() {
    for (auto& s : g_swatch) s.Reset();
    g_br.Reset();
    g_rt.Reset();
}

// each card's preview: a hue x brightness patch run through that filter at full strength
static void MakeSwatches() {
    UINT n = UINT(36.0f * g_dpi / 96 + 0.5f);
    std::vector<UINT32> px(size_t(n) * n);
    for (int t = 0; t < FILTER_COUNT; t++) {
        float m[3][3], o[3];
        FilterMatrix(t, 1, m, o);
        for (UINT y = 0; y < n; y++)
            for (UINT x = 0; x < n; x++) {
                float h = 6.0f * x / (n - 1), v = 1.0f - 0.6f * y / (n - 1), s = 0.85f;
                float c = v * s, xx = c * (1 - std::fabs(std::fmod(h, 2.0f) - 1)), mm = v - c, in[3];
                int seg = int(h) % 6;
                float rgb[6][3] = {{c, xx, 0}, {xx, c, 0}, {0, c, xx}, {0, xx, c}, {xx, 0, c}, {c, 0, xx}};
                for (int k = 0; k < 3; k++) in[k] = rgb[seg][k] + mm;
                BYTE out[3];
                for (int i = 0; i < 3; i++) {
                    float val = m[i][0] * in[0] + m[i][1] * in[1] + m[i][2] * in[2] + o[i];
                    out[i] = BYTE(std::clamp(val, 0.0f, 1.0f) * 255 + 0.5f);
                }
                px[size_t(y) * n + x] = 0xFF000000u | UINT32(out[0]) << 16 | UINT32(out[1]) << 8 | out[2];
            }
        ComPtr<ID2D1Bitmap> bmp;
        D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), float(g_dpi), float(g_dpi));
        if (SUCCEEDED(g_rt->CreateBitmap(D2D1::SizeU(n, n), px.data(), n * 4, bp, &bmp))) g_rt->CreateBitmapBrush(bmp.Get(), &g_swatch[t]);
    }
}

static bool Target() {
    if (g_rt) return true;
    RECT rc;
    GetClientRect(g_panel, &rc);
    D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, g_mica ? D2D1_ALPHA_MODE_PREMULTIPLIED : D2D1_ALPHA_MODE_IGNORE), float(g_dpi), float(g_dpi));
    if (FAILED(g_d2d->CreateHwndRenderTarget(p, D2D1::HwndRenderTargetProperties(g_panel, D2D1::SizeU(rc.right, rc.bottom)), &g_rt))) return false;
    if (g_mica) g_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);   // ClearType needs an opaque background
    g_rt->CreateSolidColorBrush(Wh(1), &g_br);
    MakeSwatches();
    return true;
}

// ------------------------------------------------------------------ drawing
static void Text(const wchar_t* s, const ComPtr<IDWriteTextFormat>& f, D2D1_RECT_F r, D2D1_COLOR_F c) {
    g_br->SetColor(c);
    g_rt->DrawText(s, UINT32(wcslen(s)), f.Get(), r, g_br.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

static void Fill(D2D1_RECT_F r, float rad, D2D1_COLOR_F c) {
    g_br->SetColor(c);
    g_rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), g_br.Get());
}

static void Stroke(D2D1_RECT_F r, float rad, D2D1_COLOR_F c, float w = 1) {
    g_br->SetColor(c);
    float k = w / 2;
    g_rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + k, r.top + k, r.right - k, r.bottom - k), rad, rad), g_br.Get(), w);
}

static void Circle(float x, float y, float r, D2D1_COLOR_F c) {
    g_br->SetColor(c);
    g_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), r, r), g_br.Get());
}

static void Toggle(const Pal& p, D2D1_RECT_F r, bool on, bool hover, float alpha = 1) {
    float h = r.bottom - r.top, cy = (r.top + r.bottom) / 2;
    if (on) {
        Fill(r, h / 2, Fade(hover ? p.accentHover : p.accent, alpha));
        Circle(r.right - h / 2, cy, h / 2 - (hover ? 4.f : 5.f), Fade(p.onAccent, alpha));
    } else {
        if (hover) Fill(r, h / 2, Fade(p.cardHover, alpha));
        Stroke(r, h / 2, Fade(p.ctrl, alpha));
        Circle(r.left + h / 2, cy, h / 2 - (hover ? 5.f : 6.f), Fade(p.ctrl, alpha));
    }
}

static void TimeText(int mins, wchar_t* out, int n) {
    SYSTEMTIME st{};
    st.wYear = 2000; st.wMonth = 1; st.wDay = 1;
    st.wHour = WORD(mins / 60);
    st.wMinute = WORD(mins % 60);
    if (!GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &st, nullptr, out, n)) swprintf_s(out, n, L"%02d:%02d", mins / 60, mins % 60);
}

static void TimeBox(const Pal& p, D2D1_RECT_F r, int mins, int eDec, int eInc) {
    float a = g_fs.schedule ? 1.0f : 0.4f, h = r.bottom - r.top;
    Fill(r, 6, Fade(p.card, a));
    Stroke(r, 6, Fade(p.cardBorder, a));
    if (g_hover == eDec) Fill(R(r.left + 3, r.top + 3, 30, h - 6), 4, p.cardHover);
    if (g_hover == eInc) Fill(R(r.right - 33, r.top + 3, 30, h - 6), 4, p.cardHover);
    Text(L"‹", g_fArrow, R(r.left, r.top - 1, 34, h), Fade(p.text2, a));
    Text(L"›", g_fArrow, R(r.right - 34, r.top - 1, 34, h), Fade(p.text2, a));
    wchar_t t[32];
    TimeText(mins, t, 32);
    Text(t, g_fTime, R(r.left + 34, r.top, r.right - r.left - 68, h), Fade(p.text, a));
}

static void Paint() {
    if (!Factories() || !Target()) return;
    Pal p = Palette();
    wchar_t s[200];
    g_rt->BeginDraw();
    g_rt->Clear(g_mica ? D2D1::ColorF(0, 0, 0, 0) : p.bg);

    Text(L"Colour filter", g_fTitle, R(PAD, 16, 300, 34), p.text);
    Toggle(p, kMain, g_filterOn, g_hover == E_MAIN);
    if (g_filterOn) swprintf_s(s, L"On · %s · %d%%", FilterName(g_fs.type), g_fs.intensity);
    else wcscpy_s(s, L"Off · pick a filter to turn it on");
    Text(s, g_fBody, R(PAD, 50, W - 2 * PAD, 22), p.text2);

    for (int i = 0; i < FILTER_COUNT; i++) {
        D2D1_RECT_F r = CardRect(i);
        bool sel = g_fs.type == i;
        Fill(r, 8, sel ? Fade(p.accent, g_filterOn ? 0.16f : 0.08f) : g_hover == i ? p.cardHover : p.card);
        if (sel) Stroke(r, 8, Fade(p.accent, g_filterOn ? 1.0f : 0.55f), 2);
        else Stroke(r, 8, p.cardBorder);
        D2D1_RECT_F sw = R(r.left + 10, r.top + 10, 36, 36);
        if (g_swatch[i]) {
            g_swatch[i]->SetTransform(D2D1::Matrix3x2F::Translation(sw.left, sw.top));
            g_rt->FillRoundedRectangle(D2D1::RoundedRect(sw, 5, 5), g_swatch[i].Get());
        }
        Text(FilterName(i), g_fCard, R(r.left + 56, r.top + 2, r.right - r.left - 62, 52), p.text);
    }

    Text(L"Intensity", g_fBody, R(PAD, 492, 200, 22), p.text);
    swprintf_s(s, L"%d%%", g_fs.intensity);
    Text(s, g_fBodyR, R(W - PAD - 100, 492, 100, 22), p.text);
    float v = g_fs.intensity / 100.0f, tx = kTrack0 + v * (kTrack1 - kTrack0);
    Fill(D2D1::RectF(kTrack0, kTrackY - 2, kTrack1, kTrackY + 2), 2, p.track);
    Fill(D2D1::RectF(kTrack0, kTrackY - 2, tx, kTrackY + 2), 2, p.accent);
    Circle(tx, kTrackY, 11, p.thumbBorder);
    Circle(tx, kTrackY, 10, p.thumb);
    Circle(tx, kTrackY, g_press == E_SLIDER ? 5.f : g_hover == E_SLIDER ? 7.f : 6.f, p.accent);

    Fill(D2D1::RectF(PAD, 562, W - PAD, 563), 0, p.divider);
    Text(L"Turn on when I sign in", g_fBody, R(PAD, 575, 300, 30), p.text);
    Toggle(p, R(W - PAD - 44, 579, 44, 22), g_fs.atLogin, g_hover == E_LOGIN);
    Text(L"Schedule", g_fBody, R(PAD, 613, 300, 30), p.text);
    Toggle(p, R(W - PAD - 44, 617, 44, 22), g_fs.schedule, g_hover == E_SCHED);
    float a = g_fs.schedule ? 1.0f : 0.4f;
    Text(L"From", g_fBody, R(PAD, 654, 40, 36), Fade(p.text2, a));
    TimeBox(p, kFrom, g_fs.from, E_FROM_DEC, E_FROM_INC);
    Text(L"To", g_fBody, R(PAD + 206, 654, 26, 36), Fade(p.text2, a));
    TimeBox(p, kTo, g_fs.to, E_TO_DEC, E_TO_INC);
    wchar_t f[32], t[32];
    TimeText(g_fs.from, f, 32);
    TimeText(g_fs.to, t, 32);
    if (!g_fs.schedule) wcscpy_s(s, L"Comes on and goes off by itself at the times above, every day.");
    else if (g_fs.from == g_fs.to) wcscpy_s(s, L"Pick different on and off times.");
    else swprintf_s(s, L"On every day from %s to %s. You can still switch it by hand; the next scheduled time takes over again.", f, t);
    Text(s, g_fSmall, R(PAD, 700, W - 2 * PAD, 36), p.text2);
    Fill(D2D1::RectF(PAD, 742, W - PAD, 743), 0, p.divider);
    if (g_hotkeyOk) swprintf_s(s, L"%s turns the filter on or off from anywhere.", g_hotkeyName);
    else wcscpy_s(s, L"Tip: the tray icon's menu turns the filter on or off.");
    Text(s, g_fSmall, R(PAD, 752, W - 2 * PAD, 20), p.text2);

    if (g_rt->EndDraw() == D2DERR_RECREATE_TARGET) Discard();
}

// ------------------------------------------------------------------ interaction
static void Invalidate() { if (g_panel) InvalidateRect(g_panel, nullptr, FALSE); }

static void SliderTo(float x) {
    int v = int(std::lround((x - kTrack0) / (kTrack1 - kTrack0) * 100));
    v = std::clamp(v, 0, 100);
    if (v == g_fs.intensity && g_filterOn) return;
    g_fs.intensity = v;
    FilterSetOn(true, 0);   // live while dragging
}

static void StepTime(int which, int delta, bool snap) {
    int& t = which ? g_fs.to : g_fs.from;
    if (snap && delta > 0) t = (t / 15 + 1) * 15;
    else if (snap && delta < 0) t = ((t + 14) / 15 - 1) * 15;
    else t += delta;
    t = ((t % 1440) + 1440) % 1440;
    FilterSave();
    SetTimer(g_panel, TM_APPLY, 700, nullptr);   // follow the schedule once the user stops adjusting
    Invalidate();
}

static void StepArrow(int e) {
    if (e == E_FROM_DEC) StepTime(0, -15, true);
    else if (e == E_FROM_INC) StepTime(0, 15, true);
    else if (e == E_TO_DEC) StepTime(1, -15, true);
    else if (e == E_TO_INC) StepTime(1, 15, true);
}

static LRESULT CALLBACK PanelProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    float k = 96.0f / g_dpi;
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        Paint();
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE:
        if (g_rt) g_rt->Resize(D2D1::SizeU(LOWORD(l), HIWORD(l)));
        return 0;
    case WM_DPICHANGED: {
        g_dpi = HIWORD(w);
        const RECT* r = (const RECT*)l;
        Discard();
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Invalidate();
        return 0;
    }
    case WM_SETTINGCHANGE:
        if (ThemeChanged(l)) { ThemeTitleBar(h); Invalidate(); }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) { SetCursor(LoadCursorW(nullptr, g_hover != E_NONE ? IDC_HAND : IDC_ARROW)); return TRUE; }
        break;
    case WM_MOUSEMOVE: {
        float x = GET_X_LPARAM(l) * k, y = GET_Y_LPARAM(l) * k;
        if (g_press == E_SLIDER) { SliderTo(x); return 0; }
        int e = Hit(x, y);
        if (e != g_hover) { g_hover = e; Invalidate(); }
        TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, h, 0};
        TrackMouseEvent(&t);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_press == E_NONE && g_hover != E_NONE) { g_hover = E_NONE; Invalidate(); }
        return 0;
    case WM_LBUTTONDOWN: {
        float x = GET_X_LPARAM(l) * k, y = GET_Y_LPARAM(l) * k;
        int e = Hit(x, y);
        g_press = e;
        SetCapture(h);
        if (e >= 0 && e < FILTER_COUNT) { g_fs.type = e; FilterSave(); FilterSetOn(true, 300); }
        else if (e == E_MAIN) FilterSetOn(!g_filterOn, 300);
        else if (e == E_SLIDER) SliderTo(x);
        else if (e == E_LOGIN) { g_fs.atLogin = !g_fs.atLogin; FilterSave(); }
        else if (e == E_SCHED) { g_fs.schedule = !g_fs.schedule; FilterSave(); ScheduleFollow(400); }
        else if (e >= E_FROM_DEC && e <= E_TO_INC) { StepArrow(e); SetTimer(h, TM_REPEAT, 400, nullptr); }
        Invalidate();
        return 0;
    }
    case WM_LBUTTONUP:
        if (g_press == E_SLIDER) FilterSave();
        g_press = E_NONE;
        KillTimer(h, TM_REPEAT);
        ReleaseCapture();
        Invalidate();
        return 0;
    case WM_TIMER:
        if (w == TM_REPEAT) {   // hold an arrow to keep stepping
            if (g_press >= E_FROM_DEC && g_press <= E_TO_INC && g_hover == g_press) StepArrow(g_press);
            SetTimer(h, TM_REPEAT, 70, nullptr);
        } else if (w == TM_APPLY) {
            KillTimer(h, TM_APPLY);
            if (g_fs.schedule) ScheduleFollow(400);
        }
        return 0;
    case WM_MOUSEWHEEL: {
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        ScreenToClient(h, &pt);
        int e = Hit(pt.x * k, pt.y * k), steps = GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA;
        int minutes = (GET_KEYSTATE_WPARAM(w) & MK_SHIFT) ? 1 : 5;
        if (e == E_SLIDER || (e >= 0 && e < FILTER_COUNT)) {
            g_fs.intensity = std::clamp(g_fs.intensity + 5 * steps, 0, 100);
            FilterSetOn(true, 0);
            FilterSave();
        } else if (e == E_FROM || e == E_FROM_DEC || e == E_FROM_INC) StepTime(0, steps * minutes, false);
        else if (e == E_TO || e == E_TO_DEC || e == E_TO_INC) StepTime(1, steps * minutes, false);
        return 0;
    }
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) DestroyWindow(h);
        else if (w == VK_LEFT || w == VK_RIGHT) {
            g_fs.intensity = std::clamp(g_fs.intensity + (w == VK_RIGHT ? 5 : -5), 0, 100);
            FilterSetOn(true, 0);
            FilterSave();
        } else if (w == VK_SPACE) FilterSetOn(!g_filterOn, 300);
        return 0;
    case WM_DESTROY:
        Discard();
        g_panel = nullptr;
        g_hover = g_press = E_NONE;
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void PanelUpdate() { Invalidate(); }

void PanelShow(HINSTANCE inst) {
    if (g_panel) {
        ShowWindow(g_panel, SW_RESTORE);
        SetForegroundWindow(g_panel);
        return;
    }
    if (!Factories()) return;
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.hInstance = inst;
        wc.lpfnWndProc = PanelProc;
        wc.lpszClassName = L"BackdropPanel";
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        registered = true;
    }
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
    g_dpi = dx;
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc{0, 0, LONG(W * g_dpi / 96), LONG(H * g_dpi / 96)};
    AdjustWindowRectExForDpi(&rc, style, FALSE, 0, g_dpi);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    int ww = rc.right - rc.left, wh = rc.bottom - rc.top, margin = MulDiv(12, int(g_dpi), 96);
    int x = mi.rcWork.right - ww - margin, y = std::max<int>(mi.rcWork.top, mi.rcWork.bottom - wh - margin);
    g_panel = CreateWindowExW(0, L"BackdropPanel", L"Colour filter — Backdrop", style, x, y, ww, wh, nullptr, nullptr, inst, nullptr);
    if (!g_panel) return;
    ThemeTitleBar(g_panel);
    int mica = 2;   // DWMSBT_MAINWINDOW (Windows 11 22H2+)
    g_mica = SUCCEEDED(DwmSetWindowAttribute(g_panel, 38 /* DWMWA_SYSTEMBACKDROP_TYPE */, &mica, sizeof(mica)));
    if (g_mica) {
        MARGINS mg{-1, -1, -1, -1};
        DwmExtendFrameIntoClientArea(g_panel, &mg);
    }
    ShowWindow(g_panel, SW_SHOWNORMAL);
    SetForegroundWindow(g_panel);
}
