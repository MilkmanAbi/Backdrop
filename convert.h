// VidWall converter: one-time GPU transcode of any supported media (MP4/MOV/AVI/MKV/WMV video, GIF, WebP)
// into an H.264 MP4 baked at the display's resolution for the chosen fit, so playback never scales.
#pragma once
#include <windows.h>

enum Fit : int { FIT_FILL = 0, FIT_FIT, FIT_STRETCH, FIT_CENTER };

struct Probe {
    int w = 0, h = 0;          // as displayed (rotation applied)
    int rotation = 0;          // 0 / 90 / 180 / 270
    bool image = false;        // GIF / WebP (animated or still)
    bool h264 = false;
    double fps = 0;
};

// Output size of the baked file for a source of w x h on a sw x sh screen (even, within encoder limits).
void BakedSize(int w, int h, int sw, int sh, Fit fit, int* ow, int* oh);
// How much the source gets enlarged on screen with this fit (>= 2 means it will look soft).
double Upscale(int w, int h, int sw, int sh, Fit fit);

bool ProbeMedia(const wchar_t* path, Probe& p, wchar_t* err, size_t errLen);
// True when the file can be played as-is (already H.264, upright, exactly the baked size).
bool PlayableAsIs(const wchar_t* path, const Probe& p, int sw, int sh, Fit fit);

struct ConvertArgs {
    const wchar_t* src;
    const wchar_t* dst;
    int sw, sh;
    Fit fit;
    HWND notify;            // receives progressMsg with wParam = 0..100
    UINT progressMsg;
    volatile LONG* cancel;  // non-zero = stop
};
// S_OK, E_ABORT when cancelled, otherwise an error with a readable message.
HRESULT ConvertMedia(const ConvertArgs& a, wchar_t* err, size_t errLen);
