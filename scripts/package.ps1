# Package a portable Windows build.
#   powershell -ExecutionPolicy Bypass -File scripts\package.ps1
# Produces dist\ (runnable folder) and MediaPlayer-v<version>-win64.zip next to the repo root.
# Keep this file ASCII-only: Windows PowerShell 5.1 reads BOM-less scripts as ANSI.
param(
    [string]$Config = 'Release'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build'
$dist = Join-Path $root 'dist'

$versionLine = Select-String -Path (Join-Path $root 'Version.h') -Pattern 'MP_APP_VERSION\s+"([^"]+)"'
if (-not $versionLine) { throw 'MP_APP_VERSION not found in Version.h' }
$version = $versionLine.Matches[0].Groups[1].Value
Write-Host "packaging MediaPlayer $version ($Config)"

& cmake --build $build --config $Config -j 8 --target MediaPlayer
if ($LASTEXITCODE -ne 0) { throw "build failed: $LASTEXITCODE" }

if (Test-Path $dist) { Remove-Item $dist -Recurse -Force }
New-Item -ItemType Directory -Path $dist | Out-Null
Copy-Item (Join-Path $build "$Config\MediaPlayer.exe") $dist

# Qt runtime. dxcompiler.dll is optional; windeployqt only warns about it, and that
# warning must not abort the script under ErrorActionPreference=Stop.
$previous = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
& windeployqt --release --no-translations --no-system-d3d-compiler --no-opengl-sw (Join-Path $dist 'MediaPlayer.exe')
$ErrorActionPreference = $previous

Get-ChildItem (Join-Path $build $Config) -Filter '*.dll' | ForEach-Object { Copy-Item $_.FullName $dist }

Copy-Item (Join-Path $PSScriptRoot 'README-START.txt') $dist
Copy-Item (Join-Path $PSScriptRoot 'MediaPlayer.ini.example') $dist

$models = Join-Path $dist 'models'
New-Item -ItemType Directory -Path $models | Out-Null
Copy-Item (Join-Path $PSScriptRoot 'models-readme.txt') (Join-Path $models 'PUT-MODELS-HERE.txt')

$zip = Join-Path $root "MediaPlayer-v$version-win64.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $dist '*') -DestinationPath $zip

$sizeMb = [math]::Round((Get-Item $zip).Length / 1MB, 1)
$count = (Get-ChildItem $dist -Recurse -File).Count
Write-Host "dist: $count files"
Write-Host "zip:  $zip ($sizeMb MB)"
