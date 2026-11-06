# Windows installer: installs the build tools, links your game files, builds
# the ReXGlue SDK and the game from source on this PC, and adds shortcuts.
# Nothing is downloaded from this project except source code; the game is
# translated from your own default.xex.
#
# Start it with install-windows.bat in the repository folder, or:
#   powershell -ExecutionPolicy Bypass -File tools\install-windows.ps1 [-GameDir D:\games\storm3] [-SdkDir C:\dev\rexglue-sdk]
# Running it again is safe: finished steps are skipped and the game is rebuilt.
param(
    [string]$GameDir = "",
    [string]$SdkDir = "C:\dev\rexglue-sdk",
    [switch]$SkipTools,
    [switch]$NoShortcuts
)
$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $PSScriptRoot
Set-Location $ProjectRoot

function Step($text) { Write-Host ""; Write-Host "==> $text" -ForegroundColor Cyan }
function Note($text) { Write-Host "    $text" -ForegroundColor DarkGray }
function Fail($text) { Write-Host ""; Write-Host "ERROR: $text" -ForegroundColor Red; exit 1 }

function Invoke-Checked {
    & $args[0] $args[1..($args.Count - 1)]
    if ($LASTEXITCODE -ne 0) { Fail "Command failed ($LASTEXITCODE): $($args -join ' ')" }
}

function Update-PathFromRegistry {
    $machine = [Environment]::GetEnvironmentVariable("Path", "Machine")
    $user = [Environment]::GetEnvironmentVariable("Path", "User")
    $env:Path = "$machine;$user"
    foreach ($extra in "$env:ProgramFiles\LLVM\bin", "$env:ProgramFiles\CMake\bin",
                       "$env:LOCALAPPDATA\Microsoft\WinGet\Links") {
        if ((Test-Path $extra) -and ($env:Path -notlike "*$extra*")) { $env:Path = "$extra;$env:Path" }
    }
}

function Find-VisualStudio {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { return $null }
    $path = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
    if ($path) { return $path.Trim() }
    return $null
}

Write-Host "Naruto Storm 3 Recomp - Windows installer" -ForegroundColor Yellow
Write-Host "This builds everything from source. The first run takes 30-90 minutes"
Write-Host "and needs about 25 GB of free disk space and 16 GB of RAM."

# --- Checks ------------------------------------------------------------------
if (-not [Environment]::Is64BitOperatingSystem) { Fail "A 64-bit Windows is required." }
if ($ProjectRoot.Length -gt 60) {
    Write-Host "WARNING: the repository path is long ($ProjectRoot)." -ForegroundColor Yellow
    Write-Host "         Long paths can break the build. C:\dev\naruto-recomp is a good place." -ForegroundColor Yellow
}
try {
    $longPaths = (Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem" -Name LongPathsEnabled -ErrorAction Stop).LongPathsEnabled
} catch { $longPaths = 0 }
if ($longPaths -ne 1) {
    Note "Windows long path support is off. If the build fails with 'path too long', enable it:"
    Note "  (admin PowerShell) Set-ItemProperty HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem LongPathsEnabled 1"
}

# --- 1. Build tools ----------------------------------------------------------
Step "1/6  Build tools"
Update-PathFromRegistry
if (-not $SkipTools) {
    $winget = Get-Command winget -ErrorAction SilentlyContinue
    $packages = @(
        @{ Cmd = "git";    Id = "Git.Git";            Name = "Git" },
        @{ Cmd = "cmake";  Id = "Kitware.CMake";      Name = "CMake" },
        @{ Cmd = "ninja";  Id = "Ninja-build.Ninja";  Name = "Ninja" },
        @{ Cmd = "clang";  Id = "LLVM.LLVM";          Name = "LLVM (Clang)" },
        @{ Cmd = "py";     Id = "Python.Python.3.12"; Name = "Python 3" }
    )
    foreach ($p in $packages) {
        $found = Get-Command $p.Cmd -ErrorAction SilentlyContinue
        if ($p.Cmd -eq "py" -and -not $found) { $found = Get-Command python -ErrorAction SilentlyContinue }
        if ($found) { Note "$($p.Name): found"; continue }
        if (-not $winget) { Fail "$($p.Name) is missing and winget is not available. Install it by hand (see README.md)." }
        Note "Installing $($p.Name)..."
        & winget install --id $p.Id -e --source winget --accept-package-agreements --accept-source-agreements --silent
        Update-PathFromRegistry
    }
    if (-not (Find-VisualStudio)) {
        if (-not $winget) { Fail "Visual Studio 2022 C++ build tools are missing. Install them by hand (see README.md)." }
        Note "Installing Visual Studio 2022 Build Tools (C++). This is a large download..."
        & winget install --id Microsoft.VisualStudio.2022.BuildTools -e --source winget `
            --accept-package-agreements --accept-source-agreements `
            --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
    }
}
$vs = Find-VisualStudio
if (-not $vs) { Fail "Visual Studio 2022 with the C++ tools was not found." }
Note "Visual Studio: $vs"

# Load the x64 compiler environment (like the "x64 Native Tools" prompt).
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
$envBat = Join-Path $env:TEMP "naruto-vcvars.bat"
Set-Content -Path $envBat -Encoding ASCII -Value "@call `"$vcvars`" >nul`r`n@set"
cmd /c "`"$envBat`"" | ForEach-Object {
    if ($_ -match "^([^=]+)=(.*)$") { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
}
Remove-Item $envBat -ErrorAction SilentlyContinue
# LLVM's Clang must win over any older Clang bundled with Visual Studio.
if (Test-Path "$env:ProgramFiles\LLVM\bin") { $env:Path = "$env:ProgramFiles\LLVM\bin;$env:Path" }
foreach ($tool in "git", "cmake", "ninja", "clang", "clang++", "cl") {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        Fail "$tool is still not on PATH. Close this window and run install-windows.bat again."
    }
}
$clangVersion = (& clang --version | Select-Object -First 1)
Note $clangVersion
if ($clangVersion -match "version (\d+)" -and [int]$Matches[1] -lt 20) {
    Fail "Clang 20 or newer is needed (found $($Matches[1])). Update LLVM: winget upgrade LLVM.LLVM"
}
$python = "py"
$pythonArgs = @("-3")
if (-not (Get-Command py -ErrorAction SilentlyContinue)) { $python = "python"; $pythonArgs = @() }

