@echo off
rem Build backdrop.exe (static CRT + static libwebp, no runtime dependencies), then backdrop-ui.exe, stage\ and the
rem installer if Inno Setup is present.
rem Everything except user32/kernel32/magnification is delay-loaded: a sign-in colour filter goes on before the rest loads.
rem   build.bat        release build
rem   build.bat log    build with logging to %TEMP%\backdrop.log
rem   build.bat exe    release backdrop.exe only (no installer)
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
set ROOT=%~dp0
set DEFS=/DUNICODE /D_UNICODE
if /i "%1"=="log" set DEFS=%DEFS% /DVW_LOG
rc /nologo backdrop.rc || exit /b 1
cl /nologo /utf-8 /O2 /GL /MT /EHsc /std:c++17 /W3 /Gy /Gw %DEFS% /Ithird_party\libwebp\include ^
   backdrop.cpp convert.cpp filter.cpp panel.cpp theme.cpp backdrop.res /Fe:backdrop.exe ^
   /link /SUBSYSTEM:WINDOWS /LTCG /OPT:REF /OPT:ICF /MANIFEST:NO /LIBPATH:third_party\libwebp\lib ^
   delayimp.lib /DELAYLOAD:mfplat.dll /DELAYLOAD:mfreadwrite.dll /DELAYLOAD:d3d11.dll /DELAYLOAD:d2d1.dll ^
   /DELAYLOAD:dwrite.dll /DELAYLOAD:windowscodecs.dll /DELAYLOAD:comdlg32.dll /DELAYLOAD:comctl32.dll /DELAYLOAD:wtsapi32.dll ^
   /DELAYLOAD:dwmapi.dll /DELAYLOAD:uxtheme.dll /DELAYLOAD:secur32.dll /DELAYLOAD:api-ms-win-shcore-scaling-l1-1-1.dll /DELAYLOAD:shell32.dll ^
   /DELAYLOAD:oleaut32.dll /DELAYLOAD:ole32.dll || exit /b 1
del /q *.obj backdrop.res 2>nul
if /i "%1"=="log" exit /b 0
if /i "%1"=="exe" exit /b 0
call "%ROOT%ui\build-ui.bat" || exit /b 1
cd /d "%ROOT%"
call "%ROOT%stage.bat" || exit /b 1
if exist "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" /Q setup.iss || exit /b 1
