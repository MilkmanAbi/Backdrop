// Follow Windows' light/dark app theme: tray menus, title bars, accent colour.
#pragma once
#include <windows.h>

bool ThemeIsDark();                 // Settings > Personalisation > Colours > "app mode"
bool ThemeTaskbarDark();            // "Windows mode" (taskbar / tray)
void ThemeInit();                   // let Win32 menus go dark when the app mode is dark
void ThemeRefresh();                // after WM_SETTINGCHANGE "ImmersiveColorSet"
void ThemeTitleBar(HWND w);         // dark / light caption to match
COLORREF ThemeAccent(bool dark);    // system accent, in the shade Windows uses for that mode
bool ThemeChanged(LPARAM settingChangeLParam);
