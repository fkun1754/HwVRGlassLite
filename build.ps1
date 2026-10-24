# 编译 HwVRGlassLite（MinGW-w64，静态链接）
$ErrorActionPreference = 'Stop'
$gpp = 'C:\mingw64\bin\g++.exe'
$windres = 'C:\mingw64\bin\windres.exe'
if (-not (Test-Path $gpp)) { throw "找不到编译器: $gpp" }

& $windres app.rc -O coff -o app_res.o
& $gpp -O2 -s -static -municode -mwindows -o HwVRGlassLite.exe main.cpp app_res.o `
    -I. -lsetupapi -lshell32 -lole32 -lcomctl32 -ladvapi32 `
    -static-libgcc -static-libstdc++
if ($LASTEXITCODE -ne 0) { throw "编译失败 exit=$LASTEXITCODE" }
Write-Host ("编译完成: HwVRGlassLite.exe  " + (Get-Item HwVRGlassLite.exe).Length + " 字节")
