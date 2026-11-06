# Starts the Windows build with the native renderer, as the shortcuts do.
#   powershell -ExecutionPolicy Bypass -File tools\play-windows.ps1 [-Online] [-Peer 100.64.0.2] [-Classic] [-- game options]
# -Online   experimental online play (see README.md, "Online mode").
# -Peer     the other player's IP address; asked for when -Online is given without it.
# -Classic  use Xenia's renderer instead of the native one (slower; for troubleshooting).
# Anything else is passed to the game, e.g. --fullscreen=true or --mnk_mode=true.
param(
    [switch]$Online,
    [string]$Peer = "",
    [switch]$Classic,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$GameArgs = @()
)
$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $ProjectRoot "out\build\win-amd64-release"
$exe = Join-Path $buildDir "naruto.exe"
$assets = (Join-Path $ProjectRoot "assets")

function Show-Error($text) {
    Add-Type -AssemblyName System.Windows.Forms
    [System.Windows.Forms.MessageBox]::Show($text, "Naruto Storm 3") | Out-Null
    exit 1
}
if (-not (Test-Path $exe)) { Show-Error "naruto.exe not found. Run install-windows.bat first." }
if (-not (Test-Path (Join-Path $assets "default.xex"))) { Show-Error "Game files not found in $assets." }

$arguments = @(
    "--game_data_root=$((Resolve-Path $assets).Path)", "--gpu_plugin=xenos",
    "--async_shader_compilation=true",
    "--vulkan_allow_present_mode_immediate=false",
    "--vulkan_allow_present_mode_mailbox=false",
    "--vulkan_allow_present_mode_fifo_relaxed=false"
)
if (-not $Classic) {
    $env:NARUTO_NATIVE_VIEW = "1"
    $env:NARUTO_NATIVE_VIEW_ONLY = "1"
    $arguments += "--vulkan_skip_rendering=true"
}

if ($Online) {
    $saved = Join-Path $env:LOCALAPPDATA "naruto-recomp\netplay_peer.txt"
    if (-not $Peer) {
        $last = ""
        if (Test-Path $saved) { $last = (Get-Content $saved -Raw).Trim() }
        $mine = (Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
            Where-Object { $_.IPAddress -notlike "127.*" -and $_.IPAddress -notlike "169.254.*" } |
            Select-Object -ExpandProperty IPAddress) -join ", "
        Add-Type -AssemblyName Microsoft.VisualBasic
        $Peer = [Microsoft.VisualBasic.Interaction]::InputBox(
            "Online mode is experimental.`n`nEnter the other player's IP address (LAN or Tailscale)." +
            "`nLeave it empty to find games on your local network only.`n`nYour IP: $mine",
            "Naruto Storm 3 - Online", $last)
    }
    $Peer = $Peer.Trim()
    New-Item -ItemType Directory -Force -Path (Split-Path $saved) | Out-Null
    Set-Content -Path $saved -Value $Peer
    $arguments += "--netplay=true"
    if ($Peer) { $arguments += "--netplay_peer=$Peer" }
}

# Windows PowerShell's Start-Process does not quote arguments with spaces.
$quoted = ($arguments + $GameArgs) | Where-Object { $_ } | ForEach-Object {
    if ($_ -match '\s') { '"' + $_ + '"' } else { $_ }
}
Start-Process -FilePath $exe -WorkingDirectory $buildDir -ArgumentList $quoted
