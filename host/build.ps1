# 一键构建上位机（Qt6 + MinGW）
# 用法： powershell -ExecutionPolicy Bypass -File host\build.ps1

$ErrorActionPreference = "Stop"

$QtDir     = "C:/Qt/6.8.3/mingw_64"
$MingwDir  = "C:/Qt/Tools/mingw1310_64"

if (-not (Test-Path "$QtDir/bin/qmake.exe")) {
    Write-Error "找不到 Qt: $QtDir  (先运行 aqt install-qt)"
}
if (-not (Test-Path "$MingwDir/bin/g++.exe")) {
    Write-Error "找不到 MinGW: $MingwDir  (先运行 aqt install-tool)"
}

$env:PATH = "$MingwDir/bin;$QtDir/bin;$env:PATH"

Push-Location $PSScriptRoot
try {
    Write-Host "== 配置 CMake ==" -ForegroundColor Cyan
    cmake -S . -B build -G "MinGW Makefiles" `
          -DCMAKE_PREFIX_PATH="$QtDir" `
          -DCMAKE_BUILD_TYPE=Release
    if ($LASTEXITCODE -ne 0) { throw "CMake 配置失败" }

    Write-Host "== 编译 ==" -ForegroundColor Cyan
    cmake --build build --parallel
    if ($LASTEXITCODE -ne 0) { throw "编译失败" }

    Write-Host "== 部署 Qt / MinGW 运行库（可直接双击 exe）==" -ForegroundColor Cyan
    & "$QtDir/bin/windeployqt.exe" --no-translations .\build\timing-host.exe | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "windeployqt 失败" }
    Copy-Item "$QtDir/plugins/platforms/qoffscreen.dll" .\build\platforms\ -ErrorAction SilentlyContinue

    Write-Host ""
    Write-Host "完成：" -ForegroundColor Green
    Write-Host "  GUI        build/timing-host.exe  （双击即可，已带运行库）"
    Write-Host "  联调工具   build/device-smoke.exe  (--port COM10)"
    Write-Host "  协议自检   build/protocol-selftest.exe"
    Write-Host ""
    Write-Host "临时命令行运行：`$env:PATH='$QtDir/bin;' + `$env:PATH; .\build\timing-host.exe"
}
finally {
    Pop-Location
}
