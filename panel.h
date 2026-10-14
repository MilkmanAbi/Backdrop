// Backdrop "Colour filter" window: filter cards with live previews, intensity slider, sign-in default, schedule.
#pragma once
#include <windows.h>

extern bool g_hotkeyOk;               // a global shortcut for the filter got registered
extern const wchar_t* g_hotkeyName;   // e.g. L"Win+Alt+C"
void PanelShow(HINSTANCE inst);
void PanelUpdate();           // filter changed elsewhere (tray, hotkey, schedule): repaint if open
