# Собирает сервер openkogama с правками KoGaMaZ.
#
# Берёт исходники ровно того коммита, что лежит в сборке (f0a72f51),
# кладёт поверх overlay/ и собирает. В dist/bin/server/ заменяются только
# openkogama-server.dll и OpenKogama.Api.dll - рантайм и пакеты те же.
#
#   powershell -File server-patches/build-server.ps1
param(
    [string]$Commit = 'f0a72f51a2cfcaa695fff2c242e5c83c33439761',
    [string]$Dotnet = (Join-Path $PSScriptRoot '..\tools\dotnet\dotnet.exe'),
    [string]$Server = (Join-Path $PSScriptRoot '..\dist\bin\server')
)
$ErrorActionPreference = 'Stop'

$work = Join-Path ([IO.Path]::GetTempPath()) "kogamaz-server-$Commit"
$src = Join-Path $work "openkogama-server-$Commit"
if (-not (Test-Path $src)) {
    New-Item -ItemType Directory -Force $work | Out-Null
    $zip = Join-Path $work 'src.zip'
    Invoke-WebRequest "https://github.com/openkogama/openkogama-server/archive/$Commit.zip" -OutFile $zip -UseBasicParsing
    Expand-Archive $zip $work -Force
}

Copy-Item (Join-Path $PSScriptRoot 'overlay\*') $src -Recurse -Force

# Соседнего репозитория openkogama-photon нет - берём его dll из сборки.
$photon = (Resolve-Path (Join-Path $Server 'OpenKogama.Photon.dll')).Path
$csproj = Join-Path $src 'openkogama-server.csproj'
$xml = Get-Content $csproj -Raw
$xml = $xml -replace '<ProjectReference Include="\.\.\\openkogama-photon\\openkogama-photon\.csproj" />',
    "<Reference Include=`"OpenKogama.Photon`"><HintPath>$photon</HintPath></Reference>"
Set-Content $csproj $xml -Encoding utf8

& $Dotnet build $csproj -c Release -nologo -v q
if ($LASTEXITCODE -ne 0) { throw "dotnet build failed ($LASTEXITCODE)" }

$out = Join-Path $src 'bin\Release\net10.0'
foreach ($dll in 'openkogama-server.dll', 'OpenKogama.Api.dll') {
    Copy-Item (Join-Path $out $dll) (Join-Path $Server $dll) -Force
    Write-Host "installed $dll"
}
