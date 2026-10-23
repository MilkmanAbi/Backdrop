// backdrop-ui.exe - Backdrop's optional UI process: the Backdrop app window and desktop widgets, rendered by Sciter.JS.
// It is started only on demand (the app is opened, or widgets are enabled - and then only after the core has the
// wallpaper and colour filter up), so the core (backdrop.exe) never loads sciter.dll and its sign-in path is untouched.
//
//  - widgets: one borderless, per-pixel-transparent tool window each (no taskbar button, not in Alt+Tab / Task View),
//    kept at desktop level (bottom of the z-order). Widgets are full web pages: network, fetch/WebSocket, eval and
//    Sciter's file/system modules all work, so people can build whatever they like (games, live feeds, animations).
//  - app window: Windows 11 style (Mica), the only window that gets native functions (Window.this.assetInterface).
//  - core IPC: WM_COPYDATA commands to the core's "BackdropMain" window, which replies with its state.
//  - settings: HKCU\Software\Backdrop (WidgetsEnabled, WidgetsAtLogin, GridSize) and ...\Backdrop\Widgets\<id> (one key per widget).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>
#include <commctrl.h>
#include <shlobj.h>
#include "sciter-x-window.hpp"
#include "../theme.h"
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <cstdio>

#pragma comment(lib, "user32")
#pragma comment(lib, "gdi32")
#pragma comment(lib, "shell32")
#pragma comment(lib, "comdlg32")
#pragma comment(lib, "comctl32")
#pragma comment(lib, "advapi32")
#pragma comment(lib, "ole32")
#pragma comment(lib, "d3d11")

namespace sciter { namespace application {
  static std::vector<sciter::string> s_argv;
  const std::vector<sciter::string>& argv() { return s_argv; }
  HINSTANCE hinstance() { return GetModuleHandleW(nullptr); }
} }

static const wchar_t* KEY = L"Software\\Backdrop";
static const wchar_t* WKEY = L"Software\\Backdrop\\Widgets";
static const UINT IPC_COMMAND = 0xBD01, IPC_STATE = 0xBD02, IPC_UI = 0xBD03;
static const wchar_t* VERSION = L"1.4.0";

static std::wstring g_exeDir, g_userWidgets, g_builtinWidgets, g_uiDir;
static HWND g_msg;                 // hidden window: IPC endpoint + single-instance target
static std::wstring g_reply;       // last state JSON from the core

// ------------------------------------------------------------------ small helpers
static std::wstring Esc(const std::wstring& s) {   // JSON string escape
    std::wstring o;
    for (wchar_t c : s) {
        if (c == L'"' || c == L'\\') { o += L'\\'; o += c; }
        else if (c < 32) { wchar_t b[8]; swprintf_s(b, L"\\u%04x", c); o += b; }
        else o += c;
    }
    return o;
}

static std::wstring FileUrl(const std::wstring& path) {
    std::wstring u = L"file:///";
    for (wchar_t c : path) {
        if (c == L'\\') u += L'/';
        else if (c == L' ') u += L"%20";
        else if (c == L'#') u += L"%23";
        else if (c == L'%') u += L"%25";
        else u += c;
    }
    return u;
}

static bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

static DWORD RegDword(const wchar_t* key, const wchar_t* name, DWORD def) {
    DWORD v = def, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v;
}
static void RegSetDword(const std::wstring& key, const wchar_t* name, DWORD v) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
        RegCloseKey(k);
    }
}
static void RegSetStr(const std::wstring& key, const wchar_t* name, const std::wstring& v) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)v.c_str(), DWORD((v.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(k);
    }
}
static std::wstring RegStr(const std::wstring& key, const wchar_t* name) {
    wchar_t buf[1024];
    DWORD sz = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), name, RRF_RT_REG_SZ, nullptr, buf, &sz) != ERROR_SUCCESS) return L"";
    return buf;
}

// ------------------------------------------------------------------ core IPC
static HWND Core() { return FindWindowW(L"BackdropMain", nullptr); }

static bool CoreSend(const std::wstring& cmd) {
    HWND c = Core();
    if (!c) return false;
    DWORD pid = 0;   // the core may open a window for this (Choose…, the filter panel): let it come to the front
    GetWindowThreadProcessId(c, &pid);
    AllowSetForegroundWindow(pid);
    COPYDATASTRUCT cds{IPC_COMMAND, DWORD((cmd.size() + 1) * sizeof(wchar_t)), (void*)cmd.c_str()};
    DWORD_PTR r = 0;
    return SendMessageTimeoutW(c, WM_COPYDATA, (WPARAM)g_msg, (LPARAM)&cds, SMTO_ABORTIFHUNG, 3000, &r) && r;
}

static std::wstring CoreState() {
    g_reply.clear();
    if (!CoreSend(L"state?")) return L"{\"running\":false}";
    return g_reply.empty() ? L"{\"running\":false}" : g_reply;   // the core answered with WM_COPYDATA during the send
}

// ------------------------------------------------------------------ widget registry
struct WidgetDef {
    std::wstring id, name, entry;   // entry = full path of the widget's html file
    bool enabled = false, builtin = false;
    int x = INT_MIN, y = INT_MIN;
};

