# Backdrop

<video src="https://github.com/MilkmanAbi/Backdrop/raw/main/assets/Demo.mp4" controls muted autoplay loop playsinline width="100%"></video>

<p align="center"><a href="assets/Demo.mp4">▶ Watch the demo</a></p>

A tiny Windows tray app that does two things well:

- **Video wallpaper**: any video, GIF or WebP plays behind your desktop icons, starts the moment you sign in, and pauses when you're not looking at the desktop.
- **Colour filters for the whole screen**: sepia, amber, greyscale, blue-light and more, each with a strength slider, a sign-in default and a daily schedule. Windows' own colour filters only offer a few, have no intensity setting, and are buried in Settings.

One small native exe (~450 KB). No admin rights, no background services, no network access, no telemetry. Made by Abinaash ([MilkmanAbi](https://github.com/MilkmanAbi)).

<p align="center"><img src="docs/colour-filter.png" width="380" alt="Backdrop's colour filter window"></p>

## Install

1. Download **`Backdrop-Setup-1.1.0.exe`** from [Releases](https://github.com/MilkmanAbi/Backdrop/releases/latest).
2. Run it. Windows will probably show a blue **"Windows protected your PC"** screen. Click **More info**, then **Run anyway**. That screen appears for any app that isn't code-signed, which costs money every year; it doesn't mean anything was found.
3. Backdrop appears next to the clock, asks you to pick a video or GIF, and from then on starts by itself every time you sign in.

No admin rights needed. It installs just for you, to `%LOCALAPPDATA%\Programs\Backdrop`. Requires 64-bit Windows 10 (1809) or Windows 11.

### If Windows says "Smart App Control blocked an app"

Most PCs never show this, because Smart App Control is usually already off. If yours blocks Backdrop:

1. Open Start, type **security** and open **Windows Security**.

   <img src="assets/1-search-windows-security.png" width="520" alt="Searching for Windows Security in the Start menu">

2. Go to **App & browser control**, then **Smart App Control settings**.

   <img src="assets/2-app-and-browser-control.png" width="620" alt="App & browser control page">

3. Choose **Off**, then run the installer again.

   <img src="assets/3-smart-app-control-off.png" width="620" alt="Smart App Control set to Off">

Turning Smart App Control off means Windows stops blocking *all* unknown apps, not just this one. On some Windows versions you also can't turn it back on without resetting Windows. Only do it if you're happy with that. Windows Defender antivirus keeps protecting you either way.

## Made by

**Abinaash ([MilkmanAbi](https://github.com/MilkmanAbi))**. I built Backdrop because Windows' colour filters were too limited and hard to toggle, and I wanted my favourite video as my wallpaper without a heavy app running in the background. Sharing it with my friends, and now you.

### Is it safe?

Yes. And you don't have to take my word for it:

- **All the code is right here**: about 2,600 lines of C++ you can read, and build yourself with `build.bat`.
- **It never touches the internet.** No accounts, no telemetry, no update pings.
- **No admin rights, no drivers, no services.** It runs as you, like any normal app, and only changes your own settings.
- **Clean uninstall.** Use the tray menu (**Uninstall Backdrop**) or Settings → Apps, and everything is removed: the app, its settings, its startup entry and its cache.
- The colour filter uses Windows' own built-in screen colour transform, the same one Windows' colour filters use. If Backdrop is ever closed, your screen goes straight back to normal.
## Using it

Click the tray icon:

| Menu item | What it does |
|---|---|
| **Colour filter: Sepia 70%** | Turns the filter on or off. The shortcut **Win + Alt + C** works anywhere. If another app already uses it, Backdrop picks a free one and shows it in the menu. |
| **Colour filters…** | Opens the filter window: pick a filter, drag the slider, and see it live on the whole screen. |
| **Video wallpaper on** | Turns the wallpaper on or off. When off, it unloads completely and uses about 9 MB of RAM. |
| **Choose video or GIF…** | MP4, MOV, AVI, MKV, WMV, GIF (including transparent ones) and WebP (including animated ones). |
| **Fit** | Fill (crop), Fit (black bars), Stretch, or Center (original size). |
| **Uninstall Backdrop…** | Removes everything. You can also uninstall from Settings → Apps. |

### Colour filters

Sepia, Amber, Blue light filter, Greyscale, Washed out, Dim, Night red, Inverted, and three colour-blind friendly filters: Red-green (green weak), Red-green (red weak) and Blue-yellow.

- **Intensity:** 0–100% for every filter, applied live with a smooth fade.
- **Turn on when I sign in:** the filter goes on within a fraction of a second of Backdrop starting at sign-in. Otherwise it starts off each time you sign in.
- **Schedule:** on and off at set times every day (it can run overnight, e.g. 9 pm to 7 am). Switching it by hand still works, and the next scheduled time takes over again. It keeps time correctly across sleep, clock changes and daylight saving.
- Remembers your filter and strength.

Filters use Windows' own full-screen colour transform, the same one Windows' built-in colour filters use. It works on everything, including games, videos and the taskbar, costs nothing, and switches off instantly if Backdrop is ever closed.

### Video wallpaper

- When you pick a file, Backdrop converts it once on the GPU into a video at your display's exact resolution. Playback then never has to resize anything, which keeps it light.
  - Phone videos are turned the right way up.
  - Short GIFs are looped smoothly.
  - Converted files are cached, so switching fit modes back and forth is instant.
- Warns you when a file's resolution is too low for your screen, and tells you the ideal size.
- Pauses when a maximised or full-screen window covers the desktop, when the PC is locked, and when the display is off.
- Plays muted with hardware decoding.

## Build from source

Needs Visual Studio 2022 or 2026 with the C++ workload, and CMake. [Inno Setup 6](https://jrsoftware.org/isinfo.php) is only needed for the installer.

```bat
get_libwebp.bat   :: once: fetches and builds libwebp (animated WebP support)
build.bat         :: backdrop.exe, plus dist\Backdrop-Setup-x.y.z.exe if Inno Setup is installed
```

| File | What's in it |
|---|---|
| `backdrop.cpp` | Tray app, wallpaper playback (Media Foundation → D3D11), desktop placement, install and uninstall |
| `convert.cpp` | One-time GPU conversion (decode → D3D11 video processor → hardware H.264) |
| `filter.cpp` | Colour matrices, fades, schedule |
| `panel.cpp` | The colour filter window (Direct2D) |
| `theme.cpp` | Follows Windows light/dark mode and your accent colour |

## Licence

MIT, see [LICENSE](LICENSE). Backdrop includes [libwebp](https://chromium.googlesource.com/webm/libwebp) (BSD) and the [Lucide](https://lucide.dev) "image" icon (ISC); see [THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt).
