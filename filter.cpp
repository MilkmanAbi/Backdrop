#define WIN32_LEAN_AND_MEAN
#include "filter.h"
#include <magnification.h>
#include <cmath>
#pragma comment(lib, "magnification")
#pragma comment(lib, "advapi32")
#pragma comment(lib, "user32")

FilterSettings g_fs;
bool g_filterOn = false;
void (*g_filterChanged)() = nullptr;

static const wchar_t* KEY = L"Software\\Backdrop";
static const wchar_t* kNames[FILTER_COUNT] = {
    L"Sepia", L"Amber", L"Blue light filter", L"Greyscale", L"Washed out", L"Dim",
    L"Night red", L"Inverted", L"Red-green (green weak)", L"Red-green (red weak)", L"Blue-yellow",
};

const wchar_t* FilterName(int t) { return kNames[(t >= 0 && t < FILTER_COUNT) ? t : 0]; }

struct Mat {
    float m[3][3];
    float o[3];
};

static Mat Identity() { return Mat{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, {0, 0, 0}}; }

// colour-blind friendly: simulate the deficiency (Machado 2009, severity 1), push what's lost into channels still seen
static Mat Daltonize(const float s[3][3], const float d[3][3]) {
    Mat r = Identity();
    float e[3][3];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) e[i][j] = (i == j) - s[i][j];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            float v = 0;
            for (int k = 0; k < 3; k++) v += d[i][k] * e[k][j];
            r.m[i][j] += v;
        }
    return r;
}

static Mat Full(int t) {
    static const float L[3] = {0.2126f, 0.7152f, 0.0722f};   // Rec.709 luma
    static const float toGB[3][3] = {{0, 0, 0}, {0.7f, 1, 0}, {0.7f, 0, 1}}, toRG[3][3] = {{1, 0, 0.7f}, {0, 1, 0.7f}, {0, 0, 0}};
    static const float deut[3][3] = {{0.367322f, 0.860646f, -0.227968f}, {0.280085f, 0.672501f, 0.047413f}, {-0.011820f, 0.042940f, 0.968881f}};
    static const float prot[3][3] = {{0.152286f, 1.052583f, -0.204868f}, {0.114503f, 0.786281f, 0.099216f}, {-0.003882f, -0.048116f, 1.051998f}};
    static const float trit[3][3] = {{1.255528f, -0.076749f, -0.178779f}, {-0.078411f, 0.930809f, 0.147602f}, {0.004733f, 0.691367f, 0.303900f}};
    Mat r{};
    auto diag = [&](float a, float b, float c) { r.m[0][0] = a; r.m[1][1] = b; r.m[2][2] = c; };
    switch (t) {
    case 0: {   // classic sepia, normalised so white -> warm paper instead of clipping
        static const float s[3][3] = {{0.291f, 0.569f, 0.140f}, {0.258f, 0.508f, 0.124f}, {0.201f, 0.395f, 0.097f}};
        memcpy(r.m, s, sizeof(s));
        break;
    }
    case 1: diag(1.0f, 0.64f, 0.16f); break;    // amber
    case 2: diag(1.0f, 0.87f, 0.64f); break;    // blue light filter (~4000 K)
    case 3: for (auto& row : r.m) memcpy(row, L, sizeof(L)); break;   // greyscale
    case 4:                                     // washed out: half the saturation, lifted blacks, softer whites
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 3; j++) r.m[i][j] = 0.72f * (0.45f * (i == j) + 0.55f * L[j]);
            r.o[i] = 0.20f;
        }
        break;
    case 5: diag(0.35f, 0.35f, 0.35f); break;   // dim (below the backlight's minimum)
    case 6: memcpy(r.m[0], L, sizeof(L)); break;   // night red: brightness kept in red only
    case 7: diag(-1, -1, -1); r.o[0] = r.o[1] = r.o[2] = 1; break;   // inverted
    case 8: r = Daltonize(deut, toGB); break;   // red-green, green weak (deuteranopia)
    case 9: r = Daltonize(prot, toGB); break;   // red-green, red weak (protanopia)
    case 10: r = Daltonize(trit, toRG); break;  // blue-yellow (tritanopia)
    default: r = Identity();
    }
    return r;
}