static std::vector<WidgetDef> LoadWidgets() {
    std::vector<WidgetDef> v;
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, WKEY, 0, KEY_READ, &k) != ERROR_SUCCESS) return v;
    wchar_t sub[128];
    for (DWORD i = 0;; i++) {
        DWORD n = 128;
        if (RegEnumKeyExW(k, i, sub, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        std::wstring key = std::wstring(WKEY) + L"\\" + sub;
        WidgetDef w;
        w.id = sub;
        w.name = RegStr(key, L"Name");
        w.entry = RegStr(key, L"Entry");
        w.enabled = RegDword(key.c_str(), L"Enabled", 1) != 0;
        w.builtin = RegDword(key.c_str(), L"Builtin", 0) != 0;
        std::wstring gallery = RegStr(key, L"Gallery");   // built-ins follow the install folder
        if (w.builtin && !gallery.empty()) w.entry = g_builtinWidgets + L"\\" + gallery + L"\\index.html";
        w.x = int(RegDword(key.c_str(), L"X", DWORD(INT_MIN)));
        w.y = int(RegDword(key.c_str(), L"Y", DWORD(INT_MIN)));
        if (!w.entry.empty()) v.push_back(w);
    }
    RegCloseKey(k);
    return v;
}

static std::wstring TitleOf(const std::wstring& html, const std::wstring& fallback) {
    HANDLE f = CreateFileW(html.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return fallback;
    char buf[8192];
    DWORD got = 0;
    ReadFile(f, buf, sizeof(buf) - 1, &got, nullptr);
    CloseHandle(f);
    buf[got] = 0;
    std::string s(buf);
    std::string lo = s;
    std::transform(lo.begin(), lo.end(), lo.begin(), ::tolower);
    size_t a = lo.find("<title>"), b = lo.find("</title>");
    if (a == std::string::npos || b == std::string::npos || b <= a + 7) return fallback;
    std::string t = s.substr(a + 7, b - a - 7);
    int n = MultiByteToWideChar(CP_UTF8, 0, t.c_str(), int(t.size()), nullptr, 0);
    std::wstring w(size_t(n), L' ');
    MultiByteToWideChar(CP_UTF8, 0, t.c_str(), int(t.size()), &w[0], n);
    return w.empty() ? fallback : w;
}

// built-in sample widgets shipped in <app>\widgets\<id>\index.html
static std::vector<WidgetDef> Gallery() {
    std::vector<WidgetDef> v;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((g_builtinWidgets + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return v;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        WidgetDef w;
        w.id = fd.cFileName;
        w.entry = g_builtinWidgets + L"\\" + fd.cFileName + L"\\index.html";
        if (!Exists(w.entry)) continue;
        w.name = TitleOf(w.entry, w.id);
        w.builtin = true;
        v.push_back(w);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return v;
}

static std::wstring UniqueId(std::wstring base) {
    for (auto& c : base) if (!iswalnum(c) && c != L'-' && c != L'_') c = L'-';
    if (base.empty()) base = L"widget";
    std::wstring id = base;
    for (int n = 2;; n++) {
        HKEY k;
        bool taken = RegOpenKeyExW(HKEY_CURRENT_USER, (std::wstring(WKEY) + L"\\" + id).c_str(), 0, KEY_READ, &k) == ERROR_SUCCESS;
        if (taken) RegCloseKey(k);
        if (!taken && !Exists(g_userWidgets + L"\\" + id)) return id;
        id = base + L"-" + std::to_wstring(n);
    }
}

static void RegisterWidget(const std::wstring& id, const std::wstring& name, const std::wstring& entry, bool builtin, const std::wstring& gallery = L"") {
    std::wstring key = std::wstring(WKEY) + L"\\" + id;
    RegSetStr(key, L"Name", name);
    RegSetStr(key, L"Entry", entry);
    RegSetDword(key, L"Enabled", 1);
    RegSetDword(key, L"Builtin", builtin ? 1 : 0);
    if (!gallery.empty()) RegSetStr(key, L"Gallery", gallery);
}

// install a user widget: a single .html / .htm file, or a .zip with an index.html (extracted by Windows' bsdtar, which
// refuses absolute paths and ".." entries)
static std::wstring InstallWidget(const std::wstring& file, std::wstring& err) {
    std::wstring ext = file.substr(file.find_last_of(L'.') + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    std::wstring stem = file.substr(file.find_last_of(L'\\') + 1);
    stem = stem.substr(0, stem.find_last_of(L'.'));
    if (!_wcsicmp(stem.c_str(), L"index")) {   // ...\clock\index.html: name it after its folder
        std::wstring parent = file.substr(0, file.find_last_of(L'\\'));
        stem = parent.substr(parent.find_last_of(L'\\') + 1);
    }
    std::wstring id = UniqueId(stem);
    std::wstring dir = g_userWidgets + L"\\" + id;
    CreateDirectoryW(g_userWidgets.c_str(), nullptr);
    if (!CreateDirectoryW(dir.c_str(), nullptr)) { err = L"Couldn't create the widget folder."; return L""; }
    std::wstring entry;
    if (ext == L"html" || ext == L"htm") {
        RemoveDirectoryW(dir.c_str());   // used in place: assets next to it keep working, edits show up on Reload
        entry = file;
    } else if (ext == L"zip") {
        wchar_t sys[MAX_PATH];
        GetSystemDirectoryW(sys, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(sys) + L"\\tar.exe\" -xf \"" + file + L"\" -C \"" + dir + L"\"";
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            err = L"Couldn't unpack the zip."; return L"";
        }
        WaitForSingleObject(pi.hProcess, 30000);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        // index.html at the top, or inside a single top-level folder
        entry = dir + L"\\index.html";
        if (!Exists(entry)) {
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.' &&
                        Exists(dir + L"\\" + fd.cFileName + L"\\index.html")) { entry = dir + L"\\" + fd.cFileName + L"\\index.html"; break; }
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
        }
        if (code != 0 || !Exists(entry)) { err = L"That zip has no index.html in it."; return L""; }
    } else {
        err = L"Pick an .html file or a .zip with an index.html."; return L"";
    }
    RegisterWidget(id, TitleOf(entry, stem), entry, false);
    return id;
}

static void DeleteTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            std::wstring p = dir + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) DeleteTree(p); else DeleteFileW(p.c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

// ------------------------------------------------------------------ widget windows
class WidgetWin;
static std::map<std::wstring, sciter::om::hasset<WidgetWin>> g_widgets;
static void SyncWidgets();
static void MaybeQuit();

static void ShowApp();
static std::wstring WidgetKey(const std::wstring& id) { return std::wstring(WKEY) + L"\\" + id; }

// the widget's <meta name="widget-size" content="WxH"> (CSS px; default 360x240) and <meta name="widget-fps" content="N">
// (default 24), read natively so the window is created at its final size before the page is even parsed
struct WidgetMeta { SIZE size{360, 240}; int fps = 24; bool keyboard = false; };

static WidgetMeta ReadMeta(const std::wstring& html) {
    WidgetMeta wm;
    HANDLE f = CreateFileW(html.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return wm;
    char buf[8192];
    DWORD got = 0;
    ReadFile(f, buf, sizeof(buf) - 1, &got, nullptr);
    CloseHandle(f);
    buf[got] = 0;
    std::string lo(buf);
    std::transform(lo.begin(), lo.end(), lo.begin(), ::tolower);
    auto content = [&](const char* name) -> const char* {   // digits of the content="" of <meta name=...>
        size_t m = lo.find(name);
        if (m == std::string::npos) return nullptr;
        size_t tagA = lo.rfind('<', m), tagB = lo.find('>', m);
        if (tagA == std::string::npos || tagB == std::string::npos) return nullptr;
        size_t c = lo.find("content", tagA);
        if (c == std::string::npos || c > tagB) return nullptr;
        size_t d = lo.find_first_of("0123456789", c);
        return d == std::string::npos || d > tagB ? nullptr : lo.c_str() + d;
    };
    int w = 0, h = 0, fps = 0;
    if (const char* p = content("widget-size"))
        if (sscanf_s(p, "%d%*[ x,]%d", &w, &h) == 2 && w > 0 && h > 0 && w < 8000 && h < 8000) wm.size = {w, h};
    if (const char* p = content("widget-fps"))
        if (sscanf_s(p, "%d", &fps) == 1) wm.fps = std::max(1, std::min(240, fps));
    {   // <meta name="widget-keyboard" content="on">: clicking the widget focuses it so it gets key presses
        size_t m = lo.find("widget-keyboard");
        if (m != std::string::npos) {
            size_t tagB = lo.find('>', m), c = lo.find("content", lo.rfind('<', m));
            if (c != std::string::npos && c < tagB) {
                std::string v = lo.substr(c, tagB - c);
                wm.keyboard = v.find("on") != std::string::npos || v.find("true") != std::string::npos || v.find("yes") != std::string::npos;
            }
        }
    }
    return wm;
}

// SVG path data scaled from CSS px to device px (Sciter's border-shape takes device px). Arc flags/rotation stay as-is.
static std::string ScalePath(const std::string& d, double k) {
    std::string out;
    char cmd = 0;
    int n = 0;
    for (size_t i = 0; i < d.size();) {
        char c = d[i];
        if (isalpha((unsigned char)c) && c != 'e' && c != 'E') { cmd = c; n = 0; out += ' '; out += c; i++; continue; }
        if (isdigit((unsigned char)c) || c == '-' || c == '.' || c == '+') {
            char* end = nullptr;
            double v = strtod(d.c_str() + i, &end);
            size_t len = size_t(end - (d.c_str() + i));
            if (!len) { i++; continue; }
            bool keep = (cmd == 'A' || cmd == 'a') && (n % 7 >= 2 && n % 7 <= 4);
            char buf[32];
            snprintf(buf, sizeof(buf), "%g", keep ? v : v * k);
            if (!out.empty() && !isalpha((unsigned char)out.back())) out += ',';
            out += buf;
            n++;
            i += len;
            continue;
        }
        i++;   // separators
    }
    return out;
}

// custom shapes: any element with data-shape="<SVG path in CSS px>" is clipped to that path, content and all (images,
// GIFs, video...). Done here, before the page loads, as a generated stylesheet using Sciter's border-shape.
static std::string ShapeStyles(const std::string& s, const std::string& lo, double k) {
    std::string css;
    for (size_t at = lo.find("data-shape"); at != std::string::npos; at = lo.find("data-shape", at + 10)) {
        size_t q = lo.find_first_not_of(" \t\r\n=", at + 10);
        if (q == std::string::npos || (s[q] != '"' && s[q] != '\'')) continue;
        size_t e = s.find(s[q], q + 1);
        if (e == std::string::npos) continue;
        std::string d = s.substr(q + 1, e - q - 1);
        if (d.find('"') != std::string::npos || d.find('\\') != std::string::npos) continue;
        css += "[data-shape=\"" + d + "\"] { border-shape: path(" + ScalePath(d, k) + "); overflow: hidden; }\n";
    }
    return css;
}

// load the widget's own html as the window's document, adding window-frame="transparent" to its <html> tag so the
// window is borderless and per-pixel transparent. The base URL is the file itself, so relative assets just work.
static bool LoadWidgetPage(HWND h, const std::wstring& html, double zoom = 1.0) {
    HANDLE f = CreateFileW(html.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER n{};
    GetFileSizeEx(f, &n);
    std::string s(size_t(n.QuadPart), '\0');
    DWORD got = 0;
    ReadFile(f, &s[0], DWORD(s.size()), &got, nullptr);
    CloseHandle(f);
    s.resize(got);
    std::string lo = s;
    std::transform(lo.begin(), lo.end(), lo.begin(), ::tolower);
    const std::string attr = " window-frame=\"transparent\"";
    size_t at = lo.find("<html");
    std::string shapes = ShapeStyles(s, lo, GetDpiForWindow(h) / 96.0 * zoom);
    if (std::fabs(zoom - 1.0) > 0.001) {   // the user resized it: scale the whole page, like a picture
        char z[64];
        snprintf(z, sizeof(z), "html { zoom: %.4f; }\n", zoom);
        shapes = z + shapes;
    }
    if (!shapes.empty()) {   // ahead of the page's own styles
        size_t head = lo.find("<head");
        size_t ins = head != std::string::npos ? lo.find('>', head) : std::string::npos;
        std::string block = "<style>\n" + shapes + "</style>";
        if (ins != std::string::npos) s.insert(ins + 1, block);
        else s = block + s;
        lo = s;
        std::transform(lo.begin(), lo.end(), lo.begin(), ::tolower);
    }
    at = lo.find("<html");
    if (at != std::string::npos && (lo[at + 5] == '>' || isspace((unsigned char)lo[at + 5]))) s.insert(at + 5, attr);
    else s = "<html" + attr + ">" + s + "</html>";
    // "#backdrop-widget" marks the document as a widget for the init script (relative URLs ignore the fragment)
    return SciterLoadHtml(h, (LPCBYTE)s.data(), UINT(s.size()), (FileUrl(html) + L"#backdrop-widget").c_str()) != FALSE;
}

// ---- widget outline -> window region: what makes the empty space around a widget's elements click-through. Applied
// once by the window manager, so it costs nothing per frame (Sciter's Window.shape re-applied its mask every frame).
// Boxes only: a curved region sends the window down a slow composition path (~10% -> ~70% of a core for a GIF widget).
// Spec from the widget page: one "R x y w h" per line, device px relative to the window.
struct WidgetState { std::wstring id, entry; bool move = false, keyboard = false; int fps = 24, w = 0, h = 0, bw = 0, bh = 0; double scale = 1.0; RECT vis{}; };   // bw/bh: size at 100%   // vis: visible edges, window-relative
static bool g_arrange = false;   // "Arrange widgets": grid overlay up, widgets lifted above it and snapping
static WidgetState* StateOf(HWND h);
static void SetOutline(HWND h, const std::string& spec) {
    WidgetState* st = StateOf(h);
    if (spec.empty()) {
        if (st) st->vis = RECT{};
        if (!g_arrange) SetWindowRgn(h, nullptr, TRUE);
        return;
    }
    RECT bound{INT_MAX, INT_MAX, INT_MIN, INT_MIN};   // the visible edges: bounding box of the boxes
    HRGN all = CreateRectRgn(0, 0, 0, 0);
    for (size_t at = 0; at < spec.size();) {
        size_t nl = spec.find('\n', at);
        std::string line = spec.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        at = nl == std::string::npos ? spec.size() : nl + 1;
        double x, y, w, hh;
        if (line.size() > 2 && line[0] == 'R' && sscanf_s(line.c_str() + 1, "%lf %lf %lf %lf", &x, &y, &w, &hh) == 4) {
            double f = GetDpiForWindow(h) / 96.0;   // CSS px -> device px
            x *= f; y *= f; w *= f; hh *= f;
            HRGN part = CreateRectRgn(int(x), int(y), int(x + w + 0.999), int(y + hh + 0.999));
            bound = RECT{std::min(bound.left, LONG(x)), std::min(bound.top, LONG(y)), std::max(bound.right, LONG(x + w + 0.999)), std::max(bound.bottom, LONG(y + hh + 0.999))};
            CombineRgn(all, all, part, RGN_OR);
            DeleteObject(part);
        }
    }
    if (st && bound.right > bound.left) st->vis = bound;
    if (g_arrange || !SetWindowRgn(h, all, TRUE)) DeleteObject(all);   // the system owns the region from here
}

// per-window state, owned by the subclass

static const UINT WM_WIDGET_MENU = WM_APP + 10, WM_RUN_DONE = WM_APP + 20, WM_ARRANGE_END = WM_APP + 30;
static DWORD g_frameMs = 1000 / 24;
static bool g_appOpen = false;
static void PlaceWidget(HWND h, int x, int y, int W, int H, UINT flags);
static void BeginArrange();
static void EndArrange();
static bool FreeMove();
static void RepaintOverlays(bool rebuild);
static RECT VisOf(HWND h);
static void DragBegin(HWND h, bool resize);
static void DragMove();
static void DragEnd();
static HWND DraggingWidget();
static HWND g_dragging;
static void RunCommand(HWND widget, const std::wstring& cmd, const std::wstring& opts, const sciter::value& cb);

class WidgetWin : public sciter::window {
public:
    std::wstring id;
    WidgetWin(const WidgetDef& d) : sciter::window(0), id(d.id) {}
    // Window.this.backdrop in a widget page (the init script wraps these as globalThis.backdrop)
    bool widgetMenu() { PostMessageW(get_hwnd(), WM_WIDGET_MENU, 0, 0); return true; }
    bool closeWidget() {
        RegSetDword(WidgetKey(id), L"Enabled", 0);
        PostMessageW(g_msg, WM_APP + 2, 0, 0);   // sync after this call returns (the window is closing itself)
        return true;
    }
    bool openApp() { PostMessageW(g_msg, WM_APP + 3, 0, 0); return true; }
    bool setOutline(sciter::string spec) {   // see SetOutline; "" = the whole window
        int n = WideCharToMultiByte(CP_UTF8, 0, spec.c_str(), int(spec.size()), nullptr, 0, nullptr, nullptr);
        std::string u(size_t(n), ' ');
        WideCharToMultiByte(CP_UTF8, 0, spec.c_str(), int(spec.size()), &u[0], n, nullptr, nullptr);
        SetOutline(get_hwnd(), u);
        return true;
    }
    // run a command (PowerShell by default), result comes back as JSON to cb: see RunCommand
    bool run(sciter::string cmd, sciter::string opts, sciter::value cb) { RunCommand(get_hwnd(), cmd, opts, cb); return true; }
    SOM_PASSPORT_BEGIN_EX(backdrop, WidgetWin)
      SOM_FUNCS(SOM_FUNC(widgetMenu), SOM_FUNC(closeWidget), SOM_FUNC(openApp), SOM_FUNC(setOutline), SOM_FUNC(run))
    SOM_PASSPORT_END
};

static WidgetState* StateOf(HWND h);

static void SetMove(WidgetState* st, bool on) { st->move = on; }   // the outline is drawn natively (Arrange overlay)

static void SavePos(HWND h, const std::wstring& id) {
    RECT r;
    GetWindowRect(h, &r);
    RegSetDword(WidgetKey(id), L"X", DWORD(r.left));
    RegSetDword(WidgetKey(id), L"Y", DWORD(r.top));
}

// ---- grid: the spacing widgets snap to (Arrange widgets, Snap to). Stored in DIP, used in device px.
static int GridDip() { return std::max(8, std::min(128, int(RegDword(KEY, L"GridSize", 24)))); }
static int GridPx(HWND h) { return MulDiv(GridDip(), int(GetDpiForWindow(h)), 96); }

static RECT WorkAreaOf(HWND h) {
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
    return mi.rcWork;
}

// resize a widget to scale s (1 = its own widget-size), keeping the top-left of what it draws in place. The page is
// zoomed as a whole, so text, images, GIFs and shapes all scale together. Remembered per widget.
static void ApplyScale(HWND h, WidgetState* st, double s, bool save) {
    s = std::max(0.25, std::min(4.0, s));
    double ratio = s / st->scale;
    RECT wr;
    GetWindowRect(h, &wr);
    RECT v = st->vis;   // visible edges scale with the page
    st->vis = RECT{LONG(std::lround(v.left * ratio)), LONG(std::lround(v.top * ratio)), LONG(std::lround(v.right * ratio)), LONG(std::lround(v.bottom * ratio))};
    int x = wr.left + v.left - st->vis.left, y = wr.top + v.top - st->vis.top;
    st->scale = s;
    st->w = int(std::lround(st->bw * s));
    st->h = int(std::lround(st->bh * s));
    RECT wa = WorkAreaOf(h);   // grown past the screen edge: slide it back in
    if (x + st->vis.right > wa.right) x = wa.right - st->vis.right;
    if (y + st->vis.bottom > wa.bottom) y = wa.bottom - st->vis.bottom;
    x = std::max(x, int(wa.left - st->vis.left));
    y = std::max(y, int(wa.top - st->vis.top));
    PlaceWidget(h, x, y, st->w, st->h, 0);
    auto it = g_widgets.find(st->id);
    if (it != g_widgets.end()) { try { it->second->call_function("__backdropZoom", s); } catch (...) {} }
    if (save) {
        RegSetDword(WidgetKey(st->id), L"Scale", DWORD(std::lround(s * 1000)));
        SavePos(h, st->id);
        if (it != g_widgets.end()) {   // shapes and the click-through outline at the new size: reload the page once
            if (std::fabs(s - 1.0) > 0.001 || true) LoadWidgetPage(h, st->entry, s);
            PlaceWidget(h, x, y, st->w, st->h, SWP_FRAMECHANGED);
        }
    }
}

// a plain Windows menu (follows dark/light mode). Owned by the hidden IPC window so the widget itself is never
// activated (activation is what made widgets flash a frame when right-clicked repeatedly).
static void WidgetMenu(HWND h, WidgetState* st) {
    enum { ARRANGE = 1, REMEMBER, RELOAD, FOLDER, APP, CLOSE, RESET, SNAP_TL = 20, SNAP_TR, SNAP_BL, SNAP_BR, SNAP_C, SIZE0 = 40 };
    static const int sizes[] = {50, 75, 100, 125, 150, 200, 300};
    HMENU size = CreatePopupMenu();
    for (int i = 0; i < 7; i++) {
        wchar_t t[32];
        swprintf_s(t, sizes[i] == 100 ? L"%d%% (original)" : L"%d%%", sizes[i]);
        AppendMenuW(size, MF_STRING | (std::abs(st->scale * 100 - sizes[i]) < 0.5 ? MF_CHECKED : 0), SIZE0 + i, t);
    }
    HMENU snap = CreatePopupMenu();
    AppendMenuW(snap, MF_STRING, SNAP_TL, L"Top left");
    AppendMenuW(snap, MF_STRING, SNAP_TR, L"Top right");
    AppendMenuW(snap, MF_STRING, SNAP_BL, L"Bottom left");
    AppendMenuW(snap, MF_STRING, SNAP_BR, L"Bottom right");
    AppendMenuW(snap, MF_STRING, SNAP_C, L"Centre");
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (g_arrange ? MF_CHECKED : 0), ARRANGE, g_arrange ? L"Done arranging" : L"Arrange widgets");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)snap, L"Snap to");
    AppendMenuW(m, MF_STRING, REMEMBER, L"Remember position");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)size, L"Size");
    AppendMenuW(m, MF_STRING | (std::fabs(st->scale - 1.0) > 0.001 ? 0 : MF_GRAYED), RESET, L"Reset to original size");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, RELOAD, L"Reload");
    AppendMenuW(m, MF_STRING, FOLDER, L"Open widget folder");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, APP, L"Backdrop settings…");
    AppendMenuW(m, MF_STRING, CLOSE, L"Close widget");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_msg);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, g_msg, nullptr);
    PostMessageW(g_msg, WM_NULL, 0, 0);
    DestroyMenu(m);
    if (cmd >= SNAP_TL && cmd <= SNAP_C) {   // the monitor's work area (taskbar excluded), one grid cell in from the edge
        RECT wa = WorkAreaOf(h), r, v = VisOf(h);   // by what the widget draws, not its window
        GetWindowRect(h, &r);
        int w = v.right - v.left, hh = v.bottom - v.top, g = GridPx(h);
        int x = (cmd == SNAP_TL || cmd == SNAP_BL) ? wa.left + g : cmd == SNAP_C ? (wa.left + wa.right - w) / 2 : wa.right - w - g;
        int y = (cmd == SNAP_TL || cmd == SNAP_TR) ? wa.top + g : cmd == SNAP_C ? (wa.top + wa.bottom - hh) / 2 : wa.bottom - hh - g;
        PlaceWidget(h, x - (v.left - r.left), y - (v.top - r.top), 0, 0, SWP_NOSIZE);
        SavePos(h, st->id);
    }
    else if (cmd == ARRANGE) { if (g_arrange) PostMessageW(g_msg, WM_ARRANGE_END, 0, 0); else BeginArrange(); }
    else if (cmd == REMEMBER) SavePos(h, st->id);
    else if (cmd == RESET) ApplyScale(h, st, 1.0, true);
    else if (cmd >= SIZE0 && cmd < SIZE0 + 7) ApplyScale(h, st, sizes[cmd - SIZE0] / 100.0, true);
    else if (cmd == RELOAD) {
        RECT r;
        GetWindowRect(h, &r);
        LoadWidgetPage(h, st->entry, st->scale);   // Sciter resets the window on load: put it back exactly where it was
        PlaceWidget(h, r.left, r.top, st->w, st->h, SWP_FRAMECHANGED);
        if (st->move) SetMove(st, true);
    }
    else if (cmd == FOLDER) {
        std::wstring dir = st->entry.substr(0, st->entry.find_last_of(L'\\'));
        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    else if (cmd == APP) ShowApp();
    else if (cmd == CLOSE) { RegSetDword(WidgetKey(st->id), L"Enabled", 0); PostMessageW(g_msg, WM_APP + 2, 0, 0); }
}

