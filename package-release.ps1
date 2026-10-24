# 藏笺 PC 正式发布脚本：生成免安装目录、ZIP 与单文件 EXE。
# 所有依赖只从 E:\pz 读取，产物只写入本项目 dist；不修改 PATH、注册表或系统配置。
param(
    [string]$CertificateThumbprint = '',
    [string]$TimestampUrl = 'http://timestamp.digicert.com'
)

$ErrorActionPreference = 'Stop'
$projectRoot = 'E:\pz\apk\藏笺\pc'
$linkRoot = 'E:\pz\apk\cjpc'
$qtRoot = 'E:\pz\environments\qt\6.8.3\msvc2022_64'
$pythonRoot = 'E:\pz\environments\python\3.11.9'
$ariaRoot = 'E:\pz\packages\aria2\1.37.0'
$ariaExe = Join-Path $ariaRoot 'win64\aria2-1.37.0-win-64bit-build1\aria2c.exe'
$ariaLicense = Join-Path $ariaRoot 'source\COPYING'
$vcvars = 'E:\pz\vs-buildtools\VC\Auxiliary\Build\vcvars64.bat'
$vcRuntimeRoot = 'E:\pz\vs-buildtools\VC\Redist\MSVC\14.44.35112\x64\Microsoft.VC143.CRT'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$releaseRoot = Join-Path $projectRoot "dist\release-$stamp"
$portableDir = Join-Path $releaseRoot 'portable'
$singleDir = Join-Path $releaseRoot 'single'
$launcherWork = Join-Path $releaseRoot 'launcher-build'

foreach ($required in @($qtRoot, $pythonRoot, $ariaExe, $ariaLicense, $vcvars, $vcRuntimeRoot)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "缺少发布依赖：$required" }
}

# 先构建 Release；构建脚本只在子进程内载入 MSVC 环境。
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $projectRoot 'build.ps1') -Config Release
if ($LASTEXITCODE -ne 0) { throw "Release 构建失败（退出码 $LASTEXITCODE）" }

