# Build font_gen.wasm + font_gen.js using Emscripten inside Docker.
# Requires: Docker Desktop running on Windows.
#
# Usage (from repo root or tools/font_gen/):
#   powershell -ExecutionPolicy Bypass -File tools/font_gen/build_wasm_docker.ps1

$ErrorActionPreference = 'Stop'

$scriptDir  = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot   = (Resolve-Path "$scriptDir\..\..")
$toolDir    = $scriptDir
$outDir     = "$repoRoot\docs"

# Convert Windows paths to Docker-compatible paths (forward slashes, /c/ prefix)
function To-DockerPath($path) {
    $abs = (Resolve-Path $path).Path
    # C:\foo\bar -> /c/foo/bar
    $abs -replace '^([A-Za-z]):\\', { '/'+$_.Groups[1].Value.ToLower()+'/' } -replace '\\', '/'
}

$dockerToolDir = To-DockerPath $toolDir
$dockerOutDir  = To-DockerPath $outDir

Write-Host "=== Building font_gen WASM via Docker ===" -ForegroundColor Cyan
Write-Host "Tool dir : $toolDir"
Write-Host "Output   : $outDir"

# Run emcmake cmake + emmake build inside the official Emscripten Docker image.
# We mount the tools/font_gen source and docs output directories.
docker run --rm `
    -v "${toolDir}:/src" `
    -v "${outDir}:/out" `
    -w /src `
    emscripten/emsdk:latest `
    bash -c @'
set -e
echo "=== emcmake configure ==="
emcmake cmake /src \
    -B /tmp/build_wasm \
    -DCMAKE_BUILD_TYPE=Release \
    -DFONT_GEN_WASM=ON

echo "=== emmake build ==="
emmake cmake --build /tmp/build_wasm --target font_gen -j$(nproc)

echo "=== Copy output ==="
cp /tmp/build_wasm/font_gen.js   /out/font_gen.js
cp /tmp/build_wasm/font_gen.wasm /out/font_gen.wasm

echo "=== Done ==="
ls -lh /out/font_gen.js /out/font_gen.wasm
'@

Write-Host ""
Write-Host "Output files:" -ForegroundColor Green
Get-Item "$outDir\font_gen.js", "$outDir\font_gen.wasm" | Format-Table Name, Length -AutoSize
