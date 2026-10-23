// Backdrop blur: a slight Gaussian blur over the whole screen, independent of (and stacking with) the colour filter.
// Always starts off; only the strength is remembered. Strength is capped so the screen always stays readable enough
// to turn it off again.
#pragma once
#include <windows.h>

extern bool g_blurOn;
extern int g_blurStrength;            // percent of the cap (10..100)
extern bool g_blurKeyOk;              // a global shortcut for the blur got registered
extern const wchar_t* g_blurKeyName;  // e.g. L"Win+Alt+B"

void BlurInit(HWND notify, UINT failMsg);   // failMsg is posted if this PC can't do it (blur then turns itself off)
void BlurSetOn(bool on);              // fades in / out
void BlurSetStrength(int pct);        // clamped, saved, live
void BlurShutdown();
