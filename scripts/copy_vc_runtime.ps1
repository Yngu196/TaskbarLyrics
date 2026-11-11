<#
.SYNOPSIS
    Ensure the VC++ CRT redistributable DLLs are present in a build output directory.

.DESCRIPTION
    Locates Microsoft.VC143.CRT inside an installed Visual Studio 2022 using
    vswhere (plus VSINSTALLDIR / VCToolsRedistDir / common install roots),
    verifies each DLL's PE machine type matches the target architecture, and
    copies only the DLLs that are missing or have the wrong architecture.

    This replaces the old ci/redist fallback. No Microsoft binaries are stored
    in this repository: every DLL comes from the build machine's own Visual
    Studio installation, which is the source Microsoft licenses for
    redistribution with an application. Failing to find one is a hard error,
    never a silently incomplete package.

    Compatible with Windows PowerShell 5.1 and PowerShell 7.

.EXAMPLE
    .\scripts\copy_vc_runtime.ps1 -Arch x64 -Destination "moeKoe-taskbar-lyrics\x64"
    .\scripts\copy_vc_runtime.ps1 -Arch arm64 -Destination "moeKoe-taskbar-lyrics\arm64" -VerifyOnly
#>
[CmdletBinding()]
param(
    [ValidateSet('x64', 'arm64')]
    [string]$Arch = 'x64',

    [Parameter(Mandatory = $true)]
    [string]$Destination,

    # Only check, never write. Useful as a CI preflight.
    [switch]$VerifyOnly
)

$ErrorActionPreference = 'Stop'

# PowerShell 7.3+ turns a non-zero native exit code into a terminating error
# when $ErrorActionPreference is 'Stop'. vswhere is only a hint here - its
# failure must not abort the search before the other install roots are tried.
# The variable does not exist in Windows PowerShell 5.1, hence Get-Variable.
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

$machineOf = @{ x64 = 0x8664; arm64 = 0xAA64 }
$expectedMachine = $machineOf[$Arch]
$expectedName = if ($Arch -eq 'arm64') { 'ARM64' } else { 'AMD64' }

# vcruntime140_1.dll is only linked by x64 / ARM64EC targets. On a native
# ARM64 build the file shipped in VS's arm64 Redist folder is itself an x64
# binary, so copying it would break scripts/verify_package.py.
$needed = @('msvcp140.dll', 'vcruntime140.dll')
if ($Arch -ne 'arm64') { $needed += 'vcruntime140_1.dll' }

function Get-PeMachine {
    param([string]$Path)
    try {
        $stream = [System.IO.File]::OpenRead($Path)
        try {
            $buf = New-Object byte[] 512
            $n = $stream.Read($buf, 0, 512)
            if ($n -lt 0x100) { return $null }
            $peOffset = [BitConverter]::ToInt32($buf, 0x3C)
            if ($peOffset -lt 0 -or ($peOffset + 6) -gt $n) { return $null }
            # e_lfanew points at the "PE\0\0" signature; the COFF Machine
            # field follows 4 bytes later.
            if ($buf[$peOffset] -ne 0x50 -or $buf[$peOffset + 1] -ne 0x45) { return $null }
            return [BitConverter]::ToUInt16($buf, $peOffset + 4)
        } finally {
            $stream.Dispose()
        }
    } catch {
        return $null
    }
}

function Test-CrtDir {
    param([string]$Dir, [string[]]$Dlls, [int]$Machine)
    foreach ($dll in $Dlls) {
        $candidate = Join-Path $Dir $dll
        if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) { return $false }
        if ((Get-PeMachine $candidate) -ne $Machine) { return $false }
    }
    return $true
}

function Say { param([string]$Message) Write-Host $Message }