// Widgets sit ON the desktop: per-pixel transparent tool windows (no taskbar button, not in Alt+Tab / Task View) OWNED by
// the window that holds the desktop icons (Progman on 24H2+'s raised desktop, the icon WorkerW before that). Windows keeps
// an owned window directly above its owner, and we always ask for the bottom of the z-order, so a widget lands just
// above the wallpaper and icons and below every app - it can never cover anything. Deliberately no "on top" option; the
// only exception is "Arrange widgets", where the user has the whole screen dimmed and the widgets lifted to arrange them.
// (Sciter only does per-pixel alpha for top-level windows, so they can't be real children of the desktop.)
static LRESULT CALLBACK WidgetSubclass(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR ref) {
    auto* st = (WidgetState*)ref;
    switch (m) {
    case WM_WINDOWPOSCHANGING: {
        auto* wp = (WINDOWPOS*)l;
        if (!(wp->flags & SWP_NOZORDER)) wp->hwndInsertAfter = g_arrange ? HWND_TOPMOST : HWND_BOTTOM;
        wp->flags |= SWP_NOOWNERZORDER;   // never drag the owner (the desktop window) along: that reads as "show desktop"
        if (!(wp->flags & SWP_NOSIZE) && st->w > 0 && (wp->cx != st->w || wp->cy != st->h)) {   // only we size widgets
            wp->cx = st->w;
            wp->cy = st->h;
        }
        break;
    }
    case WM_MOUSEACTIVATE:   // clicks never activate a widget (no frame flash, no z-order shuffle) unless it wants keys
        return st->keyboard ? MA_ACTIVATE : MA_NOACTIVATE;
    case WM_NCACTIVATE:      // never paint an active / inactive frame
        return TRUE;
    case WM_NCPAINT:
        return 0;
    case WM_NCHITTEST:       // move / arrange: the whole widget is a drag handle, its bottom-right corner resizes
        if (st->move && g_arrange) {
            POINT pt{short(LOWORD(l)), short(HIWORD(l))};
            RECT v = VisOf(h);
            int hs = MulDiv(18, int(GetDpiForWindow(h)), 96);
            if (pt.x >= v.right - hs && pt.x <= v.right + hs / 2 && pt.y >= v.bottom - hs && pt.y <= v.bottom + hs / 2) return HTBOTTOMRIGHT;
        }
        if (st->move) { LRESULT r = DefSubclassProc(h, m, w, l); return r == HTNOWHERE || r == HTTRANSPARENT ? r : HTCAPTION; }
        break;
    case WM_NCLBUTTONDBLCLK: // no caption double-click (it would try to maximise)
        if (st->move) return 0;
        break;
    case WM_SETCURSOR:
        if (st->move && LOWORD(l) == HTBOTTOMRIGHT) { SetCursor(LoadCursorW(nullptr, IDC_SIZENWSE)); return TRUE; }
        if (st->move && LOWORD(l) == HTCAPTION) { SetCursor(LoadCursorW(nullptr, IDC_SIZEALL)); return TRUE; }
        break;
    case WM_NCLBUTTONDOWN:   // arranging: our own drag loop (exact cursor tracking, smart guides, grid)
        if (g_arrange && w == HTCAPTION) { DragBegin(h, false); return 0; }
        if (g_arrange && w == HTBOTTOMRIGHT) { DragBegin(h, true); return 0; }
        break;
    case WM_MOUSEMOVE:
        if (DraggingWidget() == h) { DragMove(); return 0; }
        break;
    case WM_LBUTTONUP:
        if (DraggingWidget() == h) { DragEnd(); return 0; }
        break;
    case WM_CAPTURECHANGED:
        if (DraggingWidget() == h && (HWND)l != h) DragEnd();
        break;
    case WM_NCRBUTTONDOWN:
        if (st->move) return 0;
        break;
    case WM_NCRBUTTONUP:     // right-click while movable (on the "caption" Windows would show the system menu)
        if (st->move) { WidgetMenu(h, st); return 0; }
        break;
    case WM_CONTEXTMENU:
        WidgetMenu(h, st);
        return 0;
    case WM_SYSCOMMAND:      // no Alt+Space menu, no maximise / minimise / resize on a widget
        switch (w & 0xFFF0) { case SC_KEYMENU: case SC_MAXIMIZE: case SC_MINIMIZE: case SC_RESTORE: case SC_SIZE: return 0; }
        break;
    case WM_WIDGET_MENU:
        WidgetMenu(h, st);
        return 0;
    case WM_WINDOWPOSCHANGED: // keep the outlines on the overlay under the widgets (a drag repaints by itself)
        if (g_arrange && DraggingWidget() != h && !(((WINDOWPOS*)l)->flags & SWP_NOMOVE)) RepaintOverlays(false);
        break;
    case WM_EXITSIZEMOVE:    // remember where the user put it
        SavePos(h, st->id);
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(h, WidgetSubclass, 0);
        delete st;
        break;
    }
    return DefSubclassProc(h, m, w, l);
}

static WidgetState* StateOf(HWND h) {
    DWORD_PTR ref = 0;
    return GetWindowSubclass(h, WidgetSubclass, 0, &ref) ? (WidgetState*)ref : nullptr;
}

static HWND DesktopHost() {   // the window whose child is SHELLDLL_DefView
    HWND pm = FindWindowW(L"Progman", nullptr);
    if (pm && FindWindowExW(pm, nullptr, L"SHELLDLL_DefView", nullptr)) return pm;
    HWND found = nullptr;
    EnumWindows([](HWND w, LPARAM p) -> BOOL {
        if (FindWindowExW(w, nullptr, L"SHELLDLL_DefView", nullptr)) { *(HWND*)p = w; return FALSE; }
        return TRUE;
    }, (LPARAM)&found);
    return found;
}

