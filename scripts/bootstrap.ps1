# Bootstrap vcpkg dependencies for a clean checkout of MoeKoeTaskbarLyrics.
# ASCII-only output on purpose: Windows PowerShell 5.1 mangles non-ASCII text.
# Works with Windows PowerShell 5.1 and PowerShell 7+.
#
#   .\scripts\bootstrap.ps1 -Arch x64 -Build
#   .\scripts\bootstrap.ps1 -Arch arm64
#   .\scripts\bootstrap.ps1 -Arch x64 -VcpkgRoot 'D:\vcpkg'
#   .\scripts\bootstrap.ps1 -Arch x64 -VcpkgCommit <sha> -Build
#   .\scripts\bootstrap.ps1 -Arch x64 -VcpkgRoot 'D:\ci\vcpkg' -VcpkgCommit <sha>
#   .\scripts\bootstrap.ps1 -Arch x64 -CheckOnly
#
# -VcpkgCommit is applied only when the target root has to be cloned; an
# existing vcpkg tree is never switched silently (a WARN is printed instead).
#
# The selected vcpkg root is written to .vcpkgroot (git-ignored, one line).
# Both build.cmd and CMakeLists.txt read that file, so the command line and
# the VS IDE resolve the exact same dependency tree without any machine-level
# environment variable.
#
# This uses vcpkg classic mode (installed/<triplet>), matching the existing
# find_library / POST_BUILD assumptions in CMakeLists.txt. It does NOT switch
# the project to manifest mode.

