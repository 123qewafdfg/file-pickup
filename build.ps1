# 藏笺 PC 构建脚本（PowerShell 版）
# 存在原因：Qt 的 qmlimportscanner 在本机（控制台代码页 936）处理含中文的路径时会乱码，
# 因此构建统一走 ASCII 联接 E:\pz\apk\cjpc -> E:\pz\apk\藏笺\pc。
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File build.ps1            # Debug
#   powershell -ExecutionPolicy Bypass -File build.ps1 -Config Release

param(
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Debug'
)

$ErrorActionPreference = 'Stop'

$qtRoot   = 'E:\pz\environments\qt\6.8.3\msvc2022_64'
$vcvars   = 'E:\pz\vs-buildtools\VC\Auxiliary\Build\vcvars64.bat'
$cmakeBin = 'E:\pz\apk\001\build-dependencies\Android\Sdk\cmake\4.1.2\bin'
$cmake    = Join-Path $cmakeBin 'cmake.exe'
$ninja    = Join-Path $cmakeBin 'ninja.exe'
$linkRoot = 'E:\pz\apk\cjpc'
$realRoot = 'E:\pz\apk\藏笺\pc'

if (-not (Test-Path -LiteralPath $qtRoot)) {
    throw "找不到 Qt：$qtRoot"
}

# 确保 ASCII 联接存在（首次运行或联接被删时自动重建）
if (-not (Test-Path -LiteralPath (Join-Path $linkRoot 'CMakeLists.txt'))) {
    cmd /c "mklink /J `"$linkRoot`" `"$realRoot`"" | Out-Null
}

# 载入 MSVC 环境（只影响当前进程）
$vcOut = cmd /c "call `"$vcvars`" >nul 2>&1 && set"
foreach ($line in $vcOut) {
    if ($line -match '^([A-Za-z_][A-Za-z0-9_]*)=(.*)$') {
        $name = $Matches[1]
        if ($name -eq 'PATH') { $env:Path = $Matches[2] }
        else { Set-Item -Path "env:$name" -Value $Matches[2] }
    }
}
if (-not (Get-Command cl -ErrorAction SilentlyContinue)) {
    throw 'MSVC 环境载入失败（找不到 cl.exe）'
}

$buildDir = Join-Path $linkRoot "build\$Config"

& $cmake -S $linkRoot -B $buildDir -G Ninja `
    "-DCMAKE_BUILD_TYPE=$Config" `
    "-DCMAKE_PREFIX_PATH=$qtRoot" `
    "-DCMAKE_MAKE_PROGRAM=$ninja"
if ($LASTEXITCODE -ne 0) { throw "CMake 配置失败（退出码 $LASTEXITCODE）" }

& $cmake --build $buildDir --parallel
if ($LASTEXITCODE -ne 0) { throw "构建失败（退出码 $LASTEXITCODE）" }

Write-Host ""
Write-Host "[完成] $buildDir" -ForegroundColor Green
Write-Host "[自测] powershell -File (Join-Path $buildDir 'cangjian_tests.exe')"
