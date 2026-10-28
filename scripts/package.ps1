# Package a portable Windows build.
#   powershell -ExecutionPolicy Bypass -File scripts\package.ps1          # CPU build (build\)
#   powershell -ExecutionPolicy Bypass -File scripts\package.ps1 -Gpu     # CUDA build (build-cuda\)
# Produces dist\ (runnable folder) and MediaPlayer-v<version>-win64[-cuda].zip next to the repo root.
# The CUDA variant needs build-cuda\ to exist, see docs/THIRD_PARTY_VERSIONS.md.
# Keep this file ASCII-only: Windows PowerShell 5.1 reads BOM-less scripts as ANSI.
param(
    [string]$Config = 'Release',
    [switch]$Gpu
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$buildName = if ($Gpu) { 'build-cuda' } else { 'build' }
$build = Join-Path $root $buildName
$dist = Join-Path $root 'dist'

if (-not (Test-Path $build)) { throw "build directory not found: $build (use -Gpu only after configuring build-cuda)" }

$versionLine = Select-String -Path (Join-Path $root 'Version.h') -Pattern 'MP_APP_VERSION\s+"([^"]+)"'
if (-not $versionLine) { throw 'MP_APP_VERSION not found in Version.h' }
$version = $versionLine.Matches[0].Groups[1].Value
$flavor = if ($Gpu) { 'cuda' } else { 'cpu' }
Write-Host "packaging MediaPlayer $version ($flavor / $Config)"

& cmake --build $build --config $Config -j 8 --target MediaPlayer
if ($LASTEXITCODE -ne 0) { throw "build failed: $LASTEXITCODE" }

if (Test-Path $dist) { Remove-Item $dist -Recurse -Force }
New-Item -ItemType Directory -Path $dist | Out-Null
Copy-Item (Join-Path $build "$Config\MediaPlayer.exe") $dist

# Qt runtime. dxcompiler.dll is optional; windeployqt only warns about it, and that
# warning must not abort the script under ErrorActionPreference=Stop.
# windeployqt is usually not on PATH: derive it from Qt6_DIR recorded in the CMake cache.
$windeployqt = 'windeployqt'
$cache = Join-Path $build 'CMakeCache.txt'
if (Test-Path $cache) {
    $m = Select-String -Path $cache -Pattern '^Qt6_DIR:[^=]*=(.+)$' | Select-Object -First 1
    if ($m) {
        $qtLib = $m.Matches[0].Groups[1].Value.Trim()
        $qtPrefix = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $qtLib))
        $candidate = Join-Path $qtPrefix 'bin\windeployqt.exe'
        if (Test-Path $candidate) { $windeployqt = $candidate }
    }
}
if (-not (Test-Path $windeployqt) -and -not (Get-Command $windeployqt -ErrorAction SilentlyContinue)) {
    throw "windeployqt not found (looked next to Qt6_DIR from $cache)"
}

$previous = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
& $windeployqt --release --no-translations --no-system-d3d-compiler --no-opengl-sw (Join-Path $dist 'MediaPlayer.exe')
$ErrorActionPreference = $previous

# windeployqt failing silently would produce a package that cannot start; make it loud.
if (-not (Test-Path (Join-Path $dist 'Qt6Core.dll'))) {
    throw "windeployqt did not copy Qt6Core.dll into $dist"
}

Get-ChildItem (Join-Path $build $Config) -Filter '*.dll' | ForEach-Object { Copy-Item $_.FullName $dist }

Copy-Item (Join-Path $PSScriptRoot 'README-START.txt') $dist
Copy-Item (Join-Path $PSScriptRoot 'MediaPlayer.ini.example') $dist

$models = Join-Path $dist 'models'
New-Item -ItemType Directory -Path $models | Out-Null
Copy-Item (Join-Path $PSScriptRoot 'models-readme.txt') (Join-Path $models 'PUT-MODELS-HERE.txt')

# A CUDA package that silently fell back to CPU is worse than no package.
if ($Gpu) {
    foreach ($required in @('ggml-cuda.dll', 'cudart64_12.dll', 'cublas64_12.dll')) {
        if (-not (Test-Path (Join-Path $dist $required))) {
            throw "CUDA build is missing $required in $dist"
        }
    }
}

$suffix = if ($Gpu) { '-cuda' } else { '' }
$zip = Join-Path $root "MediaPlayer-v$version-win64$suffix.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $dist '*') -DestinationPath $zip

$sizeMb = [math]::Round((Get-Item $zip).Length / 1MB, 1)
$count = (Get-ChildItem $dist -Recurse -File).Count
Write-Host "dist: $count files"
Write-Host "zip:  $zip ($sizeMb MB)"
