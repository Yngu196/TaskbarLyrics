# Read-only preflight for a clean Windows build of MoeKoeTaskbarLyrics.
# ASCII-only output on purpose: Windows PowerShell 5.1 mangles non-ASCII text.
# Works with Windows PowerShell 5.1 and PowerShell 7+.
#
#   .\scripts\check_build_env.ps1 -Arch x64
#   .\scripts\check_build_env.ps1 -Arch arm64 -VcpkgRoot 'D:\vcpkg'
#   .\scripts\check_build_env.ps1 -Arch x64 -Entry vs
#
# build.cmd and CMakeLists.txt now share ONE resolution order (see below),
# so -Entry is kept only for regression comparison. A complete tree sitting
# in some other directory must NOT count: the build will not use it.
# This script never writes anything.

param(
    [ValidateSet('x64', 'arm64')]
    [string]$Arch = 'x64',
    [string]$VcpkgRoot = '',
    [string]$InstalledDir = '',
    [ValidateSet('build', 'vs')]
    [string]$Entry = 'build'
)

$ErrorActionPreference = 'SilentlyContinue'
$root = Split-Path -Parent $PSScriptRoot
$triplet = "$Arch-windows"
$fail = 0
$warn = 0

function Report {
    param([string]$Level, [string]$Name, [string]$Detail)
    Write-Output (("{0,-4} {1,-27} {2}" -f $Level, $Name, $Detail).TrimEnd())
    if ($Level -eq 'FAIL') { $script:fail++ }
    if ($Level -eq 'WARN') { $script:warn++ }
}

function Find-CommandVersion {
    param([string]$Exe, [string]$Pattern)
    $line = & $Exe --version 2>$null | Select-Object -First 1
    if (-not $line) { return $null }
    $m = [regex]::Match([string]$line, $Pattern)
    if ($m.Success) { return [version]$m.Groups[1].Value }
    return $null
}

Write-Output ("Build preflight: arch=$Arch triplet=$triplet entry=$Entry")
Write-Output (("Project root: {0}" -f $root))
Write-Output ('')

# --- Toolchain -------------------------------------------------------------
$cmakeVer = Find-CommandVersion -Exe 'cmake' -Pattern 'version\s+(\d+\.\d+\.\d+)'
Report $(if ($cmakeVer -and $cmakeVer -ge [version]'3.20.0') { 'OK' } else { 'FAIL' }) `
    'CMake >= 3.20' $(if ($cmakeVer) { "$cmakeVer" } else { 'cmake not on PATH' })

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsPath = $null
if (Test-Path $vswhere) {
    $vsPath = (& $vswhere -latest -products * -property installationPath 2>$null | Select-Object -First 1)
}
if (-not $vsPath -or -not (Test-Path $vsPath)) {
    Report 'FAIL' 'VS2022 (vswhere)' 'Visual Studio 2022 not found'
    $toolsetDir = $null
} else {
    $msvcRoot = Join-Path $vsPath 'VC\Tools\MSVC'
    $versions = @(Get-ChildItem $msvcRoot -Directory 2>$null |
        ForEach-Object { try { [version]$_.Name } catch { $null } } |
        Where-Object { $_ -ne $null } | Sort-Object -Descending)
    $best = $versions | Select-Object -First 1
    Report $(if ($best -and $best -ge [version]'14.44.0') { 'OK' } else { 'FAIL' }) `
        'MSVC v143 >= 14.44' $(if ($best) { "$best  ($vsPath)" } else { "no toolset under $msvcRoot" })
    $toolsetDir = if ($best) { Join-Path $msvcRoot $best.ToString() } else { $null }
}

$sdkRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Include'
$sdkVersions = @(Get-ChildItem $sdkRoot -Directory 2>$null |
    ForEach-Object { try { [version]$_.Name } catch { $null } } |
    Where-Object { $_ -ne $null } | Sort-Object -Descending)
$sdk = $sdkVersions | Select-Object -First 1
Report $(if ($sdk -and $sdk -ge [version]'10.0.20348.0') { 'OK' } else { 'FAIL' }) `
    'Windows SDK >= 20348' $(if ($sdk) { "$sdk" } else { "not found under $sdkRoot" })

if ($Arch -eq 'arm64') {
    $armCl = if ($toolsetDir) { Join-Path $toolsetDir 'bin\Hostx64\arm64\cl.exe' } else { '' }
    Report $(if ($armCl -and (Test-Path $armCl)) { 'OK' } else { 'FAIL' }) `
        'MSVC ARM64 cross tools' $(if ($armCl) { $armCl } else { 'no toolset' })
}

