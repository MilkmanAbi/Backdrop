@echo off
rem Assemble the install layout in stage\ (what the installer ships): backdrop.exe, backdrop-ui.exe, sciter.dll, ui\, widgets\
cd /d "%~dp0"
if not exist stage mkdir stage
copy /y backdrop.exe stage\ >nul
copy /y ui\backdrop-ui.exe stage\ >nul
copy /y third_party\sciter-js-sdk\bin\windows\x64\sciter.dll stage\ >nul
if not exist stage\ui mkdir stage\ui
copy /y ui\arrange.htm stage\ui\ >nul
if exist ui\app robocopy ui\app stage\ui\app /mir /njh /njs /nfl /ndl >nul
robocopy widgets stage\widgets /mir /njh /njs /nfl /ndl >nul
exit /b 0