function Write-GitHubDiagnostic {
    param([string]$Message)
    if ($env:GITHUB_ACTIONS -ne 'true') { return }
    # Workflow commands require %, CR and LF to be escaped. Publishing the
    # reason as an annotation makes it readable through the anonymous
    # check-runs/annotations API, not only through admin-only job logs.
    $oneLine = ($Message -replace '%', '%25' -replace "`r", '%0D' -replace "`n", '%0A')
    Write-Host "::error::$oneLine"
    if ($env:GITHUB_STEP_SUMMARY) {
        $lines = @('### VC++ CRT resolution failed', '', '```text') +
                 @($Message -split "`r?`n") + @('```', '')
        [System.IO.File]::AppendAllText(
            $env:GITHUB_STEP_SUMMARY,
            (($lines -join "`r`n") + "`r`n"))
    }
}

function Fail {
    param([string]$Message)
    Write-GitHubDiagnostic $Message
    # GitHub Actions' PowerShell wrapper can exit with $LASTEXITCODE even when
    # a script throws. Set it explicitly so a missing CRT can never go green.
    $global:LASTEXITCODE = 1
    throw $Message
}

$global:LASTEXITCODE = 0
Say ("VC++ CRT: arch=$Arch expected PE machine=0x{0:X4} ($expectedName)" -f $expectedMachine)
Say "VC++ CRT: DLLs=$($needed -join ', ')"

# --- Destination -----------------------------------------------------------
$dest = if ([System.IO.Path]::IsPathRooted($Destination)) {
    $Destination
} else {
    Join-Path (Get-Location).Path $Destination
}
$dest = [System.IO.Path]::GetFullPath($dest)
if (-not (Test-Path -LiteralPath $dest -PathType Container)) {
    if ($VerifyOnly) { Fail "Destination directory does not exist: $dest" }
    New-Item -ItemType Directory -Path $dest -Force | Out-Null
}

$missing = New-Object System.Collections.Generic.List[string]
foreach ($dll in $needed) {
    $target = Join-Path $dest $dll
    if (-not (Test-Path -LiteralPath $target -PathType Leaf)) {
        $missing.Add($dll)
        continue
    }
    $actual = Get-PeMachine $target
    if ($actual -ne $expectedMachine) {
        Say ("[WARN] $dll in output has machine 0x{0:X4}, expected 0x{1:X4} - will replace" -f $actual, $expectedMachine)
        $missing.Add($dll)
    }
}

if ($missing.Count -eq 0) {
    Say "OK: all CRT DLLs already present in $dest with correct architecture"
    Say "    (normally copied by CMakeLists.txt POST_BUILD from the local VS Redist)"
    $global:LASTEXITCODE = 0
    return
}
Say "Missing or wrong-architecture: $($missing -join ', ')"
if ($VerifyOnly) {
    Fail "CRT DLLs missing/wrong in $dest (VerifyOnly; no files written)"
}

# --- Collect Visual Studio install roots ----------------------------------
$roots = New-Object System.Collections.Generic.List[string]
function Add-Root {
    param([string]$Path)
    if (-not $Path) { return }
    $trimmed = $Path.Trim().TrimEnd('\')
    if ($trimmed -and (Test-Path -LiteralPath $trimmed -PathType Container) -and -not $roots.Contains($trimmed)) {
        $roots.Add($trimmed)
    }
}

$pf86 = [Environment]::GetEnvironmentVariable('ProgramFiles(x86)')
if ($pf86) {
    $vswhere = Join-Path $pf86 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere -PathType Leaf) {
        try {
            $installs = & $vswhere -products * -property installationPath 2>$null
            foreach ($install in $installs) { Add-Root $install }
        } catch {
            Say "[WARN] vswhere lookup failed: $($_.Exception.Message)"
        }
    }
}
Add-Root $env:VSINSTALLDIR
if ($env:VCToolsRedistDir) {
    Add-Root $env:VCToolsRedistDir
    $parent = Split-Path $env:VCToolsRedistDir -Parent
    Add-Root $parent
    Add-Root (Split-Path $parent -Parent)
}
foreach ($common in @(
    'C:\Program Files\Microsoft Visual Studio\2022',
    'C:\Program Files (x86)\Microsoft Visual Studio\2022',
    'D:\VisualStudioCommunity2022',
    'D:\Program Files\Microsoft Visual Studio\2022'
)) { Add-Root $common }

