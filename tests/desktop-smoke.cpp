#include "../backdrop.cpp"
#include <cstdio>

// Uses the production desktop attachment and media engine; never loads/saves
// settings, registers startup/hotkeys, or changes the system wallpaper.
int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2 || argc > 3) return 2;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    MFStartup(MF_VERSION);
    if (argc == 3) {
        volatile LONG cancel = 0;
        wchar_t error[512]{};
        ConvertArgs args{argv[1], argv[2], 1280, 720, FIT_FILL, nullptr, 0, &cancel};
        HRESULT hr = ConvertMedia(args, error, 512);
        printf("combined blur/converter build: conversion=0x%08X\n", unsigned(hr));
        if (FAILED(hr)) return 1;
    }
    InitializeCriticalSection(&g_cs);
    g_inst = GetModuleHandleW(nullptr);
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    WNDCLASSW wc{};
    wc.hInstance = g_inst; wc.lpfnWndProc = HostProc; wc.lpszClassName = L"BackdropHost";
    RegisterClassW(&wc);
    wc.lpfnWndProc = DefWindowProcW; wc.lpszClassName = L"BackdropSmoke";
    RegisterClassW(&wc);
    g_main = CreateWindowW(wc.lpszClassName, L"", 0, 0, 0, 0, 0, nullptr, nullptr, g_inst, nullptr);
    GetFullPathNameW(argv[argc - 1], MAX_PATH, g_play, nullptr);
    int failed = 0;
    for (int cycle = 0; cycle < 3; ++cycle) {
        printf("starting cycle=%d\n", cycle);
        bool engine = StartEngine();
        bool attached = engine && Attach();
        printf("engine=%d attach=%d\n", engine, attached);
        if (!engine || !attached || !Attached()) { ++failed; break; }
        printf("attached\n");
        RepairDesktopOrder();
        int frames = 0;
        g_engine->Play();
        DWORD start = GetTickCount();
        while (GetTickCount() - start < 1500) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
            LONGLONG pts;
            if (g_engine->OnVideoStreamTick(&pts) == S_OK) {
                ComPtr<ID3D11Texture2D> bb;
                RECT dst{0,0,LONG(g_w),LONG(g_h)};
                MFVideoNormalizedRect src{0,0,1,1}; MFARGB border{0,0,0,255};
                if (SUCCEEDED(g_swap->GetBuffer(0, IID_PPV_ARGS(&bb))) &&
                    SUCCEEDED(g_engine->TransferVideoFrame(bb.Get(), &src, &dst, &border)) &&
                    SUCCEEDED(g_swap->Present(0,0))) ++frames;
            }
            Sleep(5);
        }
        printf("desktop cycle=%d rendered=%d raised=%d parentValid=%d\n",cycle,frames,RaisedDesktop(Progman()),Attached());
        if (frames < 5) ++failed;
        StopEngine(); Detach(); ReleaseD3D();
        if (g_host || g_swap || g_engine || g_dev) ++failed;
    }
    StopEngine(); Detach(); ReleaseD3D();
    DestroyWindow(g_main);
    CloseHandle(g_wake);
    DeleteCriticalSection(&g_cs);
    MFShutdown(); CoUninitialize();
    return failed ? 1 : 0;
}