[CmdletBinding()]
param(
    [ValidateSet('x64', 'arm64')]
    [string]$Arch = 'x64',
    [string]$VcpkgRoot = '',
    [string]$VcpkgCommit = '',
    [switch]$Build,
    [switch]$CheckOnly
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$triplet = "$Arch-windows"
$packages = @('ixwebsocket', 'nlohmann-json', 'zlib', 'mbedtls', 'kissfft')
$pinFile = Join-Path $root '.vcpkgroot'

function Say([string]$msg) { Write-Output $msg }

function Test-VcpkgSourceTree([string]$path) {
    if (-not $path) { return $false }
    return ((Test-Path (Join-Path $path 'vcpkg.exe')) -or
            (Test-Path (Join-Path $path 'bootstrap-vcpkg.bat')))
}

# --- 1. Pick a vcpkg root, preferring one that already exists --------------
# -VcpkgRoot may point at a directory that does not exist yet (CI does this).
# In that case we clone into exactly that path and honour -VcpkgCommit, so a
# fresh runner and a fresh developer clone run the SAME preparation code.
function Initialize-VcpkgClone {
    param([string]$Path, [string]$Commit)
    $Path = [System.IO.Path]::GetFullPath($Path)
    Say "No usable vcpkg at $Path; cloning..."
    $parent = Split-Path -Parent $Path
    if (-not (Test-Path $parent)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    if (-not (Test-Path (Join-Path $Path '.git'))) {
        & git clone https://github.com/microsoft/vcpkg.git $Path
        if ($LASTEXITCODE -ne 0) { throw 'git clone failed' }
    }
    if ($Commit) {
        & git -C $Path checkout $Commit
        if ($LASTEXITCODE -ne 0) { throw "checkout $Commit failed" }
    }
}

$selected = $null
$source = $null

if ($VcpkgRoot) {
    $selected = $VcpkgRoot
    $source = '-VcpkgRoot'
} elseif (Test-Path $pinFile) {
    $pinned = (Get-Content $pinFile -TotalCount 1).Trim()
    if ($pinned -and (Test-VcpkgSourceTree $pinned)) {
        $selected = $pinned
        $source = '.vcpkgroot (previous bootstrap)'
    }
}
if (-not $selected) {
    foreach ($candidate in @($env:VCPKG_ROOT, $env:VCPKG_INSTALLATION_ROOT, 'D:\vcpkg', 'C:\vcpkg')) {
        if (Test-VcpkgSourceTree $candidate) {
            $selected = $candidate
            $source = 'existing local vcpkg'
            break
        }
    }
}
if (-not $selected) {
    $selected = Join-Path $env:LOCALAPPDATA 'MoeKoeTaskbarLyrics\vcpkg'
    $source = 'auto-managed clone'
}

$selected = [System.IO.Path]::GetFullPath($selected)
if (-not (Test-VcpkgSourceTree $selected)) {
    Initialize-VcpkgClone -Path $selected -Commit $VcpkgCommit
    $source = 'fresh clone'
}
Say "vcpkg root : $selected  [$source]"
Say "triplet    : $triplet"

# --- 2. Version pin is opt-in; never silently switch an existing tree -----
if ($VcpkgCommit -and (Test-Path (Join-Path $selected '.git'))) {
    $head = (& git -C $selected rev-parse HEAD 2>$null)
    if ($head) { $head = ([string]$head).Trim() }
    if ($head -and -not $head.StartsWith($VcpkgCommit, [System.StringComparison]::OrdinalIgnoreCase)) {
        Say "WARN: -VcpkgCommit $VcpkgCommit requested, but this root is at $head."
        Say "WARN: refusing to switch an existing tree; delete it or pass -VcpkgRoot to a fresh clone."
    }
}

# --- 3. Bootstrap vcpkg.exe if needed -------------------------------------
$vcpkgExe = Join-Path $selected 'vcpkg.exe'
if (-not (Test-Path $vcpkgExe)) {
    $bootstrap = Join-Path $selected 'bootstrap-vcpkg.bat'
    Say 'Bootstrapping vcpkg.exe (first run only)...'
    & $bootstrap -disableMetrics
    if ($LASTEXITCODE -ne 0) { throw 'bootstrap-vcpkg.bat failed' }
}

if ($CheckOnly) {
    $preflight = Join-Path $PSScriptRoot 'check_build_env.ps1'
    & $preflight -Arch $Arch -VcpkgRoot $selected
    # 'return', not 'exit': GitHub Actions dot-sources PowerShell steps,
    # where 'exit' would terminate the whole step before its remaining lines.
    return
}

# --- 3.5 Ensure the ixwebsocket submodule is present ----------------------
# CI checks out with submodules: recursive; a plain `git clone` does not.
# Make bootstrap the single entry point so both paths compile ixwebsocket
# from source instead of silently falling back to the vcpkg prebuilt lib.
$submoduleHeader = Join-Path $root 'third_party\ixwebsocket\ixwebsocket\IXWebSocket.h'
if (-not (Test-Path $submoduleHeader)) {
    if (Test-Path (Join-Path $root '.git')) {
        Say 'Initializing git submodules (ixwebsocket source)...'
        & git -C $root submodule update --init --recursive
        if ($LASTEXITCODE -ne 0) { throw 'git submodule update failed' }
    } else {
        Say 'WARN: ixwebsocket submodule missing and this is not a git checkout; CMake will use the vcpkg prebuilt lib.'
    }
}

# --- 4. Install the five dependencies for this triplet --------------------
Say "Installing: $($packages -join ' ') --triplet $triplet --classic"
& $vcpkgExe install @packages --triplet $triplet --classic
if ($LASTEXITCODE -ne 0) { throw "vcpkg install failed for $triplet" }

# --- 5. Verify the files CMakeLists.txt actually consumes -----------------
$installed = Join-Path $selected "installed\$triplet"
$required = @(
    'include\zlib.h', 'include\nlohmann\json.hpp',
    'include\kissfft\kiss_fft.h', 'include\ixwebsocket\IXWebSocket.h',
    'lib\z.lib', 'lib\mbedtls.lib', 'lib\mbedx509.lib', 'lib\mbedcrypto.lib',
    'lib\kissfft-float.lib', 'lib\ixwebsocket.lib',
    'bin\z.dll', 'bin\kissfft-float.dll'
)
foreach ($f in $required) {
    if (-not (Test-Path (Join-Path $installed $f))) {
        throw "Incomplete install: missing $installed\$f"
    }
}
Say "Verified $($required.Count) required files under $installed"

# --- 6. Pin the root for both build.cmd and CMakeLists.txt ----------------
Set-Content -Path $pinFile -Value $selected -Encoding ASCII -NoNewline
Say "Pinned vcpkg root in $pinFile (git-ignored)"
Say 'Hint: do NOT set VCPKG_INSTALLED_DIR as a permanent user variable; it would break other architectures.'

# --- 7. Optional build -----------------------------------------------------
if ($Build) {
    $env:VCPKG_ROOT = $selected
    Remove-Item Env:\VCPKG_INSTALLED_DIR -ErrorAction SilentlyContinue
    $buildCmd = Join-Path $root 'build.cmd'
    & $buildCmd release $Arch
    if ($LASTEXITCODE -ne 0) { throw "build failed for $Arch" }
    $preset = if ($Arch -eq 'x64') { 'x64-Release' } else { 'ARM64-Release' }
    Say "Build done. Tests: ctest --test-dir out\build\$preset -C Release --output-on-failure"
} else {
    Say "Done. Next: .\build.cmd release $Arch   (or re-run this script with -Build)"
}
return
