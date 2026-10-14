@echo off
rem Fetch and build libwebp 1.5.0 as static /MT libraries into third_party\libwebp (needed once before build.bat).
rem Needs Visual Studio 2022/2026 with C++, and CMake on PATH (Windows 10+ already has curl and tar).
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul || ^
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist third_party mkdir third_party
cd third_party
curl -L -o libwebp-1.5.0.zip https://github.com/webmproject/libwebp/archive/refs/tags/v1.5.0.zip || exit /b 1
tar -xf libwebp-1.5.0.zip || exit /b 1
del libwebp-1.5.0.zip
cmake -S libwebp-1.5.0 -B libwebp-build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DCMAKE_POLICY_DEFAULT_CMP0091=NEW -DBUILD_SHARED_LIBS=OFF ^
  -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF -DWEBP_BUILD_GIF2WEBP=OFF ^
  -DWEBP_BUILD_IMG2WEBP=OFF -DWEBP_BUILD_VWEBP=OFF -DWEBP_BUILD_WEBPINFO=OFF -DWEBP_BUILD_WEBPMUX=OFF ^
  -DWEBP_BUILD_EXTRAS=OFF -DWEBP_BUILD_LIBWEBPMUX=OFF -DCMAKE_INSTALL_PREFIX=%CD%\libwebp || exit /b 1
cmake --build libwebp-build && cmake --install libwebp-build || exit /b 1
rmdir /s /q libwebp-build
echo libwebp ready in third_party\libwebp