# --- vcpkg resolution ------------------------------------------------------
# Mirrors build.cmd :resolve_vcpkg_installed_dir and CMakeLists.txt:60-95:
#   VCPKG_INSTALLED_DIR env -> VCPKG_INSTALLATION_ROOT -> VCPKG_ROOT
#   -> .vcpkgroot (bootstrap pin) -> D:\vcpkg -> C:\vcpkg
$pinPath = Join-Path $root '.vcpkgroot'
$pin = $null
if (Test-Path $pinPath) { $pin = (Get-Content $pinPath -TotalCount 1).Trim() }
$pinnedDir = if ($pin) { Join-Path $pin "installed\$triplet" } else { $null }

$probe = @()
if ($InstalledDir) { $probe += ,@($InstalledDir, '-InstalledDir (simulates VCPKG_INSTALLED_DIR)') }
if ($env:VCPKG_INSTALLED_DIR) { $probe += ,@($env:VCPKG_INSTALLED_DIR, 'VCPKG_INSTALLED_DIR env') }
if ($env:VCPKG_INSTALLATION_ROOT) { $probe += ,@((Join-Path $env:VCPKG_INSTALLATION_ROOT "installed\$triplet"), 'VCPKG_INSTALLATION_ROOT env') }
if ($env:VCPKG_ROOT) { $probe += ,@((Join-Path $env:VCPKG_ROOT "installed\$triplet"), 'VCPKG_ROOT env') }
if ($VcpkgRoot) { $probe += ,@((Join-Path $VcpkgRoot "installed\$triplet"), '-VcpkgRoot (bootstrap selected root)') }
if ($pinnedDir) { $probe += ,@($pinnedDir, '.vcpkgroot (bootstrap pin)') }
$probe += ,@("D:\vcpkg\installed\$triplet", 'D:/vcpkg fallback')
$probe += ,@("C:\vcpkg\installed\$triplet", 'C:/vcpkg fallback')

$installed = $null
$resolvedBy = $null
foreach ($p in $probe) {
    $c = $p[0]
    if ($c -and (Test-Path (Join-Path $c 'include')) -and (Test-Path (Join-Path $c 'lib'))) {
        $installed = $c
        $resolvedBy = $p[1]
        break
    }
}
if (-not $installed) {
    Report 'FAIL' 'vcpkg installed tree' "no resolvable tree; run: .\scripts\bootstrap.ps1 -Arch $Arch"
    $manifestDir = Join-Path $root "vcpkg_installed\$triplet"
    if (Test-Path (Join-Path $manifestDir 'lib')) {
        Report 'WARN' 'manifest tree unused' "project-local vcpkg_installed\$triplet is complete but will not be resolved"
    }
} else {
    Report 'OK' 'resolved via' "$resolvedBy"
    $leaf = Split-Path $installed -Leaf
    Report $(if ($leaf -eq $triplet) { 'OK' } else { 'FAIL' }) 'triplet name match' "$leaf (expected $triplet) -> $installed"
    $manifestDir = Join-Path $root "vcpkg_installed\$triplet"
    if ((Test-Path (Join-Path $manifestDir 'lib')) -and ($installed -ne $manifestDir)) {
        Report 'WARN' 'manifest tree unused' "complete vcpkg_installed\$triplet exists but is not what gets resolved"
    }

    $headers = @('zlib.h', 'nlohmann\json.hpp', 'kissfft\kiss_fft.h', 'ixwebsocket\IXWebSocket.h')
    $libs = @('z.lib', 'mbedtls.lib', 'mbedx509.lib', 'mbedcrypto.lib', 'kissfft-float.lib', 'ixwebsocket.lib')
    $dlls = @('z.dll', 'kissfft-float.dll')
    foreach ($h in $headers) {
        Report $(if (Test-Path (Join-Path $installed "include\$h")) { 'OK' } else { 'FAIL' }) "include/$h" ''
    }
    foreach ($l in $libs) {
        Report $(if (Test-Path (Join-Path $installed "lib\$l")) { 'OK' } else { 'FAIL' }) "lib/$l" ''
    }
    foreach ($d in $dlls) {
        Report $(if (Test-Path (Join-Path $installed "bin\$d")) { 'OK' } else { 'FAIL' }) "bin/$d" 'POST_BUILD copy_if_different requires this'
    }
    $debugDll = Join-Path $installed 'debug\bin\kissfft-float.dll'
    Report $(if (Test-Path $debugDll) { 'OK' } else { 'WARN' }) 'debug/bin/kissfft-float.dll' 'needed only for Debug builds'
}

