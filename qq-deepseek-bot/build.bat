@echo off
setlocal
cd /d "%~dp0"

set "VCVARS="
if exist "D:\vs\VC\Auxiliary\Build\vcvarsall.bat" set "VCVARS=D:\vs\VC\Auxiliary\Build\vcvarsall.bat"
if not defined VCVARS if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
if not defined VCVARS if exist "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat"
if not defined VCVARS if exist "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat"

if not defined VCVARS (
  echo [ERROR] vcvarsall.bat not found. Install VS2022 C++ workload, or edit build.bat.
  exit /b 1
)

echo Using: %VCVARS%
call "%VCVARS%" x64 >nul
if errorlevel 1 ( echo [ERROR] vcvarsall failed & exit /b 1 )

cl /nologo /EHsc /std:c++17 /O2 /utf-8 /W3 /D_WIN32_WINNT=0x0A00 /DUNICODE /D_UNICODE /I third_party src\qq_deepseek_bot.cpp /Fe:qq_deepseek_bot.exe /link winhttp.lib ws2_32.lib
if errorlevel 1 ( echo [ERROR] compile failed & exit /b 1 )

echo [OK] build success: qq_deepseek_bot.exe
