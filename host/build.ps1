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

    Write-Host ""
    Write-Host "完成：" -ForegroundColor Green
    Write-Host "  GUI        build/timing-host.exe"
    Write-Host "  联调工具   build/device-smoke.exe  (--port COM10)"
    Write-Host "  协议自检   build/protocol-selftest.exe"
    Write-Host ""
    Write-Host "运行前需要把 Qt DLL 加入 PATH，或直接双击 build 目录里的 exe（同目录需有 Qt6*.dll）。"
    Write-Host "临时运行：`$env:PATH='$QtDir/bin;' + `$env:PATH; .\build\timing-host.exe"
}
finally {
    Pop-Location
}