// borderless popup: Sciter creates a captioned, resizable frame (and re-applies it on every load); a caption is what
// flashed through when widgets were clicked or moved
static void WidgetFrame(HWND h) {
    LONG_PTR s = GetWindowLongPtrW(h, GWL_STYLE);
    LONG_PTR want = (s & ~LONG_PTR(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) | WS_POPUP;
    if (want != s) SetWindowLongPtrW(h, GWL_STYLE, want);
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    LONG_PTR wantEx = (ex | WS_EX_TOOLWINDOW) & ~LONG_PTR(WS_EX_APPWINDOW | WS_EX_WINDOWEDGE | WS_EX_DLGMODALFRAME);
    if (wantEx != ex) SetWindowLongPtrW(h, GWL_EXSTYLE, wantEx);
}

// place a widget at screen position x,y (physical px)
static void PlaceWidget(HWND h, int x, int y, int W, int H, UINT flags) {
    WidgetFrame(h);
    SetWindowPos(h, g_arrange ? HWND_TOPMOST : HWND_BOTTOM, x, y, W, H, flags | SWP_NOACTIVATE);
}

static void OpenWidget(const WidgetDef& d) {
    HWND host = DesktopHost();
    if (!host) return;   // Explorer isn't up; TaskbarCreated brings us back
    sciter::om::hasset<WidgetWin> w = new WidgetWin(d);
    HWND h = w->get_hwnd();
    if (!h) return;
    SetWindowLongPtrW(h, GWLP_HWNDPARENT, (LONG_PTR)host);   // owned by the desktop: always directly above it
    auto* st = new WidgetState();
    st->id = d.id;
    st->entry = d.entry;
    // final size and position before the page loads: no jump, nothing recalculated at sign-in
    int k = int(GetDpiForWindow(h));
    WidgetMeta meta = ReadMeta(d.entry);
    st->fps = meta.fps;
    st->keyboard = meta.keyboard;
    st->bw = MulDiv(meta.size.cx, k, 96);
    st->bh = MulDiv(meta.size.cy, k, 96);
    st->scale = std::max(0.25, std::min(4.0, RegDword(WidgetKey(d.id).c_str(), L"Scale", 1000) / 1000.0));   // remembered size
    st->w = int(std::lround(st->bw * st->scale));
    st->h = int(std::lround(st->bh * st->scale));
    SetWindowSubclass(h, WidgetSubclass, 0, (DWORD_PTR)st);
    g_widgets[d.id] = w;
    int x = d.x, y = d.y;
    bool fresh = x == INT_MIN || y == INT_MIN;
    if (fresh) {   // first time: stacked down the right edge of the work area, then remembered there
        RECT wa;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        int g = MulDiv(GridDip(), k, 96);
        x = wa.right - st->w - g;
        y = wa.top + g;
        for (auto& kv : g_widgets) {
            RECT r;
            if (kv.first != d.id && GetWindowRect(kv.second->get_hwnd(), &r) && r.right > x && r.bottom > y) y = r.bottom + g;
        }
    }
    w->bind();   // attach the native interface (load() would do this; we load the page bytes ourselves)
    LoadWidgetPage(h, d.entry, st->scale);
    PlaceWidget(h, x, y, st->w, st->h, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    if (fresh) SavePos(h, d.id);
    if (g_arrange) SetMove(st, true);
}

static void CloseWidget(const std::wstring& id) {
    auto it = g_widgets.find(id);
    if (it == g_widgets.end()) return;
    it->second->close();
    g_widgets.erase(it);
}

// make the open widget windows match the settings
static void SyncWidgets() {
    for (auto it = g_widgets.begin(); it != g_widgets.end();)
        it = IsWindow(it->second->get_hwnd()) ? std::next(it) : g_widgets.erase(it);
    bool on = RegDword(KEY, L"WidgetsEnabled", 0) != 0;
    std::vector<WidgetDef> defs = LoadWidgets();
    std::vector<std::wstring> keep;
    for (auto& d : defs)
        if (on && d.enabled && Exists(d.entry)) keep.push_back(d.id);
    std::vector<std::wstring> open;
    for (auto& kv : g_widgets) open.push_back(kv.first);
    for (auto& id : open)
        if (std::find(keep.begin(), keep.end(), id) == keep.end()) CloseWidget(id);
    for (auto& d : defs)
        if (std::find(keep.begin(), keep.end(), d.id) != keep.end() && !g_widgets.count(d.id)) OpenWidget(d);
    int fps = 24;
    for (auto& kv : g_widgets)
        if (WidgetState* st = StateOf(kv.second->get_hwnd())) fps = std::max(fps, st->fps);
    g_frameMs = DWORD(1000 / fps);
    if (g_arrange && g_widgets.empty()) EndArrange();
}

// ------------------------------------------------------------------ Arrange widgets: dimmed grid, outlines, smart guides
// One overlay per monitor work area (the taskbar stays clear) drawn with UpdateLayeredWindow: the dim and the grid are
// rendered once into a cached buffer; on top of it every widget gets the same outline around what it actually draws
// (its visible edges, not its window), the dragged one in the accent colour, plus PowerPoint-style smart guides while
// dragging: dashed lines when edges / centres line up with another widget or the screen centre, and arrows with the gap
// when spacing matches. Widgets sit above the overlay (topmost while arranging) and are dragged by our own loop (exact
// cursor tracking, so slow drags never stick). Guides win over the grid; Alt held = no snapping at all.
// A Sciter command bar at the bottom: Grid / Free move, Guides, Auto grid, grid size, Done. Esc / Done ends it.
class ArrangePanel;
struct Overlay { HWND hwnd = nullptr; RECT r{}; HDC dc = nullptr; HBITMAP bmp = nullptr; UINT32* px = nullptr; std::vector<UINT32> base; };
struct Guide { POINT a, b; bool spacing; int label; };   // screen px; spacing = double arrow with the gap in DIP
static std::vector<Overlay> g_overlays;
static std::vector<Guide> g_guides;
static sciter::om::hasset<ArrangePanel> g_panel;
static struct { HWND h = nullptr; POINT cur0{}; RECT win0{}, vis0{}; bool resize = false; double scale0 = 1; } g_drag;
// g_dragging: the widget being dragged right now (drawn highlighted)

static bool FreeMove() { return RegDword(KEY, L"ArrangeFree", 0) != 0; }
static bool GuidesOn() { return RegDword(KEY, L"ArrangeGuides", 1) != 0; }

static RECT VisOf(HWND h) {   // what the widget actually draws, in screen px (its window if it never said)
    RECT wr;
    GetWindowRect(h, &wr);
    WidgetState* st = StateOf(h);
    if (st && st->vis.right > st->vis.left && st->vis.bottom > st->vis.top)
        return RECT{wr.left + st->vis.left, wr.top + st->vis.top, wr.left + st->vis.right, wr.top + st->vis.bottom};
    return wr;
}

static int NearestLine(int v, int origin, int cell) { return origin + int(std::lround(double(v - origin) / cell)) * cell; }

static void BuildBase(Overlay& o) {   // dim + grid lines (none in free move), cached
    int W = o.r.right - o.r.left, H = o.r.bottom - o.r.top, cell = MulDiv(GridDip(), int(GetDpiForWindow(o.hwnd)), 96);
    bool lines = !FreeMove();
    o.base.assign(size_t(W) * H, 0);
    const UINT32 dim = 0x99000000, line = 0xB0383838, major = 0xC0545454;   // premultiplied BGRA
    for (int y = 0; y < H; y++) {
        bool hy = lines && y % cell == 0, my = lines && y % (cell * 4) == 0;
        UINT32* row = o.base.data() + size_t(y) * W;
        for (int x = 0; x < W; x++) {
            bool hx = lines && x % cell == 0, mx = lines && x % (cell * 4) == 0;
            row[x] = (my || mx) ? major : (hy || hx) ? line : dim;
        }
    }
}

static void Compose(Overlay& o) {   // cached base + outlines + guides, then onto the screen
    int W = o.r.right - o.r.left, H = o.r.bottom - o.r.top;
    if (!o.px || o.base.size() != size_t(W) * H) return;
    memcpy(o.px, o.base.data(), o.base.size() * sizeof(UINT32));
    int k = int(GetDpiForWindow(o.hwnd)), gap = MulDiv(4, k, 96), t = std::max(2, MulDiv(3, k, 192));
    COLORREF ac = ThemeAccent(true);
    const UINT32 accent = 0xFF000000 | GetRValue(ac) << 16 | GetGValue(ac) << 8 | GetBValue(ac);
    const UINT32 white = 0xE6E6E6E6, fill = 0xA8181818;
    auto put = [&](int x, int y, UINT32 c) { if (x >= 0 && y >= 0 && x < W && y < H) o.px[size_t(y) * W + x] = c; };
    auto box = [&](RECT r, UINT32 c, int th) {
        for (int i = 0; i < th; i++) {
            for (int x = r.left; x < r.right; x++) { put(x, r.top + i, c); put(x, r.bottom - 1 - i, c); }
            for (int y = r.top; y < r.bottom; y++) { put(r.left + i, y, c); put(r.right - 1 - i, y, c); }
        }
    };
    for (auto& kv : g_widgets) {   // outline + faint fill around each widget's visible edges
        HWND h = kv.second->get_hwnd();
        if (!IsWindowVisible(h)) continue;
        RECT v = VisOf(h);
        RECT r{v.left - o.r.left - gap, v.top - o.r.top - gap, v.right - o.r.left + gap, v.bottom - o.r.top + gap};
        if (r.right < 0 || r.bottom < 0 || r.left > W || r.top > H) continue;
        for (int y = std::max(0, int(r.top)); y < std::min(H, int(r.bottom)); y++)
            for (int x = std::max(0, int(r.left)); x < std::min(W, int(r.right)); x++) o.px[size_t(y) * W + x] = fill;
        box(r, h == g_dragging ? accent : white, t);
        int gl = MulDiv(14, k, 96), gt = std::max(3, MulDiv(4, k, 96));   // resize grip: an L at the bottom-right corner
        for (int i = 0; i < gl; i++)
            for (int j = 0; j < gt; j++) { put(r.right - 1 - i, r.bottom - 1 - j, h == g_dragging ? accent : white); put(r.right - 1 - j, r.bottom - 1 - i, h == g_dragging ? accent : white); }
    }
    int dash = MulDiv(6, k, 96), space = MulDiv(4, k, 96), lw = std::max(1, MulDiv(3, k, 192)), head = MulDiv(6, k, 96);
    for (auto& g : g_guides) {     // smart guides
        int x1 = g.a.x - o.r.left, y1 = g.a.y - o.r.top, x2 = g.b.x - o.r.left, y2 = g.b.y - o.r.top;
        bool vert = x1 == x2;
        int len = vert ? y2 - y1 : x2 - x1;
        for (int s = 0; s <= len; s++) {
            if (!g.spacing && s % (dash + space) >= dash) continue;   // alignment lines are dashed
            for (int i = 0; i < lw; i++) put(vert ? x1 + i - lw / 2 : x1 + s, vert ? y1 + s : y1 + i - lw / 2, accent);
        }
        if (!g.spacing) continue;
        for (int s = 0; s < head; s++)   // arrowheads at both ends
            for (int i = 0; i < lw; i++) {
                if (vert) { put(x1 - s, y1 + s + i, accent); put(x1 + s, y1 + s + i, accent); put(x1 - s, y2 - s - i, accent); put(x1 + s, y2 - s - i, accent); }
                else      { put(x1 + s + i, y1 - s, accent); put(x1 + s + i, y1 + s, accent); put(x2 - s - i, y1 - s, accent); put(x2 - s - i, y1 + s, accent); }
            }
        wchar_t txt[16];   // the gap, in DIP, on a small accent pill
        swprintf_s(txt, L"%d", g.label);
        static HFONT font;
        static int fontDpi;
        if (!font || fontDpi != k) {
            if (font) DeleteObject(font);
            font = CreateFontW(-MulDiv(12, k, 96), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI Variable Text");
            fontDpi = k;
        }
        HGDIOBJ of = SelectObject(o.dc, font);
        SIZE ts;
        GetTextExtentPoint32W(o.dc, txt, int(wcslen(txt)), &ts);
        int pw = ts.cx + MulDiv(12, k, 96), ph = ts.cy + MulDiv(2, k, 96), cx = (x1 + x2) / 2, cy = (y1 + y2) / 2;
        RECT pill{cx - pw / 2, cy - ph / 2, cx - pw / 2 + pw, cy - ph / 2 + ph};
        if (vert) OffsetRect(&pill, pw / 2 + MulDiv(6, k, 96), 0);
        else OffsetRect(&pill, 0, -(ph / 2 + MulDiv(6, k, 96)));
        for (int y = pill.top; y < pill.bottom; y++)
            for (int x = pill.left; x < pill.right; x++) put(x, y, accent);
        SetBkMode(o.dc, TRANSPARENT);
        SetTextColor(o.dc, RGB(0, 0, 0));
        GdiFlush();
        DrawTextW(o.dc, txt, -1, &pill, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        GdiFlush();
        for (int y = std::max(0, int(pill.top)); y < std::min(H, int(pill.bottom)); y++)   // GDI leaves alpha at 0
            for (int x = std::max(0, int(pill.left)); x < std::min(W, int(pill.right)); x++) o.px[size_t(y) * W + x] |= 0xFF000000;
        SelectObject(o.dc, of);
    }
    POINT src{0, 0}, pos{o.r.left, o.r.top};
    SIZE sz{W, H};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(o.hwnd, nullptr, &pos, &sz, o.dc, &src, 0, &bf, ULW_ALPHA);
}

static void RepaintOverlays(bool rebuild) {
    for (auto& o : g_overlays) { if (rebuild) BuildBase(o); Compose(o); }
}

static LRESULT CALLBACK GridProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;   // clicking the dimmed area must never bring it above the widgets
    if (m == WM_SETCURSOR) { SetCursor(LoadCursorW(nullptr, IDC_ARROW)); return TRUE; }
    return DefWindowProcW(h, m, w, l);
}

// ---- smart guides. axis 0 = x, 1 = y. Rects are visible edges in screen px.
static int Lo(const RECT& r, int a) { return a ? r.top : r.left; }
static int Hi(const RECT& r, int a) { return a ? r.bottom : r.right; }
static bool Overlap(const RECT& p, const RECT& q, int a) { return Lo(p, !a) < Hi(q, !a) && Hi(p, !a) > Lo(q, !a); }   // share a row / column

struct Gap { RECT a, b; int size; };
static std::vector<Gap> GapsAmong(const std::vector<RECT>& rs, int a) {   // gaps between neighbours in the same row / column
    std::vector<Gap> out;
    for (auto& p : rs)
        for (auto& q : rs) {
            if (&p == &q || !Overlap(p, q, a) || Hi(p, a) > Lo(q, a)) continue;
            bool between = false;
            for (auto& m : rs)
                if (&m != &p && &m != &q && Overlap(m, p, a) && Lo(m, a) >= Hi(p, a) && Hi(m, a) <= Lo(q, a)) between = true;
            if (!between) out.push_back({p, q, Lo(q, a) - Hi(p, a)});
        }
    return out;
}

static bool SolveAxis(int a, const RECT& v, const std::vector<RECT>& others, const RECT& wa, int T, int& delta) {
    int lo = Lo(v, a), hi = Hi(v, a), mid = (lo + hi) / 2, size = hi - lo, best = T + 1, bd = 0;
    auto take = [&](int d) { if (std::abs(d) < best) { best = std::abs(d); bd = d; } };
    auto lineUp = [&](int t) { take(t - lo); take(t - mid); take(t - hi); };
    for (auto& o : others) { lineUp(Lo(o, a)); lineUp((Lo(o, a) + Hi(o, a)) / 2); lineUp(Hi(o, a)); }
    take((Lo(wa, a) + Hi(wa, a)) / 2 - mid);   // screen centre
    const RECT* L = nullptr;   // nearest neighbours before / after in the same row / column
    const RECT* R = nullptr;
    for (auto& o : others) {
        if (!Overlap(o, v, a)) continue;
        if (Hi(o, a) <= lo + T && (!L || Hi(o, a) > Hi(*L, a))) L = &o;
        if (Lo(o, a) >= hi - T && (!R || Lo(o, a) < Lo(*R, a))) R = &o;
    }
    if (L && R) take((Hi(*L, a) + Lo(*R, a) - size) / 2 - lo);   // centred between them
    for (auto& g : GapsAmong(others, a)) {                       // same gap as elsewhere
        if (L) take(Hi(*L, a) + g.size - lo);
        if (R) take(Lo(*R, a) - g.size - size - lo);
    }
    if (best > T) return false;
    delta = bd;
    return true;
}

static std::vector<Guide> GuidesFor(const RECT& v, const std::vector<RECT>& others, const RECT& wa, int k) {
    std::vector<Guide> out;
    int pad = MulDiv(10, k, 96);
    for (int a = 0; a < 2; a++) {
        int anchors[3] = {Lo(v, a), (Lo(v, a) + Hi(v, a)) / 2, Hi(v, a)};
        std::vector<int> done;
        auto line = [&](int at, int from, int to) {   // a line perpendicular to the axis, at "at"
            for (int d : done) if (std::abs(d - at) <= 1) return;
            done.push_back(at);
            out.push_back(a == 0 ? Guide{{at, from}, {at, to}, false, 0} : Guide{{from, at}, {to, at}, false, 0});
        };
        for (auto& o : others) {
            int targets[3] = {Lo(o, a), (Lo(o, a) + Hi(o, a)) / 2, Hi(o, a)};
            for (int an : anchors)
                for (int tg : targets)
                    if (std::abs(an - tg) <= 1) line(tg, std::min(Lo(v, !a), Lo(o, !a)) - pad, std::max(Hi(v, !a), Hi(o, !a)) + pad);
        }
        int c = (Lo(wa, a) + Hi(wa, a)) / 2;
        if (std::abs(anchors[1] - c) <= 1) line(c, Lo(wa, !a), Hi(wa, !a));
        // equal spacing: the gaps next to the widget that match each other or a gap elsewhere get arrows
        std::vector<RECT> all = others;
        all.push_back(v);
        auto gaps = GapsAmong(all, a);
        std::vector<Gap> mine, rest;
        for (auto& g : gaps) (EqualRect(&g.a, &v) || EqualRect(&g.b, &v) ? mine : rest).push_back(g);
        std::vector<const Gap*> shown;
        for (auto& m : mine) {
            bool match = false;
            for (auto& m2 : mine) if (&m2 != &m && std::abs(m2.size - m.size) <= 1) match = true;
            for (auto& r : rest) if (std::abs(r.size - m.size) <= 1) { match = true; shown.push_back(&r); }
            if (match) shown.push_back(&m);
        }
        for (auto* g : shown) {
            if (g->size <= 0) continue;
            bool dup = false;
            for (auto& e : out) if (e.spacing && (a == 0 ? e.a.x == Hi(g->a, 0) && e.b.x == Lo(g->b, 0) : e.a.y == Hi(g->a, 1) && e.b.y == Lo(g->b, 1))) dup = true;
            if (dup) continue;
            int p = (std::max(Lo(g->a, !a), Lo(g->b, !a)) + std::min(Hi(g->a, !a), Hi(g->b, !a))) / 2;
            int label = MulDiv(g->size, 96, k);
            out.push_back(a == 0 ? Guide{{Hi(g->a, 0), p}, {Lo(g->b, 0), p}, true, label} : Guide{{p, Hi(g->a, 1)}, {p, Lo(g->b, 1)}, true, label});
        }
    }
    return out;
}

// where a dragged widget goes for a raw position (window top-left in screen px): smart guides first, else the grid
static void SmartPlace(HWND h, int& x, int& y) {
    RECT wr, v = VisOf(h);
    GetWindowRect(h, &wr);
    OffsetRect(&v, x - wr.left, y - wr.top);
    RECT wa = WorkAreaOf(h);
    int k = int(GetDpiForWindow(h)), T = MulDiv(8, k, 96), cell = GridPx(h), dx = 0, dy = 0;
    bool snap = GetKeyState(VK_MENU) >= 0;   // Alt: no snapping, like PowerPoint
    std::vector<RECT> others;
    for (auto& kv : g_widgets)
        if (kv.second->get_hwnd() != h && IsWindowVisible(kv.second->get_hwnd())) others.push_back(VisOf(kv.second->get_hwnd()));
    bool gx = false, gy = false;
    if (snap && GuidesOn()) { gx = SolveAxis(0, v, others, wa, T, dx); gy = SolveAxis(1, v, others, wa, T, dy); }
    if (snap && !FreeMove()) {
        if (!gx) dx = NearestLine(v.left, wa.left, cell) - v.left;
        if (!gy) dy = NearestLine(v.top, wa.top, cell) - v.top;
    }
    OffsetRect(&v, dx, dy);
    x += dx;
    y += dy;
    if (snap && GuidesOn()) g_guides = GuidesFor(v, others, wa, k); else g_guides.clear();
}

static HWND DraggingWidget() { return g_drag.h; }

static void DragBegin(HWND h, bool resize) {
    g_drag.h = h;
    g_drag.resize = resize;
    GetCursorPos(&g_drag.cur0);
    GetWindowRect(h, &g_drag.win0);
    g_drag.vis0 = VisOf(h);
    WidgetState* st = StateOf(h);
    g_drag.scale0 = st ? st->scale : 1;
    g_dragging = h;
    SetCapture(h);
    SetCursor(LoadCursorW(nullptr, resize ? IDC_SIZENWSE : IDC_SIZEALL));
    RepaintOverlays(false);
}

static void DragMove() {   // follows the cursor exactly from where it was grabbed, then snaps
    POINT p;
    GetCursorPos(&p);
    if (g_drag.resize) {   // proportional: the whole widget scales; in grid mode the right edge lands on a grid line
        WidgetState* st = StateOf(g_drag.h);
        if (!st) return;
        const RECT& v0 = g_drag.vis0;
        double vw = v0.right - v0.left, vh = v0.bottom - v0.top;
        double f = std::max((vw + p.x - g_drag.cur0.x) / vw, (vh + p.y - g_drag.cur0.y) / vh);
        if (!FreeMove() && GetKeyState(VK_MENU) >= 0) {
            RECT wa = WorkAreaOf(g_drag.h);
            int right = NearestLine(int(v0.left + vw * f), wa.left, GridPx(g_drag.h));
            if (right > v0.left) f = (right - v0.left) / vw;
        }
        double s = std::max(0.25, std::min(4.0, g_drag.scale0 * f));
        if (std::fabs(s - st->scale) > 0.0005) ApplyScale(g_drag.h, st, s, false);
        g_guides.clear();
        RepaintOverlays(false);
        return;
    }
    int x = g_drag.win0.left + p.x - g_drag.cur0.x, y = g_drag.win0.top + p.y - g_drag.cur0.y;
    SmartPlace(g_drag.h, x, y);
    RECT r;
    GetWindowRect(g_drag.h, &r);
    if (r.left != x || r.top != y) PlaceWidget(g_drag.h, x, y, 0, 0, SWP_NOSIZE);
    RepaintOverlays(false);
}

static void DragEnd() {
    HWND h = g_drag.h;
    if (!h) return;
    g_drag.h = nullptr;
    if (GetCapture() == h) ReleaseCapture();
    if (WidgetState* st = StateOf(h)) {
        if (g_drag.resize) ApplyScale(h, st, st->scale, true);   // remember the size; page re-laid out at it once
        SavePos(h, st->id);
    }
    g_dragging = nullptr;
    g_guides.clear();
    RepaintOverlays(false);
}

static void SnapAll() {   // every widget's visible top-left onto the grid
    for (auto& kv : g_widgets) {
        HWND h = kv.second->get_hwnd();
        RECT wr, v = VisOf(h), wa = WorkAreaOf(h);
        GetWindowRect(h, &wr);
        int g = GridPx(h);
        int dx = NearestLine(v.left, wa.left, g) - v.left, dy = NearestLine(v.top, wa.top, g) - v.top;
        if (v.right + dx > wa.right) dx -= ((v.right + dx - wa.right) + g - 1) / g * g;     // stay on screen
        if (v.bottom + dy > wa.bottom) dy -= ((v.bottom + dy - wa.bottom) + g - 1) / g * g;
        PlaceWidget(h, wr.left + dx, wr.top + dy, 0, 0, SWP_NOSIZE);
    }
}

// Auto grid: the cell size (16-96 DIP) that the widgets' visible widths and heights are closest to whole multiples of,
// so with their top-left on the grid their other edges land on grid lines too. Mid-sized cells win near-ties.
static int AutoGridDip() {
    std::vector<double> dims;
    for (auto& kv : g_widgets) {
        HWND h = kv.second->get_hwnd();
        RECT v = VisOf(h);
        double k = GetDpiForWindow(h) / 96.0;
        dims.push_back((v.right - v.left) / k);
        dims.push_back((v.bottom - v.top) / k);
    }
    if (dims.empty()) return GridDip();
    int best = GridDip();
    double bestScore = 1e9;
    for (int c = 16; c <= 96; c++) {
        double s = 0;
        for (double d : dims) { double m = d / c; s += std::fabs(m - std::max(1.0, std::round(m))); }
        s = s / dims.size() + std::fabs(c - 36) * 0.004;
        if (s < bestScore) { bestScore = s; best = c; }
    }
    return best;
}

class ArrangePanel : public sciter::window {
public:
    ArrangePanel() : sciter::window(0) {}
    int gridSize() { return GridDip(); }
    bool isFree() { return FreeMove(); }
    bool guides() { return GuidesOn(); }
    bool setGrid(int dip) {
        RegSetDword(KEY, L"GridSize", DWORD(std::max(8, std::min(128, dip))));
        if (!FreeMove()) SnapAll();
        RepaintOverlays(true);
        return true;
    }
    bool setFree(bool on) {
        RegSetDword(KEY, L"ArrangeFree", on ? 1 : 0);
        if (!on) SnapAll();
        RepaintOverlays(true);
        return true;
    }
    bool setGuides(bool on) { RegSetDword(KEY, L"ArrangeGuides", on ? 1 : 0); return true; }
    int autoGrid() {
        int dip = AutoGridDip();
        RegSetDword(KEY, L"ArrangeFree", 0);
        setGrid(dip);
        return dip;
    }
    bool done() { PostMessageW(g_msg, WM_ARRANGE_END, 0, 0); return true; }
    sciter::string accent() {
        COLORREF c = ThemeAccent(true);
        wchar_t b[16];
        swprintf_s(b, L"#%02x%02x%02x", GetRValue(c), GetGValue(c), GetBValue(c));
        return b;
    }
    SOM_PASSPORT_BEGIN_EX(arrange, ArrangePanel)
      SOM_FUNCS(SOM_FUNC(gridSize), SOM_FUNC(isFree), SOM_FUNC(guides), SOM_FUNC(setGrid), SOM_FUNC(setFree), SOM_FUNC(setGuides),
                SOM_FUNC(autoGrid), SOM_FUNC(done), SOM_FUNC(accent))
    SOM_PASSPORT_END
};

static void BeginArrange() {
    if (g_arrange) return;
    g_arrange = true;
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = GridProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"BackdropGrid";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        registered = true;
    }
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR mon, HDC, LPRECT, LPARAM) -> BOOL {
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(mon, &mi);
        Overlay o;
        o.r = mi.rcWork;
        int W = o.r.right - o.r.left, H = o.r.bottom - o.r.top;
        o.hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"BackdropGrid", L"", WS_POPUP,
                                 o.r.left, o.r.top, W, H, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!o.hwnd) return TRUE;
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB};
        o.dc = CreateCompatibleDC(nullptr);
        o.bmp = CreateDIBSection(o.dc, &bi, DIB_RGB_COLORS, (void**)&o.px, nullptr, 0);
        if (o.bmp) SelectObject(o.dc, o.bmp);
        g_overlays.push_back(o);
        return TRUE;
    }, 0);
    for (auto& kv : g_widgets) {   // lift every widget above the overlay; while arranging the whole rectangle drags
        HWND h = kv.second->get_hwnd();
        SetWindowLongPtrW(h, GWLP_HWNDPARENT, 0);   // not desktop-owned while topmost (Explorer treats those as the desktop)
        SetWindowRgn(h, nullptr, TRUE);
        SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        if (WidgetState* st = StateOf(h)) SetMove(st, true);
    }
    RepaintOverlays(true);
    for (auto& o : g_overlays) ShowWindow(o.hwnd, SW_SHOWNOACTIVATE);
    for (auto& kv : g_widgets)     // back above the overlays that were just shown
        SetWindowPos(kv.second->get_hwnd(), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    g_panel = new ArrangePanel();
    HWND p = g_panel->get_hwnd();
    SetWindowLongPtrW(p, GWL_EXSTYLE, (GetWindowLongPtrW(p, GWL_EXSTYLE) | WS_EX_TOOLWINDOW) & ~WS_EX_APPWINDOW);
    g_panel->load(FileUrl(g_uiDir + L"\\arrange.htm").c_str());
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int k = int(GetDpiForWindow(p)), W = MulDiv(880, k, 96), H = MulDiv(56, k, 96);
    SetWindowPos(p, HWND_TOPMOST, (wa.left + wa.right - W) / 2, wa.bottom - H - MulDiv(24, k, 96), W, H, SWP_SHOWWINDOW);
    SetForegroundWindow(p);   // Esc works straight away
}