New-Item -ItemType Directory -Path $portableDir, $singleDir, $launcherWork -Force | Out-Null
$releaseExe = Join-Path $linkRoot 'build\Release\cangjian.exe'
Copy-Item -LiteralPath $releaseExe -Destination (Join-Path $portableDir 'cangjian.exe')
Copy-Item -LiteralPath (Join-Path $linkRoot 'build\Release\libsodium.dll') `
    -Destination (Join-Path $portableDir 'libsodium.dll')

& (Join-Path $qtRoot 'bin\windeployqt.exe') --release --no-translations `
    --no-system-d3d-compiler --no-opengl-sw --qmldir (Join-Path $linkRoot 'qml') `
    (Join-Path $portableDir 'cangjian.exe')
if ($LASTEXITCODE -ne 0) { throw "windeployqt 失败（退出码 $LASTEXITCODE）" }

# windeployqt 在未继承 VCINSTALLDIR 时不会部署 MSVC 运行库；显式复制 x64 CRT，保证免安装包可启动。
Copy-Item -Path (Join-Path $vcRuntimeRoot '*.dll') -Destination $portableDir -Force

Copy-Item -LiteralPath $ariaExe -Destination (Join-Path $portableDir 'aria2c.exe')
New-Item -ItemType Directory -Path (Join-Path $portableDir 'licenses') -Force | Out-Null
Copy-Item -LiteralPath $ariaLicense -Destination (Join-Path $portableDir 'licenses\aria2-GPL-2.0.txt')

$scriptDir = Join-Path $portableDir 'scripts'
New-Item -ItemType Directory -Path $scriptDir -Force | Out-Null
foreach ($name in @('qq_channel.py', 'bili_dynamic.py', 'join_ui.py', 'cdp_capture.py')) {
    Copy-Item -LiteralPath (Join-Path $projectRoot "scripts\$name") -Destination (Join-Path $scriptDir $name)
}

# 小飞机直连脚本使用固定便携路径作为内嵌资源释放失败时的可靠回退。
$pythonScriptDir = Join-Path $portableDir 'runtime\python-scripts'
New-Item -ItemType Directory -Path $pythonScriptDir -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $projectRoot 'scripts\feejii.py') -Destination (Join-Path $pythonScriptDir 'feejii.py')

# 附带便携 Python，使发布包在没有系统 Python 的电脑上也能登录和发布。
$runtimePython = Join-Path $portableDir 'runtime\python'
New-Item -ItemType Directory -Path $runtimePython -Force | Out-Null
Copy-Item -Path (Join-Path $pythonRoot '*') -Destination $runtimePython -Recurse -Force
$segnoSource = 'E:\pz\packages\python\segno'
if (Test-Path -LiteralPath $segnoSource) {
    $sitePackages = Join-Path $runtimePython 'Lib\site-packages'
    New-Item -ItemType Directory -Path $sitePackages -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $segnoSource 'segno') -Destination $sitePackages -Recurse -Force
    Copy-Item -LiteralPath (Join-Path $segnoSource 'segno-1.6.6.dist-info') -Destination $sitePackages -Recurse -Force
}

function Invoke-Sign([string]$path) {
    if ([string]::IsNullOrWhiteSpace($CertificateThumbprint)) { return }
    $signtool = Get-ChildItem -LiteralPath 'E:\pz\vs-buildtools\SDK' -Filter signtool.exe -Recurse -File |
        Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $signtool) { throw '提供了证书指纹，但未找到 signtool.exe' }
    & $signtool.FullName sign /sha1 $CertificateThumbprint /fd SHA256 /tr $TimestampUrl /td SHA256 $path
    if ($LASTEXITCODE -ne 0) { throw "代码签名失败：$path" }
}

Invoke-Sign (Join-Path $portableDir 'cangjian.exe')

$portableZip = Join-Path $releaseRoot '藏笺-PC-免安装.zip'
Compress-Archive -Path (Join-Path $portableDir '*') -DestinationPath $portableZip -CompressionLevel Optimal

# 在发布目录的隔离工作区构建单文件启动器，不覆盖 tools\launcher 的历史产物。
Copy-Item -LiteralPath (Join-Path $projectRoot 'tools\launcher\main.cpp') -Destination $launcherWork
Copy-Item -LiteralPath (Join-Path $projectRoot 'tools\launcher\miniz.c') -Destination $launcherWork
Copy-Item -LiteralPath (Join-Path $projectRoot 'tools\launcher\miniz.h') -Destination $launcherWork
Copy-Item -LiteralPath $portableZip -Destination (Join-Path $launcherWork 'payload.zip')
[System.IO.File]::WriteAllText((Join-Path $launcherWork 'launcher.rc'),
    "#include <windows.h>`r`nPAYLOAD RCDATA `"payload.zip`"`r`n", [System.Text.Encoding]::ASCII)

$vcOut = cmd.exe /c "call `"$vcvars`" >nul 2>&1 && set"
foreach ($line in $vcOut) {
    if ($line -match '^([A-Za-z_][A-Za-z0-9_]*)=(.*)$') {
        if ($Matches[1] -eq 'PATH') { $env:Path = $Matches[2] }
        else { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
    }
}
Push-Location $launcherWork
try {
    & rc.exe /nologo /fo launcher.res launcher.rc
    if ($LASTEXITCODE -ne 0) { throw '资源编译失败' }
    & cl.exe /nologo /std:c++17 /EHsc /O2 /MT /utf-8 main.cpp miniz.c launcher.res `
        "/Fe:$(Join-Path $singleDir '藏笺.exe')" /link /SUBSYSTEM:WINDOWS user32.lib shell32.lib ole32.lib
    if ($LASTEXITCODE -ne 0) { throw '单文件启动器编译失败' }
} finally {
    Pop-Location
}
Invoke-Sign (Join-Path $singleDir '藏笺.exe')

$manifest = [ordered]@{
    createdAt = (Get-Date).ToString('o')
    signed = -not [string]::IsNullOrWhiteSpace($CertificateThumbprint)
    portable = $portableDir
    portableZip = $portableZip
    singleExe = (Join-Path $singleDir '藏笺.exe')
    includes = @('aria2c.exe', 'aria2 GPL-2.0 license', 'runtime Python', 'QQ/B站/小飞机直连/自动入频道脚本')
}
[System.IO.File]::WriteAllText((Join-Path $releaseRoot 'release-manifest.json'),
    ($manifest | ConvertTo-Json -Depth 4), [System.Text.Encoding]::UTF8)
Write-Host "[完成] $releaseRoot" -ForegroundColor Green
