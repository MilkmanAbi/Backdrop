# Backdrop

A tiny Windows tray app that does two things well:

- **Video wallpaper**: any video, GIF or WebP plays behind your desktop icons, starts the moment you sign in, and pauses when you're not looking at the desktop.
- **Colour filters for the whole screen**: sepia, amber, greyscale, blue-light and more, each with a strength slider, a sign-in default and a daily schedule. Windows' own colour filters only offer a few, have no intensity setting, and are buried in Settings.

One small native exe (~450 KB). No admin rights, no background services, no network access, no telemetry.

<p align="center"><img src="docs/colour-filter.png" width="380" alt="Backdrop's colour filter window"></p>

## Install

Open **PowerShell** (Start menu, type `powershell`, press Enter) and paste:

```powershell
irm https://raw.githubusercontent.com/MilkmanAbi/Backdrop/main/install.ps1 | iex
```

That's it. Backdrop appears next to the clock and starts by itself every time you sign in.

- **Admin isn't needed.** A normal PowerShell window is fine, and so is "Run as administrator". Backdrop always installs and runs as *you*.
- **No scary warning.** If you download the installer in a browser, Windows shows "Windows protected your PC", because Backdrop isn't code-signed (that costs money every year). The command above doesn't hit that screen. Here's what it does, all of it visible in [`install.ps1`](install.ps1):
  1. Fetches the latest installer from this repo's [Releases](https://github.com/MilkmanAbi/Backdrop/releases).
  2. Checks its SHA-256 against the checksum GitHub recorded when it was uploaded, and refuses to run anything that doesn't match.
  3. Installs it silently to `%LOCALAPPDATA%\Programs\Backdrop`.
  4. Starts it.

**Prefer clicking?** Download `Backdrop-Setup-x.y.z.exe` from [Releases](https://github.com/MilkmanAbi/Backdrop/releases) and run it. On the blue "Windows protected your PC" screen, click **More info**, then **Run anyway**.

Requires 64-bit Windows 10 (1809) or Windows 11.

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