static void EndArrange() {
    if (!g_arrange) return;
    DragEnd();
    g_arrange = false;
    g_dragging = nullptr;
    g_guides.clear();
    HWND host = DesktopHost();
    for (auto& kv : g_widgets) {
        HWND h = kv.second->get_hwnd();
        if (WidgetState* st = StateOf(h)) { SetMove(st, false); SavePos(h, st->id); }
        // order matters: drop topmost first, THEN hang it back on the desktop. Owning a still-topmost window to the
        // desktop pulls the desktop window up with it, which Windows treats as "show desktop" and minimises apps.
        SetWindowPos(h, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        SetWindowPos(h, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        SetWindowLongPtrW(h, GWLP_HWNDPARENT, (LONG_PTR)host);   // back on the desktop
        SetWindowPos(h, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        try { kv.second->call_function("backdropReshape"); } catch (...) {}   // click-through outline again
    }
    for (auto& o : g_overlays) {
        DestroyWindow(o.hwnd);
        if (o.bmp) DeleteObject(o.bmp);
        if (o.dc) DeleteDC(o.dc);
    }
    g_overlays.clear();
    if (g_panel) { g_panel->close(); g_panel = nullptr; }
}

// ------------------------------------------------------------------ backdrop.run(): local commands for widgets
// PowerShell by default (Windows PowerShell 5.1, always present), or "cmd", or "none" (run the line as a program).
// Runs hidden; { admin: true } goes through the normal UAC prompt (output is captured through a temp file then).
// Result to the callback as JSON: {"ok":bool,"code":n,"stdout":"","stderr":"","error":"","timedOut":bool}
struct RunJob {
    sciter::value cb;
    std::wstring cmd, shell, cwd;
    bool admin = false;
    DWORD timeout = 60000;
    bool ok = false, timedOut = false;
    DWORD code = 0;
    std::string out, err;
    std::wstring error;
};

static std::wstring JsonField(const std::wstring& j, const wchar_t* key) {   // tiny reader for our own options JSON
    std::wstring k = L"\"" + std::wstring(key) + L"\"";
    size_t a = j.find(k);
    if (a == std::wstring::npos) return L"";
    a = j.find(L':', a + k.size());
    if (a == std::wstring::npos) return L"";
    a = j.find_first_not_of(L" \t", a + 1);
    if (a == std::wstring::npos) return L"";
    if (j[a] != L'"') { size_t e = j.find_first_of(L",}", a); return j.substr(a, e == std::wstring::npos ? std::wstring::npos : e - a); }
    std::wstring v;
    for (size_t i = a + 1; i < j.size() && j[i] != L'"'; i++) {
        if (j[i] == L'\\' && i + 1 < j.size()) { i++; v += j[i] == L'n' ? L'\n' : j[i] == L't' ? L'\t' : j[i]; }
        else v += j[i];
    }
    return v;
}

static std::wstring Base64Utf16(const std::wstring& s) {   // for powershell -EncodedCommand (no quoting problems)
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto* b = (const unsigned char*)s.data();
    size_t n = s.size() * 2;
    std::wstring o;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = b[i] << 16 | (i + 1 < n ? b[i + 1] << 8 : 0) | (i + 2 < n ? b[i + 2] : 0);
        o += wchar_t(t[v >> 18 & 63]);
        o += wchar_t(t[v >> 12 & 63]);
        o += i + 1 < n ? wchar_t(t[v >> 6 & 63]) : L'=';
        o += i + 2 < n ? wchar_t(t[v & 63]) : L'=';
    }
    return o;
}

static std::wstring SysExe(const wchar_t* rel) {
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    return std::wstring(sys) + L"\\" + rel;
}

static std::wstring ToWide(const std::string& s, UINT cp) {
    if (s.empty()) return L"";
    if (cp == CP_UTF8 && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), nullptr, 0) == 0) cp = CP_ACP;
    int n = MultiByteToWideChar(cp, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L' ');
    MultiByteToWideChar(cp, 0, s.data(), int(s.size()), &w[0], n);
    if (!w.empty() && w[0] == 0xFEFF) w.erase(0, 1);
    return w;
}

