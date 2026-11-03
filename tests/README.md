# Backdrop 1.5.0 validation

Run on Windows with the same Visual Studio C++ workload and libwebp as the app.
The media matrix also needs Python, FFmpeg and ffprobe on PATH.

```powershell
cmd /c tests\build-tests.bat
.\tests\filter-tests.exe
.\tests\blur-tests.exe
python tests/run-media-tests.py 'path\to\Rainy.mp4' 'path\to\test-output'
.\tests\desktop-smoke.exe 'path\to\Rainy.mp4' 'path\to\integrated.mp4'
```

The desktop test briefly attaches a video behind the desktop icons, renders frames,
then releases it, three times. It does not save settings, register startup entries,
change the system wallpaper or launch the settings panel. Run it on an unlocked desktop.
All test commands must exit 0; the tests are console executables so inspect their exit
status if invoking from a shell that continues after native command failures.

## Results, 3 November 2026

Host: Windows 11 26H2, build 26300.9818, x64. Visual Studio 2026 / MSVC 19.51.

- Rainy: H.264, 848x462, 30 fps, 673 frames, 22.433 seconds.
- Fill, Fit, Stretch and Center: all 673 frames retained, duration within 0.1 seconds;
  each MP4 independently decoded from start to finish with FFmpeg (`-xerror`).
- Forced software decoder + encoder: all 673 frames retained and independently decoded.
- Animated GIF and WebP: converted, H.264 output independently decoded.
- Immediate/in-flight cancellation, invalid output dimensions, corrupt and missing input:
  expected errors, destination preserved, no partial files left behind.
- Filters: all 11 filters produce identity at zero strength and finite full-strength matrices.
- Schedule: 5,760 checks covering every minute of daytime, overnight, equal-time and
  midnight-boundary windows; start inclusive, end exclusive, equal times disabled.
- Blur: 101 kernel widths checked for finite offsets and normalized weights;
  identity-filter colour correction remains identity.
- Integrated blur/converter executable: Rainy conversion succeeds with both modules linked.
- Media Engine / D3D11 / raised desktop: three attach, render and teardown cycles;
  38, 41 and 41 frames presented in 1.5 seconds per cycle; valid parent and released resources.
- Inno Setup 6 builds the per-user 1.5.0 installer containing the core and notices only.

## Regression addressed

1.4.0 introduced a second global `struct Gpu` in blur.cpp, differing from convert.cpp.
This violated the C++ One Definition Rule, including incompatible generated constructors
and destructors. The release compiler emitted C4789 (40-byte object, write at offset 864).
The types are now `BlurGpu` and `ConvertGpu`, so their symbols cannot collide.
The combined-module test prevents relying solely on an isolated converter test.

Desktop hosting now initializes Explorer's raised composition even if its WorkerW already
exists and repairs order only when needed. Reference: [Lively maintainer's Microsoft
developer correspondence](https://github.com/lively-community/lively/issues/2074).
[Windows release information](https://learn.microsoft.com/en-us/windows/release-health/windows11-release-information)
and [KB5127936](https://support.microsoft.com/help/5127936) were reviewed; the release notes
do not identify a new DWM breakage matching Rainy. No DRM change is required.

## Limits

No reboot, physical monitor hotplug, HDR/other-GPU matrix, Windows 10/older Windows 11,
or streaming protected-content playback was exercised. The installer was compiled;
installation/uninstallation was not run against the user's live profile/startup task.
AddressSanitizer could not link because this Visual Studio installation lacks its
runtime thunk library. Native execution and full output decoding were used instead.
