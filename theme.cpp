#define WIN32_LEAN_AND_MEAN
#include "theme.h"
#include <dwmapi.h>
#pragma comment(lib, "dwmapi")
#pragma comment(lib, "advapi32")

bool ThemeIsDark() {
    DWORD v = 1, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
                 RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v == 0;
}

bool ThemeTaskbarDark() {
    DWORD v = 0, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"SystemUsesLightTheme",
                 RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v == 0;
}

// uxtheme exports these by ordinal only (Win10 1903+; 1809's #135 is AllowDarkModeForApp(BOOL), same effect with 1)
typedef int(WINAPI* SetPreferredAppModeFn)(int);
typedef void(WINAPI* FlushMenuThemesFn)();

static HMODULE Ux() {
    static HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return ux;
}

void ThemeInit() {
    if (auto f = (SetPreferredAppModeFn)GetProcAddress(Ux(), MAKEINTRESOURCEA(135))) f(1);   // 1 = AllowDark: follow the system
    ThemeRefresh();
}

void ThemeRefresh() {
    if (auto f = (FlushMenuThemesFn)GetProcAddress(Ux(), MAKEINTRESOURCEA(136))) f();
}

void ThemeTitleBar(HWND w) {
    BOOL dark = ThemeIsDark();
    DwmSetWindowAttribute(w, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
}

COLORREF ThemeAccent(bool dark) {
    BYTE pal[32];
    DWORD sz = sizeof(pal);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent", L"AccentPalette",
                     RRF_RT_REG_BINARY, nullptr, pal, &sz) == ERROR_SUCCESS && sz >= 32) {
        const BYTE* c = pal + (dark ? 1 : 4) * 4;   // AccentLight2 on dark, AccentDark1 on light (what Windows 11 uses)
        return RGB(c[0], c[1], c[2]);
    }
    DWORD abgr = 0;
    sz = sizeof(abgr);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", RRF_RT_REG_DWORD, nullptr, &abgr, &sz) == ERROR_SUCCESS)
        return RGB(abgr & 0xFF, (abgr >> 8) & 0xFF, (abgr >> 16) & 0xFF);
    return dark ? RGB(0x60, 0xCD, 0xFF) : RGB(0x00, 0x5F, 0xB8);
}

bool ThemeChanged(LPARAM l) {
    return l && !lstrcmpW((LPCWSTR)l, L"ImmersiveColorSet");
}
