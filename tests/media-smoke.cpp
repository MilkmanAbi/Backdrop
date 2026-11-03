#include "../convert.cpp"
#include <mfapi.h>
#include <cstdio>
#include <cstdlib>
#include <thread>

// Exercises the production converter without touching wallpaper settings or startup tasks.
int wmain(int argc, wchar_t** argv) {
    if (argc < 3) return 2;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);
    Probe p;
    wchar_t error[512]{};
    bool probed = ProbeMedia(argv[1], p, error, 512);
    wprintf(L"probe=%d size=%dx%d fps=%.3f error=%s\n", probed, p.w, p.h, p.fps, error);
    bool cancelled = argc > 6 && !wcscmp(argv[6], L"cancel");
    bool software = argc > 6 && !wcscmp(argv[6], L"software");
    bool cancelLate = argc > 6 && !wcscmp(argv[6], L"cancel-late");
    volatile LONG cancel = cancelled ? 1 : 0;
    ConvertArgs a{argv[1], argv[2], argc > 3 ? _wtoi(argv[3]) : 1920,
        argc > 4 ? _wtoi(argv[4]) : 1080, Fit(argc > 5 ? _wtoi(argv[5]) : 0), nullptr, 0, &cancel};
    std::thread cancellation;
    if (cancelLate) cancellation = std::thread([&] { Sleep(30); InterlockedExchange(&cancel, 1); });
    HRESULT hr = software ? ConvertOnce(a, argv[2], false, error, 512) : ConvertMedia(a, error, 512);
    if (cancellation.joinable()) cancellation.join();
    wprintf(L"convert=0x%08X error=%s\n", unsigned(hr), error);
    MFShutdown();
    CoUninitialize();
    return cancel ? (hr == E_ABORT ? 0 : 1) : (SUCCEEDED(hr) ? 0 : 1);
}
