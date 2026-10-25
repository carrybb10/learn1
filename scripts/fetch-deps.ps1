# 拉取第三方源码、预编译库与模型（不入 git）
# 用法：.\scripts\fetch-deps.ps1 [-SkipModels]
param([switch]$SkipModels)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$tp = Join-Path $root "third_party"
New-Item -ItemType Directory -Force $tp | Out-Null

# 版本号与 docs/THIRD_PARTY_VERSIONS.md 保持一致
$llamaCommit   = "4ebdf2c74acce30883d8e34b7c70b3eb8146f2fe"
$whisperCommit = "60c0be6ac8fa71b1a2ae2dd938a31a34a508e774"

function Ensure-Repo($name, $url, $commit) {
    $dir = Join-Path $tp $name
    if (-not (Test-Path (Join-Path $dir ".git"))) {
        Write-Host "clone $name ..."
        git clone --filter=blob:none $url $dir
    }
    Push-Location $dir
    git fetch --depth 1 origin $commit 2>$null
    git checkout --quiet $commit
    Pop-Location
    Write-Host "$name @ $commit ok"
}

Ensure-Repo "llama.cpp"   "https://github.com/ggml-org/llama.cpp.git"       $llamaCommit
Ensure-Repo "whisper.cpp" "https://github.com/ggml-org/whisper.cpp.git"     $whisperCommit

# FFmpeg：LGPL shared build（BtbN），解压到 third_party/ffmpeg（含 include/lib/bin）
$ff = Join-Path $tp "ffmpeg"
if (-not (Test-Path (Join-Path $ff "include"))) {
    Write-Host "下载 FFmpeg (LGPL shared, win64) ..."
    $zip = Join-Path $env:TEMP "ffmpeg-lgpl.zip"
    Invoke-WebRequest "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-lgpl-shared.zip" -OutFile $zip
    Expand-Archive $zip -DestinationPath $env:TEMP -Force
    $inner = Get-ChildItem $env:TEMP -Directory -Filter "ffmpeg-master-latest-win64-lgpl-shared" | Select-Object -First 1
    New-Item -ItemType Directory -Force $ff | Out-Null
    Copy-Item "$($inner.FullName)\*" $ff -Recurse -Force
}

# SDL2：开发包解压到 third_party/SDL2（include/ 直接在根下，符合 CMakeLists 预期）
$sdl = Join-Path $tp "SDL2"
if (-not (Test-Path (Join-Path $sdl "include"))) {
    Write-Host "下载 SDL2 2.32.x ..."
    $zip = Join-Path $env:TEMP "sdl2.zip"
    Invoke-WebRequest "https://github.com/libsdl-org/SDL/releases/download/release-2.32.4/SDL2-devel-2.32.4-VC.zip" -OutFile $zip
    Expand-Archive $zip -DestinationPath $env:TEMP -Force
    Copy-Item "$env:TEMP\SDL2-devel-2.32.4-VC\SDL2\*" $sdl -Recurse -Force
}

if (-not $SkipModels) {
    $models = Join-Path $tp "models"
    New-Item -ItemType Directory -Force $models | Out-Null
    $mt = Join-Path $models "Hy-MT2-1.8B-Q4_K_M.gguf"
    if (-not (Test-Path $mt)) {
        Write-Warning "缺翻译模型：$mt"
        Write-Host  "  从 HuggingFace 的 Hy-MT2-1.8B GGUF 量化仓库下载 Q4_K_M 权重，重命名成上面的文件名。"
        Write-Host  "  参考：https://huggingface.co/models?search=Hy-MT2-1.8B%20GGUF"
    }
    $wh = Join-Path $tp "whisper.cpp\models\ggml-small.bin"
    if (-not (Test-Path $wh)) {
        Write-Host "下载 whisper ggml-small.bin (~465MB) ..."
        Invoke-WebRequest "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small.bin" -OutFile $wh
    }
}

Write-Host "done. 校验哈希见 docs/THIRD_PARTY_VERSIONS.md"
