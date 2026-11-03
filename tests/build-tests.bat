@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0.."
cl /nologo /utf-8 /O2 /MT /EHsc /std:c++17 /W3 /DUNICODE /D_UNICODE /Ithird_party\libwebp\include tests\media-smoke.cpp /Fe:tests\media-smoke.exe /link user32.lib /LIBPATH:third_party\libwebp\lib || exit /b 1
cl /nologo /utf-8 /O2 /MT /EHsc /std:c++17 /W3 /DUNICODE /D_UNICODE tests\filter-tests.cpp /Fe:tests\filter-tests.exe /link user32.lib || exit /b 1
cl /nologo /utf-8 /O2 /MT /EHsc /std:c++17 /W3 /DUNICODE /D_UNICODE tests\blur-tests.cpp filter.cpp /Fe:tests\blur-tests.exe /link user32.lib || exit /b 1
cl /nologo /utf-8 /O2 /Zi /MT /EHsc /std:c++17 /W3 /DUNICODE /D_UNICODE /Ithird_party\libwebp\include tests\desktop-smoke.cpp convert.cpp blur.cpp filter.cpp panel.cpp theme.cpp /Fe:tests\desktop-smoke.exe /link /DEBUG /MAP:tests\desktop-smoke.map /LIBPATH:third_party\libwebp\lib || exit /b 1
exit /b 0
