# KoGaMaZ - launcher build (MinGW, same as official CI)
# Run: powershell -ExecutionPolicy Bypass -File build.ps1
$ErrorActionPreference = 'Stop'
$root  = 'C:\Users\serge\Videos\KoGaMaZ'
$tools = "$root\tools"
$src   = "$root\openkogama-launcher-main"
$build = "$root\build"
$dist  = "$root\dist"

$cmake = (Get-ChildItem "$tools\cmake" -Recurse -Filter cmake.exe | Select-Object -First 1).FullName
$ninja = (Get-ChildItem "$tools\ninja" -Recurse -Filter ninja.exe | Select-Object -First 1).FullName
$qtDir = (Get-ChildItem "$tools\Qt" -Directory -Filter 'mingw_64' -Recurse -Depth 2 | Select-Object -First 1).FullName
$gxx   = (Get-ChildItem "$tools\Qt" -Recurse -Filter 'g++.exe' | Select-Object -First 1).FullName
if (-not $cmake -or -not $ninja -or -not $qtDir -or -not $gxx) { throw "tools not found: cmake=$cmake ninja=$ninja qt=$qtDir gxx=$gxx" }
$gccDir   = Split-Path $gxx
$ninjaDir = Split-Path $ninja
$cmakeDir = Split-Path $cmake
$env:PATH = "$qtDir\bin;$gccDir;$ninjaDir;$cmakeDir;" + $env:PATH

Write-Host "Qt:    $qtDir"
Write-Host "g++:   $gxx"
Write-Host "ninja: $ninja"

& $cmake -S $src -B $build -G Ninja `
    "-DCMAKE_MAKE_PROGRAM=$ninja" `
    -DCMAKE_BUILD_TYPE=Release `
    "-DCMAKE_PREFIX_PATH=$qtDir" `
    "-DCMAKE_C_COMPILER=$(Join-Path $gccDir 'gcc.exe')" `
    "-DCMAKE_CXX_COMPILER=$gxx"
if ($LASTEXITCODE) { throw 'configure failed' }

& $cmake --build $build
if ($LASTEXITCODE) { throw 'build failed' }

& $cmake --install $build --prefix $dist
if ($LASTEXITCODE) { throw 'install failed' }

# remove the bare build-dir exe on purpose: it has NO Qt DLLs next to it,
# so double-clicking it gives "Qt6Gui.dll not found". Only dist\bin is runnable.
Remove-Item (Join-Path $build 'KoGaMaZ.exe') -Force -ErrorAction SilentlyContinue

$exe = Get-ChildItem $dist -Recurse -Filter 'KoGaMaZ.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
if ($exe) { Write-Host ("DONE: " + $exe.FullName) } else { Write-Host "DONE: KoGaMaZ.exe not found under $dist" }
