@echo off
rem Cangjian PC build entry (batch file is ASCII-only on purpose: cmd codepage safety).
rem Usage: build.bat [Debug|Release]
setlocal
set "QT_ROOT=E:\pz\environments\qt\6.8.3\msvc2022_64"
set "VS_ROOT=E:\pz\vs-buildtools"
set "CMAKE_BIN=E:\pz\apk\001\build-dependencies\Android\Sdk\cmake\4.1.2\bin"
set "BUILD_TYPE=%~1"
if "%BUILD_TYPE%"=="" set "BUILD_TYPE=Debug"
set "SRC=%~dp0"
set "BUILD_DIR=%SRC%build\%BUILD_TYPE%"
if not exist "%QT_ROOT%\bin\qmake.exe" (
    echo [ERROR] Qt not found: %QT_ROOT%
    exit /b 1
)
call "%VS_ROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
    echo [ERROR] vcvars64.bat failed
    exit /b 1
)
"%CMAKE_BIN%\cmake.exe" -S "%SRC%." -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=%BUILD_TYPE% -DCMAKE_PREFIX_PATH="%QT_ROOT%" -DCMAKE_MAKE_PROGRAM="%CMAKE_BIN%\ninja.exe"
if errorlevel 1 exit /b 1
"%CMAKE_BIN%\cmake.exe" --build "%BUILD_DIR%" --parallel
if errorlevel 1 exit /b 1
echo.
echo [OK] build dir: %BUILD_DIR%
endlocal
