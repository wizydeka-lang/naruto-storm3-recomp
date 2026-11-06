# Clone ReXGlue at the revision this project was built against, apply the
# project's SDK patches, then build and install it with the Vulkan backend,
# which the game's native renderer needs. Run from an "x64 Native Tools"
# PowerShell session (install-windows.bat does this for you).
# Usage: powershell -ExecutionPolicy Bypass -File tools\setup-sdk.ps1 [-SdkDir C:\dev\rexglue-sdk] [-WithDebug]
param(
    [string]$SdkDir = "C:\dev\rexglue-sdk",
    # Also build the Debug runtime (only needed for debugging the game).
    [switch]$WithDebug
)
$ErrorActionPreference = "Stop"

$SdkRevision = "c94f5ebdcb3c9d1a460ca48e04f9758448f8d518"
$ProjectRoot = Split-Path -Parent $PSScriptRoot

function Invoke-Checked {
    & $args[0] $args[1..($args.Count - 1)]
    if ($LASTEXITCODE -ne 0) { throw "Command failed ($LASTEXITCODE): $($args -join ' ')" }
}

foreach ($tool in "git", "cmake", "ninja", "clang", "clang++", "cl") {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "Missing $tool. Install the prerequisites and use an x64 Native Tools PowerShell."
    }
}

if (-not (Test-Path "$SdkDir\.git")) {
    # LF checkout keeps the project patches applying cleanly.
    Invoke-Checked git clone -c core.autocrlf=input -c core.longpaths=true `
        https://github.com/rexglue/rexglue-sdk.git $SdkDir
}
Set-Location $SdkDir
if ((git rev-parse HEAD) -ne $SdkRevision) {
    if (git status --porcelain --untracked-files=no) {
        throw "$SdkDir has local changes and is not at $SdkRevision; use another directory."
    }
    Invoke-Checked git fetch origin
    Invoke-Checked git checkout --detach $SdkRevision
}
Invoke-Checked git submodule update --init --recursive

# Apply each patch once; a patch that already reverses cleanly is in place.
foreach ($patch in Get-ChildItem "$ProjectRoot\patches\*.patch") {
    git apply --reverse --check $patch.FullName 2>$null
    if ($LASTEXITCODE -eq 0) {
        Write-Host "Already applied: $($patch.Name)"
    } else {
        Invoke-Checked git apply $patch.FullName
        Write-Host "Applied: $($patch.Name)"
    }
}

# FidelityFX headers enable the FSR 1 and CAS upscalers, whose shaders ship
# with the SDK. Only the headers are fetched; the full FidelityFX runtime
# (FSR 2/3) is not built, so REXGLUE_ENABLE_FIDELITYFX stays off.
$FfxRevision = "eee08db1688ac3d1275a70b728f4a8ba22914213"
$FfxDir = "$SdkDir\out\fidelityfx-sdk"
if (-not (Test-Path "$FfxDir\sdk\include")) {
    Invoke-Checked git init -q $FfxDir
    Invoke-Checked git -C $FfxDir remote add origin https://github.com/rexglue/FidelityFX-SDK.git
    Invoke-Checked git -C $FfxDir sparse-checkout set --no-cone /sdk/include/ /ffx-api/include/
    Invoke-Checked git -C $FfxDir fetch -q --depth 1 --filter=blob:none origin $FfxRevision
    Invoke-Checked git -C $FfxDir checkout -q FETCH_HEAD
}

Invoke-Checked cmake --preset win-amd64 -DREXGLUE_USE_VULKAN=ON -DREXGLUE_USE_D3D12=OFF `
    -DREXGLUE_ENABLE_FIDELITYFX=OFF "-DREXGLUE_FIDELITYFX_SOURCE_DIR=$FfxDir"
$configs = @("Release")
if ($WithDebug) { $configs += "Debug" }
foreach ($config in $configs) {
    # Four jobs keeps compiler memory use reasonable on most machines.
    Invoke-Checked cmake --build out/build/win-amd64 --config $config --target install --parallel 4
}
Write-Host "ReXGlue SDK installed in $SdkDir\out\install\win-amd64"