if (-not $pin) {
    Report 'WARN' '.vcpkgroot pin' 'not written yet; run .\scripts\bootstrap.ps1 to make the build independent of machine env'
} else {
    Report 'OK' '.vcpkgroot pin' $pin
}

# --- Repo-local contracts --------------------------------------------------
$submoduleHeader = Join-Path $root 'third_party\ixwebsocket\ixwebsocket\IXWebSocket.h'
$submoduleOk = Test-Path $submoduleHeader
Report $(if ($submoduleOk) { 'OK' } else { 'WARN' }) 'ixwebsocket submodule' `
    $(if ($submoduleOk) { 'source build path (local)' } else { 'empty -> falls back to vcpkg prebuilt lib (CI path)' })

$httplib = Join-Path $root 'third_party\httplib\httplib.h'
Report $(if (Test-Path $httplib) { 'OK' } else { 'FAIL' }) 'third_party/httplib.h' ''

# CRT DLLs are no longer vendored in the repo; both CI and local builds read
# them from the build machine's own VS Redist via copy_vc_runtime.ps1.
$crtScript = Join-Path $root 'scripts\copy_vc_runtime.ps1'
$crtDir = $null
$pfx86 = [Environment]::GetEnvironmentVariable('ProgramFiles(x86)')
$vswhere = if ($pfx86) { Join-Path $pfx86 'Microsoft Visual Studio\Installer\vswhere.exe' } else { $null }
if ($vswhere -and (Test-Path -LiteralPath $vswhere)) {
    foreach ($vs in (& $vswhere -products * -property installationPath 2>$null)) {
        if (-not $vs) { continue }
        $redistRoot = Join-Path $vs 'VC\Redist\MSVC'
        if (-not (Test-Path -LiteralPath $redistRoot)) { continue }
        foreach ($ver in (Get-ChildItem -LiteralPath $redistRoot -Directory -ErrorAction SilentlyContinue)) {
            foreach ($sub in @((Join-Path $ver.FullName "$Arch\Microsoft.VC143.CRT"),
                              (Join-Path $ver.FullName "onecore\$Arch\Microsoft.VC143.CRT"))) {
                if (Test-Path -LiteralPath $sub) { $crtDir = $sub; break }
            }
            if ($crtDir) { break }
        }
        if ($crtDir) { break }
    }
}
Report $(if ($crtDir) { 'OK' } else { 'WARN' }) 'VS Redist CRT source' `
    $(if ($crtDir) { "$crtDir (no vendored DLLs)" } else { "no $Arch CRT found; CI step would fail - install VC++ ARM64 build tools" })
Report $(if (Test-Path -LiteralPath $crtScript) { 'OK' } else { 'FAIL' }) 'copy_vc_runtime.ps1' `
    $(if (Test-Path -LiteralPath $crtScript) { 'CI + local share this resolver' } else { 'missing' })

if ($env:VCPKG_INSTALLED_DIR) {
    $envLeaf = Split-Path $env:VCPKG_INSTALLED_DIR -Leaf
    Report $(if ($envLeaf -eq $triplet) { 'OK' } else { 'FAIL' }) 'VCPKG_INSTALLED_DIR scope' `
        $(if ($envLeaf -eq $triplet) { 'matches current arch' } else { "pinned to $envLeaf; will NOT switch for other architectures" })
}

$py = & python --version 2>&1 | Select-Object -First 1
Report $(if ($py -and "$py" -match 'Python') { 'OK' } else { 'WARN' }) 'Python (packaging)' `
    $(if ($py) { "$py" } else { 'needed only for build.cmd release all' })

Write-Output ('')
if ($fail -gt 0) {
    Write-Output ("PREFLIGHT FAILED: $fail hard check(s), $warn warning(s).")
    Write-Output ("Fix the FAIL lines above, then: .\scripts\bootstrap.ps1 -Arch $Arch -Build")
    exit 1
}
Write-Output ("PREFLIGHT OK ($warn warning(s)).")
exit 0