static DWORD WINAPI RunThread(void* p) {
    auto* j = (RunJob*)p;
    std::wstring ps = SysExe(L"WindowsPowerShell\\v1.0\\powershell.exe"), cmdExe = SysExe(L"cmd.exe");
    const wchar_t* psArgs = L"-NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -EncodedCommand ";
    if (j->admin) {   // UAC: elevated processes can't share our pipes, so output goes through a temp file
        wchar_t tmp[MAX_PATH], file[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        GetTempFileNameW(tmp, L"bdr", 0, file);
        std::wstring exe, args;
        if (j->shell == L"cmd") { exe = cmdExe; args = L"/d /s /c \"(" + j->cmd + L") > \"" + file + L"\" 2>&1\""; }
        else if (j->shell == L"none") { exe = cmdExe; args = L"/d /s /c \"" + j->cmd + L" > \"" + file + L"\" 2>&1\""; }
        else {
            std::wstring script = L"$ProgressPreference='SilentlyContinue'; $global:LASTEXITCODE=0; try { & { " + j->cmd +
                L" } *>&1 | Out-File -Encoding utf8 -LiteralPath '" + file + L"'; if (-not $?) { exit 1 }; exit $LASTEXITCODE } catch { $_ | Out-File -Append -Encoding utf8 -LiteralPath '" +
                file + L"'; exit 1 }";
            exe = ps;
            args = psArgs + Base64Utf16(script);
        }
        SHELLEXECUTEINFOW se{sizeof(se)};
        se.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NO_CONSOLE | SEE_MASK_FLAG_NO_UI;
        se.lpVerb = L"runas";
        se.lpFile = exe.c_str();
        se.lpParameters = args.c_str();
        se.lpDirectory = j->cwd.empty() ? nullptr : j->cwd.c_str();
        se.nShow = SW_HIDE;
        if (!ShellExecuteExW(&se) || !se.hProcess) {
            j->error = GetLastError() == ERROR_CANCELLED ? L"cancelled" : L"couldn't start";
        } else {
            if (WaitForSingleObject(se.hProcess, j->timeout ? j->timeout : INFINITE) == WAIT_TIMEOUT) { TerminateProcess(se.hProcess, 1); j->timedOut = true; }
            GetExitCodeProcess(se.hProcess, &j->code);
            CloseHandle(se.hProcess);
            HANDLE f = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                char buf[65536];
                DWORD got;
                while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got) j->out.append(buf, got);
                CloseHandle(f);
            }
            j->ok = !j->timedOut;
        }
        DeleteFileW(file);
    } else {          // hidden child with stdout / stderr pipes
        std::wstring line, script;
        if (j->shell == L"cmd") line = L"\"" + cmdExe + L"\" /d /s /c \"" + j->cmd + L"\"";
        else if (j->shell == L"none") line = j->cmd;
        else {   // a temp .ps1 (UTF-8 with BOM): -File gives plain-text stderr (-EncodedCommand gives CLIXML) and any length
            wchar_t tmp[MAX_PATH], file[MAX_PATH];
            GetTempPathW(MAX_PATH, tmp);
            GetTempFileNameW(tmp, L"bdr", 0, file);
            DeleteFileW(file);
            script = std::wstring(file) + L".ps1";
            std::wstring body = L"[Console]::OutputEncoding=[Text.Encoding]::UTF8; $ProgressPreference='SilentlyContinue'\r\n" + j->cmd + L"\r\n";
            int n = WideCharToMultiByte(CP_UTF8, 0, body.c_str(), int(body.size()), nullptr, 0, nullptr, nullptr);
            std::string u8 = "\xEF\xBB\xBF" + std::string(size_t(n), ' ');
            WideCharToMultiByte(CP_UTF8, 0, body.c_str(), int(body.size()), &u8[3], n, nullptr, nullptr);
            HANDLE f = CreateFileW(script.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
            if (f != INVALID_HANDLE_VALUE) { DWORD wr; WriteFile(f, u8.data(), DWORD(u8.size()), &wr, nullptr); CloseHandle(f); }
            line = L"\"" + ps + L"\" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"" + script + L"\"";
        }
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE outR, outW, errR, errW;
        CreatePipe(&outR, &outW, &sa, 0);
        CreatePipe(&errR, &errW, &sa, 0);
        SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);
        HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
        STARTUPINFOW si{sizeof(si)};
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        si.hStdInput = nul;
        si.hStdOutput = outW;
        si.hStdError = errW;
        PROCESS_INFORMATION pi{};
        BOOL started = CreateProcessW(nullptr, &line[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                      j->cwd.empty() ? nullptr : j->cwd.c_str(), &si, &pi);
        CloseHandle(outW);
        CloseHandle(errW);
        CloseHandle(nul);
        if (!started) {
            j->error = L"couldn't start";
        } else {
            struct Reader { HANDLE h; std::string* s; };
            Reader re{errR, &j->err};
            HANDLE t = CreateThread(nullptr, 0, [](void* p) -> DWORD {   // stderr in parallel so neither pipe can fill up
                auto* r = (Reader*)p;
                char buf[16384];
                DWORD got;
                while (ReadFile(r->h, buf, sizeof(buf), &got, nullptr) && got) r->s->append(buf, got);
                return 0;
            }, &re, 0, nullptr);
            HANDLE killer = nullptr;   // the timeout: ends the process, which ends the reads
            struct Kill { HANDLE proc; DWORD ms; bool* hit; } kill{pi.hProcess, j->timeout, &j->timedOut};
            if (j->timeout) killer = CreateThread(nullptr, 0, [](void* p) -> DWORD {
                auto* k = (Kill*)p;
                if (WaitForSingleObject(k->proc, k->ms) == WAIT_TIMEOUT) { *k->hit = true; TerminateProcess(k->proc, 1); }
                return 0;
            }, &kill, 0, nullptr);
            char buf[16384];
            DWORD got;
            while (ReadFile(outR, buf, sizeof(buf), &got, nullptr) && got) j->out.append(buf, got);
            WaitForSingleObject(pi.hProcess, INFINITE);
            if (t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
            if (killer) { WaitForSingleObject(killer, INFINITE); CloseHandle(killer); }
            GetExitCodeProcess(pi.hProcess, &j->code);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            j->ok = !j->timedOut;
        }
        CloseHandle(outR);
        CloseHandle(errR);
        if (!script.empty()) DeleteFileW(script.c_str());
    }
    PostMessageW(g_msg, WM_RUN_DONE, 0, (LPARAM)j);   // the callback runs on the UI thread
    return 0;
}

static void RunCommand(HWND, const std::wstring& cmd, const std::wstring& opts, const sciter::value& cb) {
    auto* j = new RunJob();
    j->cb = cb;
    j->cmd = cmd;
    j->shell = JsonField(opts, L"shell");
    j->cwd = JsonField(opts, L"cwd");
    j->admin = JsonField(opts, L"admin") == L"true";
    std::wstring t = JsonField(opts, L"timeout");
    if (!t.empty()) j->timeout = DWORD(std::max(0.0, _wtof(t.c_str())));
    std::transform(j->shell.begin(), j->shell.end(), j->shell.begin(), ::towlower);
    HANDLE th = CreateThread(nullptr, 0, RunThread, j, 0, nullptr);
    if (th) CloseHandle(th); else { j->error = L"couldn't start"; PostMessageW(g_msg, WM_RUN_DONE, 0, (LPARAM)j); }
}

static void RunDone(RunJob* j) {
    UINT cp = j->shell == L"cmd" ? CP_OEMCP : CP_UTF8;
    std::wstring r = L"{\"ok\":" + std::wstring(j->ok ? L"true" : L"false") + L",\"code\":" + std::to_wstring(j->code) +
                     L",\"timedOut\":" + (j->timedOut ? L"true" : L"false") + L",\"error\":\"" + Esc(j->error) +
                     L"\",\"stdout\":\"" + Esc(ToWide(j->out, j->admin && j->shell.empty() ? CP_UTF8 : cp)) +
                     L"\",\"stderr\":\"" + Esc(ToWide(j->err, cp)) + L"\"}";
    try { if (j->cb.is_function() || j->cb.is_object_function()) j->cb.call(sciter::value(r)); } catch (...) {}
    delete j;
}

// ------------------------------------------------------------------ app window
class AppWin;
static sciter::om::hasset<AppWin> g_app;

class AppWin : public sciter::window {
public:
    AppWin() : sciter::window(SW_ENABLE_DEBUG * 0) {}

    sciter::string state() { return CoreState(); }
    bool send(sciter::string cmd) { return CoreSend(cmd); }
    bool startCore() {
        std::wstring exe = g_exeDir + L"\\backdrop.exe";
        return (INT_PTR)ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, nullptr, SW_SHOWNORMAL) > 32;
    }
    sciter::string widgets() {
        std::wstring s = L"{\"enabled\":";
        s += RegDword(KEY, L"WidgetsEnabled", 0) ? L"true" : L"false";
        s += L",\"atLogin\":";
        s += RegDword(KEY, L"WidgetsAtLogin", 0) ? L"true" : L"false";
        s += L",\"edit\":";
        s += g_arrange ? L"true" : L"false";
        s += L",\"items\":[";
        bool first = true;
        for (auto& d : LoadWidgets()) {
            if (!first) s += L",";
            first = false;
            s += L"{\"id\":\"" + Esc(d.id) + L"\",\"name\":\"" + Esc(d.name) + L"\",\"enabled\":" + (d.enabled ? L"true" : L"false") +
                 L",\"builtin\":" + (d.builtin ? L"true" : L"false") + L",\"missing\":" + (Exists(d.entry) ? L"false" : L"true") + L"}";
        }
        s += L"],\"gallery\":[";
        first = true;
        for (auto& g : Gallery()) {
            if (!first) s += L",";
            first = false;
            s += L"{\"id\":\"" + Esc(g.id) + L"\",\"name\":\"" + Esc(g.name) + L"\"}";
        }
        s += L"]}";
        return s;
    }
    bool widgetsEnable(bool on) {
        RegSetDword(KEY, L"WidgetsEnabled", on ? 1 : 0);
        SyncWidgets();
        return true;
    }
    bool widgetEnable(sciter::string id, bool on) {
        RegSetDword(std::wstring(WKEY) + L"\\" + id, L"Enabled", on ? 1 : 0);
        SyncWidgets();
        return true;
    }
    bool widgetAddBuiltin(sciter::string id) {
        for (auto& g : Gallery())
            if (g.id == id) {
                RegisterWidget(UniqueId(id), g.name, g.entry, true, g.id);
                if (!RegDword(KEY, L"WidgetsEnabled", 0)) widgetsEnable(true); else SyncWidgets();
                return true;
            }
        return false;
    }
    sciter::string widgetAddFile() {   // returns "" when cancelled, "!message" on error, else the new id
        wchar_t buf[MAX_PATH] = L"";
        OPENFILENAMEW of{sizeof(of)};
        of.hwndOwner = get_hwnd();
        of.lpstrFilter = L"Widgets (*.html, *.zip)\0*.html;*.htm;*.zip\0";
        of.lpstrFile = buf;
        of.nMaxFile = MAX_PATH;
        of.lpstrTitle = L"Add a widget";
        of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameW(&of)) return L"";
        std::wstring err;
        std::wstring id = InstallWidget(buf, err);
        if (id.empty()) return L"!" + err;
        if (!RegDword(KEY, L"WidgetsEnabled", 0)) widgetsEnable(true); else SyncWidgets();
        return id;
    }
    bool widgetRemove(sciter::string id) {
        CloseWidget(id);
        std::wstring key = std::wstring(WKEY) + L"\\" + id;
        bool builtin = RegDword(key.c_str(), L"Builtin", 0) != 0;
        RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
        if (!builtin) DeleteTree(g_userWidgets + L"\\" + id);
        return true;
    }
    bool widgetsArrange() { if (g_arrange) EndArrange(); else BeginArrange(); return true; }
    // the core starts backdrop-ui.exe at sign-in only when this is on (and widgets are on), after the wallpaper is up
    bool widgetsAtLogin(bool on) { RegSetDword(KEY, L"WidgetsAtLogin", on ? 1 : 0); return true; }
    bool widgetsFolder() {
        CreateDirectoryW(g_userWidgets.c_str(), nullptr);
        return (INT_PTR)ShellExecuteW(nullptr, L"open", g_userWidgets.c_str(), nullptr, nullptr, SW_SHOWNORMAL) > 32;
    }
    bool openLink(sciter::string url) {   // only our own links
        if (url != L"https://sciter.com/" && url.rfind(L"https://github.com/MilkmanAbi/Backdrop", 0) != 0) return false;
        return (INT_PTR)ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL) > 32;
    }
    sciter::string version() { return VERSION; }
    sciter::string theme() {   // {"dark":bool,"accent":"#rrggbb"} - Windows' app mode and accent colour
        bool dark = ThemeIsDark();
        COLORREF c = ThemeAccent(dark);
        wchar_t b[64];
        swprintf_s(b, L"{\"dark\":%s,\"accent\":\"#%02x%02x%02x\"}", dark ? L"true" : L"false", GetRValue(c), GetGValue(c), GetBValue(c));
        ThemeTitleBar(get_hwnd());
        return b;
    }
    sciter::string fileName(sciter::string path) {   // display name of the wallpaper file
        size_t s = path.find_last_of(L"\\/");
        return s == std::wstring::npos ? path : path.substr(s + 1);
    }

    SOM_PASSPORT_BEGIN_EX(assetInterface, AppWin)
      SOM_FUNCS(SOM_FUNC(state), SOM_FUNC(send), SOM_FUNC(startCore), SOM_FUNC(widgets), SOM_FUNC(widgetsEnable),
                SOM_FUNC(widgetEnable), SOM_FUNC(widgetAddBuiltin), SOM_FUNC(widgetAddFile), SOM_FUNC(widgetRemove),
                SOM_FUNC(widgetsArrange), SOM_FUNC(widgetsAtLogin), SOM_FUNC(widgetsFolder), SOM_FUNC(openLink), SOM_FUNC(version),
                SOM_FUNC(theme), SOM_FUNC(fileName))
    SOM_PASSPORT_END

