@echo off
rem Build backdrop-ui.exe (the optional app + widgets process; loads sciter.dll from its own folder at run time).
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
rc /nologo /fo backdrop-ui.res backdrop-ui.rc || exit /b 1
cl /nologo /utf-8 /O1 /GL /MT /EHsc /std:c++17 /W3 /Gy /Gw /DUNICODE /D_UNICODE /DWINDOWS /I..\third_party\sciter-js-sdk\include ^
   backdrop-ui.cpp ..\theme.cpp backdrop-ui.res /Fe:backdrop-ui.exe /link /SUBSYSTEM:WINDOWS /LTCG /OPT:REF /OPT:ICF /MANIFEST:NO || exit /b 1
del /q *.obj backdrop-ui.res 2>nul