void FilterMatrix(int type, float k, float m[3][3], float off[3]) {
    Mat f = Full(type), id = Identity();
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) m[i][j] = id.m[i][j] + k * (f.m[i][j] - id.m[i][j]);
        off[i] = k * f.o[i];
    }
}

// ------------------------------------------------------------------ settings
static void Dword(HKEY k, const wchar_t* n, int* v) {
    DWORD d, sz = sizeof(d);
    if (RegGetValueW(k, nullptr, n, RRF_RT_REG_DWORD, nullptr, &d, &sz) == ERROR_SUCCESS) *v = int(d);
}

static void FilterLoad() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, KEY, 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    int login = 0, sched = 0;
    Dword(k, L"FilterType", &g_fs.type);
    Dword(k, L"FilterIntensity", &g_fs.intensity);
    Dword(k, L"FilterAtLogin", &login);
    Dword(k, L"FilterSchedule", &sched);
    Dword(k, L"FilterFrom", &g_fs.from);
    Dword(k, L"FilterTo", &g_fs.to);
    RegCloseKey(k);
    g_fs.atLogin = login != 0;
    g_fs.schedule = sched != 0;
    if (g_fs.type < 0 || g_fs.type >= FILTER_COUNT) g_fs.type = 0;
    g_fs.intensity = g_fs.intensity < 0 ? 0 : g_fs.intensity > 100 ? 100 : g_fs.intensity;
    g_fs.from = ((g_fs.from % 1440) + 1440) % 1440;
    g_fs.to = ((g_fs.to % 1440) + 1440) % 1440;
}

void FilterSave() {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, KEY, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    DWORD v[] = {DWORD(g_fs.type), DWORD(g_fs.intensity), DWORD(g_fs.atLogin), DWORD(g_fs.schedule), DWORD(g_fs.from), DWORD(g_fs.to)};
    const wchar_t* n[] = {L"FilterType", L"FilterIntensity", L"FilterAtLogin", L"FilterSchedule", L"FilterFrom", L"FilterTo"};
    for (int i = 0; i < 6; i++) RegSetValueExW(k, n[i], 0, REG_DWORD, (const BYTE*)&v[i], sizeof(DWORD));
    RegCloseKey(k);
}

// ------------------------------------------------------------------ the screen effect
static bool g_magInit = false;
static Mat g_cur = Identity(), g_from, g_to;
static DWORD g_t0, g_dur;
static UINT_PTR g_fadeTimer, g_retryTimer, g_schedTimer;
static int g_retries;

static bool IsIdentity(const Mat& m) {
    Mat id = Identity();
    for (int i = 0; i < 3; i++) {
        if (std::fabs(m.o[i]) > 1e-4f) return false;
        for (int j = 0; j < 3; j++)
            if (std::fabs(m.m[i][j] - id.m[i][j]) > 1e-4f) return false;
    }
    return true;
}

static bool Push(const Mat& m) {
    bool ident = IsIdentity(m);
    if (!g_magInit) {
        if (ident) return true;   // already untouched
        if (!(g_magInit = MagInitialize() != FALSE)) return false;
    }
    MAGCOLOREFFECT e{};
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) e.transform[j][i] = m.m[i][j];   // Magnification: row = input channel, column = output
        e.transform[4][i] = m.o[i];
    }
    e.transform[3][3] = e.transform[4][4] = 1;
    bool ok = MagSetFullscreenColorEffect(&e) != FALSE;
    if (ok && ident) { MagUninitialize(); g_magInit = false; }   // fully off: let DWM compose untouched again
    return ok;
}

static Mat Target() {
    Mat t = Identity();
    if (g_filterOn) FilterMatrix(g_fs.type, g_fs.intensity / 100.0f, t.m, t.o);
    return t;
}

static void Notify() { if (g_filterChanged) g_filterChanged(); }

// at sign-in DWM / the session may not take the effect for a moment: keep trying briefly
static void CALLBACK RetryTick(HWND, UINT, UINT_PTR, DWORD) {
    if (Push(g_cur) || ++g_retries > 200) { KillTimer(nullptr, g_retryTimer); g_retryTimer = 0; }
}

static void PushOrRetry(const Mat& m) {
    g_cur = m;
    if (!Push(m) && !g_retryTimer) { g_retries = 0; g_retryTimer = SetTimer(nullptr, 0, 50, RetryTick); }
}