protected:
    LRESULT on_engine_destroyed() override {
        LRESULT r = sciter::window::on_engine_destroyed();
        g_appOpen = false;
        PostMessageW(g_msg, WM_APP + 1, 0, 0);   // app closed: quit if nothing else is open
        return r;
    }
};

static void ShowApp() {
    if (g_app && g_app->get_hwnd() && IsWindow(g_app->get_hwnd())) {
        HWND h = g_app->get_hwnd();
        if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
        SetForegroundWindow(h);
        return;
    }
    g_app = new AppWin();
    g_appOpen = true;
    g_app->load(FileUrl(g_uiDir + L"\\app\\index.htm").c_str());
    HWND h = g_app->get_hwnd();
    ThemeTitleBar(h);
    HINSTANCE self = GetModuleHandleW(nullptr);
    SendMessageW(h, WM_SETICON, ICON_BIG, (LPARAM)LoadImageW(self, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0));
    SendMessageW(h, WM_SETICON, ICON_SMALL, (LPARAM)LoadImageW(self, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    g_app->expand();
}

static void MaybeQuit() {
    bool appOpen = g_app && g_app->get_hwnd() && IsWindow(g_app->get_hwnd());
    if (!appOpen) g_app = nullptr;
    if (!appOpen && g_widgets.empty()) sciter::application::request_quit(0);
}

// ------------------------------------------------------------------ IPC window
static LRESULT CALLBACK MsgProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_COPYDATA) {
        auto* cds = (COPYDATASTRUCT*)l;
        std::wstring s((const wchar_t*)cds->lpData, cds->cbData / sizeof(wchar_t));
        while (!s.empty() && s.back() == 0) s.pop_back();
        if (cds->dwData == IPC_STATE) { g_reply = s; return TRUE; }
        if (cds->dwData == IPC_UI) {   // from a second backdrop-ui.exe or from the core
            if (s == L"app") ShowApp();
            else if (s == L"arrange") BeginArrange();
            else if (s == L"sync") { SyncWidgets(); MaybeQuit(); }
            else if (s == L"quit") { for (auto& kv : g_widgets) kv.second->close(); g_widgets.clear(); if (g_app) g_app->close(); sciter::application::request_quit(0); }
            return TRUE;
        }
        return FALSE;
    }
    if (m == WM_APP + 1) { MaybeQuit(); return 0; }
    if (m == WM_APP + 2) { SyncWidgets(); MaybeQuit(); return 0; }
    if (m == WM_APP + 3) { ShowApp(); return 0; }
    if (m == WM_RUN_DONE) { RunDone((RunJob*)l); return 0; }
    if (m == WM_ARRANGE_END) { EndArrange(); return 0; }
    static const UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    if (m == taskbarCreated && m) { SetTimer(h, 1, 1500, nullptr); return 0; }   // Explorer restarted
    if (m == WM_TIMER && w == 1) {   // the old desktop window is gone: hang every widget on the new one (or wait for it)
        HWND host = DesktopHost();
        if (!host) return 0;   // timer keeps ticking until the desktop is back
        KillTimer(h, 1);
        for (auto& kv : g_widgets) {
            HWND wh = kv.second->get_hwnd();
            if (!IsWindow(wh)) continue;
            SetWindowLongPtrW(wh, GWLP_HWNDPARENT, (LONG_PTR)host);
            SetWindowPos(wh, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        SyncWidgets();
        return 0;
    }
    if (m == WM_CLOSE) { for (auto& kv : g_widgets) kv.second->close(); g_widgets.clear(); if (g_app) g_app->close(); sciter::application::request_quit(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}


// ---- widget frame-rate cap (24 fps unless a widget asks for more with <meta name="widget-fps">). Applies while only
// widgets are open; with the app window up everything runs at the normal rate.
// 1) Sciter's message loop wakes every 16 ms for its animation heartbeat: stretched to the frame interval (input and
//    messages still wake it at once).
static decltype(&MsgWaitForMultipleObjects) g_realMsgWait;

static DWORD WINAPI CappedMsgWait(DWORD n, const HANDLE* hs, BOOL all, DWORD ms, DWORD mask) {
    if (!g_appOpen && ms && ms != INFINITE && ms < g_frameMs) ms = g_frameMs;
    return g_realMsgWait(n, hs, all, ms, mask);
}

static void* PatchImport(HMODULE mod, const char* dll, const char* fn, void* hook) {
    auto base = (BYTE*)mod;
    auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; imp++) {
        if (_stricmp((char*)(base + imp->Name), dll)) continue;
        auto thunk = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk), name = (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
        for (; name->u1.AddressOfData; name++, thunk++) {
            if (IMAGE_SNAP_BY_ORDINAL(name->u1.Ordinal) || strcmp((char*)((IMAGE_IMPORT_BY_NAME*)(base + name->u1.AddressOfData))->Name, fn)) continue;
            DWORD old;
            VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
            void* prev = (void*)thunk->u1.Function;
            thunk->u1.Function = (ULONG_PTR)hook;
            VirtualProtect(&thunk->u1.Function, sizeof(void*), old, &old);
            return prev;
        }
    }
    return nullptr;
}


// 2) The GPU backend paces itself on vsync inside IDXGISwapChain::Present, so that is where the cap really lives:
// every swapchain in the process shares dxgi's vtable, so patching Present/Present1 once covers all widget windows.
// While only widgets are open, a present comes at most once per frame interval (the thread waits out the rest).
typedef HRESULT(STDMETHODCALLTYPE* PresentFn)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* Present1Fn)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
static PresentFn g_realPresent;
static Present1Fn g_realPresent1;

static void PaceFrame(void* sc) {   // each window (swapchain) keeps its own frame clock
    if (g_appOpen) return;
    static HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    static LARGE_INTEGER freq{};
    static std::map<void*, LONGLONG> lastOf;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LONGLONG& last = lastOf[sc];
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    LONGLONG frame = freq.QuadPart * g_frameMs / 1000, due = last + frame;
    if (last && now.QuadPart < due && timer) {
        LARGE_INTEGER rel;
        rel.QuadPart = -(LONGLONG)((due - now.QuadPart) * 10000000 / freq.QuadPart);   // 100 ns units, relative
        if (SetWaitableTimer(timer, &rel, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 1000);
        QueryPerformanceCounter(&now);
    }
    last = now.QuadPart;
}
static HRESULT STDMETHODCALLTYPE CappedPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (!(flags & DXGI_PRESENT_TEST)) PaceFrame(sc);
    return g_realPresent(sc, sync, flags);
}
static HRESULT STDMETHODCALLTYPE CappedPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp) {
    if (!(flags & DXGI_PRESENT_TEST)) PaceFrame(sc);
    return g_realPresent1(sc, sync, flags, pp);
}

static void PatchSlot(void** vt, int i, void* hook, void** real) {
    DWORD old;
    if (!VirtualProtect(&vt[i], sizeof(void*), PAGE_READWRITE, &old)) return;
    *real = vt[i];
    vt[i] = hook;
    VirtualProtect(&vt[i], sizeof(void*), old, &old);
}

static void HookPresent() {   // get dxgi's swapchain vtable from a throwaway composition swapchain
    ID3D11Device* dev = nullptr;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                 D3D11_SDK_VERSION, &dev, nullptr, nullptr))) return;
    IDXGIDevice* dd = nullptr;
    IDXGIAdapter* ad = nullptr;
    IDXGIFactory2* f = nullptr;
    IDXGISwapChain1* sc = nullptr;
    if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&dd))) && SUCCEEDED(dd->GetAdapter(&ad)) && SUCCEEDED(ad->GetParent(IID_PPV_ARGS(&f)))) {
        DXGI_SWAP_CHAIN_DESC1 d{};
        d.Width = d.Height = 8;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 2;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        d.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        if (SUCCEEDED(f->CreateSwapChainForComposition(dev, &d, nullptr, &sc))) {
            void** vt = *(void***)sc;
            PatchSlot(vt, 8, (void*)CappedPresent, (void**)&g_realPresent);     // IDXGISwapChain::Present
            PatchSlot(vt, 22, (void*)CappedPresent1, (void**)&g_realPresent1);  // IDXGISwapChain1::Present1
            sc->Release();
        }
    }
    if (f) f->Release();
    if (ad) ad->Release();
    if (dd) dd->Release();
    dev->Release();
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmd, int) {
    bool wantApp = wcsstr(cmd, L"/app") != nullptr, wantArrange = wcsstr(cmd, L"/arrange") != nullptr;   // /arrange: Arrange widgets
    HANDLE mtx = CreateMutexW(nullptr, TRUE, L"Backdrop.ui");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {   // already running: hand over the request
        if (HWND o = FindWindowW(L"BackdropUI", nullptr)) {
            std::wstring req = wantApp ? L"app" : wantArrange ? L"arrange" : L"sync";
            COPYDATASTRUCT cds{IPC_UI, DWORD((req.size() + 1) * sizeof(wchar_t)), (void*)req.c_str()};
            AllowSetForegroundWindow(ASFW_ANY);
            DWORD_PTR r;
            SendMessageTimeoutW(o, WM_COPYDATA, 0, (LPARAM)&cds, SMTO_ABORTIFHUNG, 3000, &r);
        }
        return 0;
    }
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    g_exeDir = p;
    g_exeDir = g_exeDir.substr(0, g_exeDir.find_last_of(L'\\'));
    g_uiDir = g_exeDir + L"\\ui";
    g_builtinWidgets = g_exeDir + L"\\widgets";
    wchar_t la[MAX_PATH];
    GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH);
    std::wstring data = std::wstring(la) + L"\\Backdrop";
    CreateDirectoryW(data.c_str(), nullptr);
    g_userWidgets = data + L"\\widgets";
    wchar_t sys[MAX_PATH];
    if (GetSystemDirectoryW(sys, MAX_PATH)) SetCurrentDirectoryW(sys);

    // sciter.dll strictly from our own folder
    HMODULE hm = LoadLibraryExW((g_exeDir + L"\\sciter.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!hm) { MessageBoxW(nullptr, L"sciter.dll is missing next to backdrop-ui.exe. Reinstall Backdrop.", L"Backdrop", MB_ICONERROR); return 1; }
    auto api = (SciterAPI_ptr)GetProcAddress(hm, "SciterAPI");
    if (!api) return 1;
    _SAPI(api());
    if (void* prev = PatchImport(hm, "USER32.dll", "MsgWaitForMultipleObjects", (void*)CappedMsgWait)) g_realMsgWait = (decltype(&MsgWaitForMultipleObjects))prev;
    HookPresent();

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    OleInitialize(nullptr);
    sciter::application::start();
    // widgets are full web pages: let them use the network, eval and Sciter's sys/env modules
    SciterSetOption(nullptr, SCITER_SET_SCRIPT_RUNTIME_FEATURES, ALLOW_FILE_IO | ALLOW_SOCKET_IO | ALLOW_EVAL | ALLOW_SYSINFO);
    SciterSetOption(nullptr, SCITER_SET_UX_THEMING, FALSE);
    // widget authors: set BACKDROP_LOG=1 and console.log / script errors go to %LOCALAPPDATA%\Backdrop\widgets.log
    if (GetEnvironmentVariableW(L"BACKDROP_LOG", nullptr, 0)) SciterSetOption(nullptr, SCITER_SET_DEBUG_MODE, TRUE);
    if (GetEnvironmentVariableW(L"BACKDROP_LOG", nullptr, 0))
        SciterSetupDebugOutput(nullptr, nullptr, [](LPVOID, UINT, UINT, LPCWSTR text, UINT n) {
            wchar_t la[MAX_PATH];
            GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH);
            HANDLE f = CreateFileW((std::wstring(la) + L"\\Backdrop\\widgets.log").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_ALWAYS, 0, nullptr);
            if (f == INVALID_HANDLE_VALUE) return;
            int len = WideCharToMultiByte(CP_UTF8, 0, text, int(n), nullptr, 0, nullptr, nullptr);
            std::string u(size_t(len), ' ');
            WideCharToMultiByte(CP_UTF8, 0, text, int(n), &u[0], len, nullptr, nullptr);
            DWORD w;
            WriteFile(f, u.data(), DWORD(u.size()), &w, nullptr);
            CloseHandle(f);
        });
    // Skia GPU: draws per-pixel transparent windows (Direct2D / plain OpenGL draw nothing) and caches images as
    // textures; the CPU raster backend re-scales every image on every frame (a GIF widget cost ~95% of a core)
    SciterSetOption(nullptr, SCITER_SET_GFX_LAYER, GFX_LAYER_SKIA_GPU);
    // every widget page gets the Backdrop right-click menu (unless the page handles "contextmenu" itself) and the
    // move-mode outline. The app window is left alone.
    static const char* initScript = R"JS((function () {
  if (!document.url().endsWith("#backdrop-widget")) return;      // not a widget (the app window)
  const native = () => Window.this.backdrop;                         // the widget's native functions (WidgetWin)
  // right-click: Backdrop's menu, unless the widget handled "contextmenu" itself
  document.on("contextmenu", () => { native().widgetMenu(); return true; });
  // clicks on the empty space around a widget's elements go through to the desktop: the window's region is set to the
  // boxes of the page's top-level elements, once when the page has loaded. A widget that changes its layout can
  // call backdropReshape(); one that wants its whole rectangle clickable: <meta name="widget-clickthrough" content="off">
  const reshape = () => {
    const m = document.$("meta[name='widget-clickthrough']");
    const body = document.body;
    if ((m && /^(off|no|false|0)$/i.test(m.attributes["content"] || "")) || !body) { native().setOutline(""); return; }
    const css = (el, p) => getComputedStyle(el).getPropertyValue(p) || "";
    const paints = (el) => {   // has its own background (Sciter reports colours as names or #rrggbbaa)
      const bg = css(el, "background-color"), img = css(el, "background-image");
      return (bg && bg !== "transparent" && !/^#[0-9a-f]{6}00$/i.test(bg)) || (img && img !== "url()" && img !== "none");
    };
    if (paints(document.documentElement) || paints(body)) { native().setOutline(""); return; }   // the page fills its window
    const k = 1;   // CSS px; Backdrop converts to device px (Sciter's devicePixelRatio reads 1)
    let spec = "";
    for (const el of body.children) {
      if (el.id === "__backdrop-move" || el.tagName === "SCRIPT" || el.tagName === "STYLE") continue;
      const r = el.getBoundingClientRect();
      if (r.width <= 0 || r.height <= 0) continue;
      // boxes only: a curved window region sends the window down a slow composition path (a GIF widget went from
      // ~10% to ~70% of a core), so shaped and rounded elements count as their bounding box
      spec += `R ${r.left * k} ${r.top * k} ${r.width * k} ${r.height * k}\n`;
    }
    native().setOutline(spec);
  };
  document.on("complete", reshape);
  globalThis.backdropReshape = reshape;
  // globalThis.backdrop: what widget authors use
  //   await backdrop.run("Get-Date")                       -> { ok, code, stdout, stderr, error, timedOut }
  //   await backdrop.run("ipconfig", { shell: "cmd" })     shell: "powershell" (default) | "cmd" | "none"
  //   await backdrop.run("Restart-Service Spooler", { admin: true })   UAC prompt, as usual
  //   options: timeout (ms, default 60000, 0 = none), cwd
  //   backdrop.close(), backdrop.menu(), backdrop.openApp()
  globalThis.backdrop = {
    run: (cmd, opts = {}) => new Promise((resolve) => native().run(String(cmd), JSON.stringify(opts || {}), (r) => resolve(JSON.parse(r)))),
    close: () => native().closeWidget(),
    menu: () => native().widgetMenu(),
    openApp: () => native().openApp(),
  };
  globalThis.__backdropZoom = (z) => { document.documentElement.style.setProperty("zoom", String(z)); };
  globalThis.__backdropMove = (on) => {   // a dashed frame on top while the widget can be dragged
    let f = document.$("#__backdrop-move");
    if (on && !f) {
      f = document.createElement("div");
      f.id = "__backdrop-move";
      f.style.cssText = "position:fixed; left:0; top:0; right:0; bottom:0; border:2px dashed rgba(255,255,255,0.85);" +
                        "border-radius:8px; background:rgba(110,130,255,0.14); z-index:2147483647; pointer-events:none;";
      document.body.append(f);
    } else if (!on && f) f.remove();
  };
})();)JS";
    SciterSetOption(nullptr, SCITER_SET_INIT_SCRIPT, (UINT_PTR)initScript);
    // widget pages start on a clear, non-scrolling canvas (their own CSS still wins); the app styles itself
    static const char masterCss[] = "html { background: transparent; overflow: hidden; } body { margin: 0; }";
    SciterAppendMasterCSS((LPCBYTE)masterCss, UINT(sizeof(masterCss) - 1));
    ThemeInit();   // dark/light Windows menus

    WNDCLASSW wc{};
    wc.lpfnWndProc = MsgProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"BackdropUI";
    RegisterClassW(&wc);
    g_msg = CreateWindowExW(0, L"BackdropUI", L"Backdrop UI", 0, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);

    if (!wantApp) {
        SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);   // widgets at sign-in: never compete
        for (int i = 0; i < 600 && !DesktopHost(); i++) Sleep(100);           // the desktop may not be up yet
    }
    SyncWidgets();
    if (wantApp) ShowApp();
    if (!wantApp && g_widgets.empty()) return 0;   // nothing to show
    int r = sciter::application::run();
    sciter::application::shutdown();
    ReleaseMutex(mtx);
    return r;
}