# --- Build an ordered candidate list --------------------------------------
# A root may be: a VS install root, a "VC\Redist\MSVC" folder, or a specific
# version folder. Cover all three shapes, plus the onecore variant.
$candidates = New-Object System.Collections.Generic.List[object]
function Add-VersionChildren {
    param([string]$Base, [int]$Priority)
    if (-not (Test-Path -LiteralPath $Base -PathType Container)) { return }
    foreach ($verDir in (Get-ChildItem -LiteralPath $Base -Directory -ErrorAction SilentlyContinue)) {
        $sortKey = [version]'0.0'
        try { $sortKey = [version]$verDir.Name } catch { }
        foreach ($sub in @(
            (Join-Path $verDir.FullName "$Arch\Microsoft.VC143.CRT"),
            (Join-Path $verDir.FullName "onecore\$Arch\Microsoft.VC143.CRT")
        )) {
            $candidates.Add([pscustomobject]@{ Path = $sub; Version = $sortKey; Priority = $Priority })
        }
    }
}

foreach ($root in $roots) {
    # root itself is a version folder (VCToolsRedistDir often is)
    foreach ($sub in @(
        (Join-Path $root "$Arch\Microsoft.VC143.CRT"),
        (Join-Path $root "onecore\$Arch\Microsoft.VC143.CRT")
    )) {
        $candidates.Add([pscustomobject]@{ Path = $sub; Version = [version]'9999.0'; Priority = 0 })
    }
    Add-VersionChildren (Join-Path $root 'VC\Redist\MSVC') 1
    Add-VersionChildren (Join-Path $root 'Redist\MSVC') 1
    Add-VersionChildren $root 2
}

$ordered = @($candidates |
    Where-Object { Test-Path -LiteralPath $_.Path -PathType Container } |
    Sort-Object -Property @{ Expression = 'Priority' }, @{ Expression = 'Version'; Descending = $true })

Say "Candidate Redist directories searched:"
foreach ($c in $ordered) { Say ("  - {0}" -f $c.Path) }
if ($ordered.Count -eq 0) {
    Say "  (none found)"
    $msg = @"
No VC++ CRT directory for $Arch found on this machine.
Searched VS installs via vswhere, VSINSTALLDIR, VCToolsRedistDir and common
install roots for: VC\Redist\MSVC\<version>\$Arch\Microsoft.VC143.CRT
Install the "MSVC v143 C++ ARM64 build tools" / "C++ Redistributable"
component in Visual Studio Installer, then re-run.
"@
    Fail $msg
}

$source = $null
foreach ($c in $ordered) {
    if (Test-CrtDir -Dir $c.Path -Dlls $missing -Machine $expectedMachine) {
        $source = $c.Path
        break
    }
    $reasons = New-Object System.Collections.Generic.List[string]
    foreach ($dll in $missing) {
        $probe = Join-Path $c.Path $dll
        if (-not (Test-Path -LiteralPath $probe -PathType Leaf)) {
            $reasons.Add("${dll}: missing")
            continue
        }
        $actual = Get-PeMachine $probe
        $reasons.Add(("{0}: machine=0x{1:X4}" -f $dll, $actual))
    }
    Say ("  rejected {0} -> {1}" -f $c.Path, ($reasons -join '; '))
}
if (-not $source) {
    $msg = @"
No VC++ CRT directory for $Arch contains all of [$($missing -join ', ')] as
valid $expectedName PE binaries. Rejected candidates are listed above.
Install the matching VC++ Redistributable / ARM64 build tools in Visual Studio
Installer rather than vendoring DLLs into the repository.
"@
    Fail $msg
}

Say "Source: $source"
foreach ($dll in $missing) {
    $from = Join-Path $source $dll
    $to = Join-Path $dest $dll
    Copy-Item -LiteralPath $from -Destination $to -Force
    $actual = Get-PeMachine $to
    if ($actual -ne $expectedMachine) {
        Fail ("Copied $dll but PE machine is 0x{0:X4}, expected 0x{1:X4}" -f $actual, $expectedMachine)
    }
    Say ("  copied {0,-24} machine=0x{1:X4}" -f $dll, $actual)
}
Say "OK: VC++ CRT ready in $dest"
$global:LASTEXITCODE = 0