static void CALLBACK FadeTick(HWND, UINT, UINT_PTR, DWORD) {
    float p = g_dur ? float(GetTickCount() - g_t0) / g_dur : 1.0f;
    if (p >= 1) p = 1;
    float e = p * p * (3 - 2 * p);   // smoothstep
    Mat m;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) m.m[i][j] = g_from.m[i][j] + e * (g_to.m[i][j] - g_from.m[i][j]);
        m.o[i] = g_from.o[i] + e * (g_to.o[i] - g_from.o[i]);
    }
    PushOrRetry(m);
    if (p >= 1 && g_fadeTimer) { KillTimer(nullptr, g_fadeTimer); g_fadeTimer = 0; }
}

static void MoveTo(int fadeMs) {
    if (fadeMs <= 0) {
        if (g_fadeTimer) { KillTimer(nullptr, g_fadeTimer); g_fadeTimer = 0; }
        PushOrRetry(Target());
        return;
    }
    g_from = g_cur;
    g_to = Target();
    g_t0 = GetTickCount();
    g_dur = DWORD(fadeMs);
    if (!g_fadeTimer) g_fadeTimer = SetTimer(nullptr, 0, 15, FadeTick);
    FadeTick(nullptr, 0, 0, 0);
}

void FilterSetOn(bool on, int fadeMs) {
    g_filterOn = on;
    MoveTo(fadeMs);
    Notify();
}

void FilterUpdate(int fadeMs) {
    MoveTo(fadeMs);
    Notify();
}

void FilterShutdown() {
    if (g_fadeTimer) KillTimer(nullptr, g_fadeTimer);
    if (g_retryTimer) KillTimer(nullptr, g_retryTimer);
    g_fadeTimer = g_retryTimer = 0;
    Push(Identity());
    if (g_magInit) { MagUninitialize(); g_magInit = false; }
}

// ------------------------------------------------------------------ schedule
static bool g_schedState;   // what the schedule said at the last check

static bool InWindow(int now) {
    if (g_fs.from == g_fs.to) return false;
    return g_fs.from < g_fs.to ? (now >= g_fs.from && now < g_fs.to) : (now >= g_fs.from || now < g_fs.to);   // may wrap midnight
}

static int MinutesNow(SYSTEMTIME* st = nullptr) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    if (st) *st = t;
    return t.wHour * 60 + t.wMinute;
}

bool ScheduleActiveNow() { return InWindow(MinutesNow()); }

void FilterStartup() {
    FilterLoad();
    g_schedState = ScheduleActiveNow();
    g_filterOn = g_fs.atLogin || (g_fs.schedule && g_schedState);
    if (g_filterOn) PushOrRetry(Target());   // instant at sign-in, no fade
    ScheduleArm();
}

void ScheduleFollow(int fadeMs) {
    g_schedState = ScheduleActiveNow();
    if (g_fs.schedule) FilterSetOn(g_schedState, fadeMs);
    ScheduleArm();
}

void ScheduleCheck(int fadeMs) {
    bool s = ScheduleActiveNow();
    if (g_fs.schedule && s != g_schedState) FilterSetOn(s, fadeMs);
    g_schedState = s;
    ScheduleArm();
}

static void CALLBACK SchedTick(HWND, UINT, UINT_PTR, DWORD) { ScheduleCheck(4000); }   // slow, Night-light-style fade

void ScheduleArm() {
    if (g_schedTimer) { KillTimer(nullptr, g_schedTimer); g_schedTimer = 0; }
    if (!g_fs.schedule || g_fs.from == g_fs.to) return;
    SYSTEMTIME st;
    int now = MinutesNow(&st);
    auto until = [now](int m) { int d = (m - now + 1440) % 1440; return d ? d : 1440; };
    int mins = until(g_fs.from) < until(g_fs.to) ? until(g_fs.from) : until(g_fs.to);
    DWORD ms = DWORD(mins) * 60000 - st.wSecond * 1000 - st.wMilliseconds + 500;   // just past the next on/off minute
    // never sleep longer than 5 min: a timer can drift across sleep, DST or time-zone changes that send no message;
    // re-checking costs nothing and ScheduleCheck only acts when the schedule's state actually flipped
    if (ms > 5 * 60000) ms = 5 * 60000;
    g_schedTimer = SetTimer(nullptr, 0, ms, SchedTick);
}