# --- 2. Game files -----------------------------------------------------------
Step "2/6  Game files"
$assets = Join-Path $ProjectRoot "assets"
if (-not (Test-Path (Join-Path $assets "default.xex"))) {
    if (-not $GameDir) {
        Add-Type -AssemblyName System.Windows.Forms
        $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
        $dialog.Description = "Choose your extracted Naruto Storm 3 Full Burst folder (the one with default.xex)"
        $dialog.ShowNewFolderButton = $false
        if ($dialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { Fail "No game folder chosen." }
        $GameDir = $dialog.SelectedPath
    }
    if (-not (Test-Path (Join-Path $GameDir "default.xex"))) {
        Fail "$GameDir has no default.xex. Choose the extracted game folder itself."
    }
    if (Test-Path $assets) {
        if ((Get-ChildItem $assets -Force | Measure-Object).Count -gt 0) {
            Fail "assets\ exists but has no default.xex. Empty or remove it, then run again."
        }
        Remove-Item $assets -Force
    }
    # A junction points assets\ at your folder without copying 10 GB.
    try {
        New-Item -ItemType Junction -Path $assets -Target (Resolve-Path $GameDir).Path | Out-Null
        Note "Linked assets\ -> $GameDir"
    } catch {
        Note "Could not link the folder; copying it instead (this takes a while)..."
        Copy-Item -Recurse (Resolve-Path $GameDir).Path $assets
    }
}
& $python @pythonArgs tools\check-game.py
if ($LASTEXITCODE -ne 0) { Fail "The game files are not the version this project needs (see above)." }

# --- 3. ReXGlue SDK ----------------------------------------------------------
Step "3/6  ReXGlue SDK (clone, patch, build; the longest step)"
$sdkPrefix = Join-Path $SdkDir "out\install\win-amd64"
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "setup-sdk.ps1") -SdkDir $SdkDir
if ($LASTEXITCODE -ne 0) { Fail "The SDK setup failed (see above)." }
Set-Location $ProjectRoot

# --- 4. Translate and build the game ----------------------------------------
Step "4/6  Translating default.xex and building the game"
$prefixArg = "-DCMAKE_PREFIX_PATH=$($sdkPrefix -replace '\\', '/')"
Invoke-Checked cmake --preset win-amd64-release $prefixArg
Invoke-Checked cmake --build --preset win-amd64-release --target naruto_codegen
Invoke-Checked cmake --preset win-amd64-release
Invoke-Checked cmake --build --preset win-amd64-release --parallel 4

$buildDir = Join-Path $ProjectRoot "out\build\win-amd64-release"
$exe = Join-Path $buildDir "naruto.exe"
if (-not (Test-Path $exe)) { Fail "naruto.exe was not produced." }
# The runtime DLLs and the GPU plugin must sit next to the executable.
Get-ChildItem (Join-Path $sdkPrefix "bin") -Filter *.dll -ErrorAction SilentlyContinue |
    ForEach-Object { Copy-Item $_.FullName $buildDir -Force }

# --- 5. Shortcuts ------------------------------------------------------------
Step "5/6  Shortcuts"
if (-not $NoShortcuts) {
    $shell = New-Object -ComObject WScript.Shell
    $play = Join-Path $PSScriptRoot "play-windows.ps1"
    $icon = "$exe,0"
    $startMenu = Join-Path ([Environment]::GetFolderPath("Programs")) "Naruto Storm 3 Recomp"
    New-Item -ItemType Directory -Force -Path $startMenu | Out-Null
    $links = @(
        @{ Path = (Join-Path ([Environment]::GetFolderPath("Desktop")) "Naruto Storm 3.lnk"); Args = "" },
        @{ Path = (Join-Path $startMenu "Naruto Storm 3.lnk"); Args = "" },
        @{ Path = (Join-Path $startMenu "Naruto Storm 3 - Online (experimental).lnk"); Args = " -Online" }
    )
    foreach ($l in $links) {
        $s = $shell.CreateShortcut($l.Path)
        $s.TargetPath = "powershell.exe"
        $s.Arguments = "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$play`"$($l.Args)"
        $s.WorkingDirectory = $ProjectRoot
        $s.IconLocation = $icon
        $s.Save()
        Note "Created $($l.Path)"
    }
}

# --- 6. Done -----------------------------------------------------------------
Step "6/6  Done"
Write-Host "Start the game from the 'Naruto Storm 3' shortcut, or run:"
Write-Host "  powershell -ExecutionPolicy Bypass -File tools\play-windows.ps1"
Write-Host "Online (experimental): the 'Online' shortcut, or add -Online -Peer <friend's IP>."
Write-Host "After pulling updates, run install-windows.bat again to rebuild."
