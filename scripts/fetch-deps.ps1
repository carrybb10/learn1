# Fetch third-party sources, prebuilt libs and models. Nothing here is committed to git.
# Usage: powershell -ExecutionPolicy Bypass -File scripts/fetch-deps.ps1 [-SkipModels]
#
# Keep this file ASCII-only: Windows PowerShell 5.1 reads BOM-less scripts as ANSI,
# so non-ASCII comments break the parser on a fresh machine / CI runner.
param([switch]$SkipModels)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"   # Invoke-WebRequest progress bar is very slow on PS 5.1
try { [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 } catch {}

# CI runners hit transient network errors; a single failed git fetch used to kill the
# whole job (v0.5.3 run). Retry the network steps instead of failing the build.
function Invoke-WithRetry([scriptblock]$Action, [string]$What, [int]$Attempts = 3) {
    for ($i = 1; $i -le $Attempts; $i++) {
        try { & $Action; return }
        catch {
            if ($i -eq $Attempts) { throw "$What failed after $Attempts attempts: $($_.Exception.Message)" }
            Write-Warning "$What attempt $i failed: $($_.Exception.Message); retrying in 5s"
            Start-Sleep -Seconds 5
        }
    }
}

function Get-File([string]$Url, [string]$Out) {
    Invoke-WithRetry { Invoke-WebRequest $Url -OutFile $Out -UseBasicParsing } "download $Url"
}

$root = Split-Path -Parent $PSScriptRoot
$tp = Join-Path $root "third_party"
New-Item -ItemType Directory -Force $tp | Out-Null
$tmp = Join-Path $env:TEMP ("mp-deps-" + [guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Force $tmp | Out-Null

# Keep in sync with docs/THIRD_PARTY_VERSIONS.md
$llamaCommit   = "4ebdf2c74acce30883d8e34b7c70b3eb8146f2fe"
$whisperCommit = "60c0be6ac8fa71b1a2ae2dd938a31a34a508e774"
$ffmpegUrl     = "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-lgpl-shared.zip"
$sdlUrl        = "https://github.com/libsdl-org/SDL/releases/download/release-2.32.4/SDL2-devel-2.32.4-VC.zip"

function Ensure-Repo($name, $url, $commit) {
    $dir = Join-Path $tp $name
    if (-not (Test-Path (Join-Path $dir ".git"))) {
        Write-Host "clone $name ..."
        Invoke-WithRetry {
            if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
            git clone --filter=blob:none $url $dir
            if ($LASTEXITCODE -ne 0) { throw "git clone failed: $url" }
        } "clone $name"
    }
    Invoke-WithRetry {
        git -C $dir fetch --depth 1 origin $commit
        if ($LASTEXITCODE -ne 0) { throw "git fetch failed: $name @ $commit" }
    } "fetch $name"
    git -C $dir checkout --quiet $commit
    if ($LASTEXITCODE -ne 0) { throw "git checkout failed: $name @ $commit" }
    Write-Host "$name @ $commit ok"
}

Ensure-Repo "llama.cpp"   "https://github.com/ggml-org/llama.cpp.git"   $llamaCommit
Ensure-Repo "whisper.cpp" "https://github.com/ggml-org/whisper.cpp.git" $whisperCommit

# Flatten an extracted archive: find the first directory that contains $marker and
# copy its children into $dest. Archive layouts differ between upstream releases,
# so detect instead of hardcoding one path.
function Copy-Flattened($searchRoot, $marker, $dest) {
    $inner = Get-ChildItem -Path $searchRoot -Directory -Recurse -Depth 3 |
             Where-Object { Test-Path (Join-Path $_.FullName $marker) } |
             Sort-Object { $_.FullName.Length } | Select-Object -First 1
    if (-not $inner) { throw "cannot find '$marker' under $searchRoot" }
    New-Item -ItemType Directory -Force $dest | Out-Null
    Copy-Item (Join-Path $inner.FullName "*") $dest -Recurse -Force
    return $inner.FullName
}

# FFmpeg: LGPL shared build (BtbN) -> third_party/ffmpeg/{include,lib,bin}
$ff = Join-Path $tp "ffmpeg"
if (-not (Test-Path (Join-Path $ff "include"))) {
    Write-Host "download FFmpeg (LGPL shared, win64) ..."
    $zip = Join-Path $tmp "ffmpeg.zip"
    Get-File $ffmpegUrl $zip
    if ((Get-Item $zip).Length -lt 10MB) { throw "ffmpeg archive too small, download likely truncated" }
    Expand-Archive $zip -DestinationPath (Join-Path $tmp "ffmpeg") -Force
    $from = Copy-Flattened (Join-Path $tmp "ffmpeg") "include" $ff
    Write-Host "ffmpeg from: $from"
}

# SDL2: VC development package -> third_party/SDL2/{include,lib}
$sdl = Join-Path $tp "SDL2"
if (-not (Test-Path (Join-Path $sdl "include"))) {
    Write-Host "download SDL2 2.32.4 ..."
    $zip = Join-Path $tmp "sdl2.zip"
    Get-File $sdlUrl $zip
    if ((Get-Item $zip).Length -lt 1MB) { throw "SDL2 archive too small, download likely truncated" }
    Expand-Archive $zip -DestinationPath (Join-Path $tmp "sdl2") -Force
    $from = Copy-Flattened (Join-Path $tmp "sdl2") "include" $sdl
    Write-Host "sdl2 from: $from"
    # The VC package keeps binaries under lib/x64; CMakeLists links -L third_party/SDL2/lib
    $x64 = Join-Path $sdl "lib\x64"
    if (Test-Path $x64) {
        Copy-Item (Join-Path $x64 "*") (Join-Path $sdl "lib") -Force
        Remove-Item $x64 -Recurse -Force
    }
}

if (-not $SkipModels) {
    $models = Join-Path $tp "models"
    New-Item -ItemType Directory -Force $models | Out-Null
    $mt = Join-Path $models "Hy-MT2-1.8B-Q4_K_M.gguf"
    if (-not (Test-Path $mt)) {
        Write-Warning "translator model missing: $mt"
        Write-Host  "  Download the Q4_K_M GGUF build of Hy-MT2-1.8B from HuggingFace and rename it to the name above."
        Write-Host  "  https://huggingface.co/models?search=Hy-MT2-1.8B%20GGUF"
    }
    $wh = Join-Path $tp "whisper.cpp\models\ggml-small.bin"
    if (-not (Test-Path $wh)) {
        Write-Host "download whisper ggml-small.bin (~465MB) ..."
        Get-File "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small.bin" $wh
    }

    # Silero VAD for realtime subtitles (tiny, ~865KB). Mirrors first: HF is often
    # unreachable from mainland China; the file is identical on both.
    $vad = Join-Path $tp "whisper.cpp\models\ggml-silero-v5.1.2.bin"
    if (-not (Test-Path $vad)) {
        $urls = @(
            "https://hf-mirror.com/ggml-org/whisper-vad/resolve/main/ggml-silero-v5.1.2.bin",
            "https://huggingface.co/ggml-org/whisper-vad/resolve/main/ggml-silero-v5.1.2.bin"
        )
        foreach ($u in $urls) {
            try {
                Write-Host "download silero vad model from $u"
                Invoke-WebRequest $u -OutFile $vad -UseBasicParsing
                if ((Get-Item $vad).Length -gt 100000) { break }
            } catch {
                Write-Warning "vad download failed: $($_.Exception.Message)"
            }
        }        if (-not (Test-Path $vad) -or (Get-Item $vad).Length -lt 100000) {
            Write-Warning "silero vad model missing: $vad (realtime subtitles will fall back to fixed windows)"
        }
    }
}

Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
Write-Host "done. hashes: docs/THIRD_PARTY_VERSIONS.md"
