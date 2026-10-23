@echo off
rem Build backdrop.exe (static CRT + static libwebp, no runtime dependencies), then backdrop-ui.exe, stage\ and the
rem installer if Inno Setup is present.
rem Everything except user32/kernel32/magnification is delay-loaded: a sign-in colour filter goes on before the rest loads.
rem   build.bat        release build
rem   build.bat log    build with logging to %TEMP%\backdrop.log
rem   build.bat exe    release backdrop.exe only (no installer)
rem   build.bat blurtest  logging build whose blur overlay shows up in screenshots and freezes after one frame
rem   build.bat blurdump  logging build that writes the overlay to %TEMP%\blurdump_*.raw when %TEMP%\blurdump.req exists
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
set ROOT=%~dp0
set DEFS=/DUNICODE /D_UNICODE
if /i "%1"=="log" set DEFS=%DEFS% /DVW_LOG
if /i "%1"=="blurtest" set DEFS=%DEFS% /DVW_LOG /DVW_BLURTEST
if /i "%1"=="blurdump" set DEFS=%DEFS% /DVW_LOG /DVW_BLURDUMP
rc /nologo backdrop.rc || exit /b 1
rem blur.hlsl -> blur_vs.h / blur_psh.h / blur_psv.h / blur_psd.h (precompiled, so no shader compiler is needed at run time)
fxc /nologo /T vs_4_0 /E VS /Vn g_vs /Fh blur_vs.h blur.hlsl >nul || exit /b 1
fxc /nologo /T ps_4_0 /E PSH /Vn g_psh /Fh blur_psh.h blur.hlsl >nul || exit /b 1
fxc /nologo /T ps_4_0 /E PSV /Vn g_psv /Fh blur_psv.h blur.hlsl >nul || exit /b 1
fxc /nologo /T ps_4_0 /E PSD /Vn g_psd /Fh blur_psd.h blur.hlsl >nul || exit /b 1
cl /nologo /utf-8 /O2 /GL /MT /EHsc /std:c++17 /W3 /Gy /Gw %DEFS% /Ithird_party\libwebp\include ^
   backdrop.cpp blur.cpp convert.cpp filter.cpp panel.cpp theme.cpp backdrop.res /Fe:backdrop.exe ^
   /link /SUBSYSTEM:WINDOWS /LTCG /OPT:REF /OPT:ICF /MANIFEST:NO /LIBPATH:third_party\libwebp\lib ^
   delayimp.lib /DELAYLOAD:mfplat.dll /DELAYLOAD:mfreadwrite.dll /DELAYLOAD:d3d11.dll /DELAYLOAD:dxgi.dll /DELAYLOAD:dcomp.dll /DELAYLOAD:d2d1.dll ^
   /DELAYLOAD:dwrite.dll /DELAYLOAD:windowscodecs.dll /DELAYLOAD:comdlg32.dll /DELAYLOAD:comctl32.dll /DELAYLOAD:wtsapi32.dll ^
   /DELAYLOAD:dwmapi.dll /DELAYLOAD:uxtheme.dll /DELAYLOAD:secur32.dll /DELAYLOAD:api-ms-win-shcore-scaling-l1-1-1.dll /DELAYLOAD:shell32.dll ^
   /DELAYLOAD:oleaut32.dll /DELAYLOAD:ole32.dll || exit /b 1
del /q *.obj backdrop.res 2>nul
if /i "%1"=="log" exit /b 0
if /i "%1"=="blurtest" exit /b 0
if /i "%1"=="blurdump" exit /b 0
if /i "%1"=="exe" exit /b 0
call "%ROOT%ui\build-ui.bat" || exit /b 1
cd /d "%ROOT%"
call "%ROOT%stage.bat" || exit /b 1
if exist "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" /Q setup.iss || exit /b 1
