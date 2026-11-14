<#
.SYNOPSIS
    Ensure the VC++ CRT redistributable DLLs are present in a build output directory.

.DESCRIPTION
    Looks for Microsoft.VC143.CRT in installed Visual Studio instances via
    vswhere (plus VSINSTALLDIR / VCToolsRedistDir / common install roots),
    verifies each DLL's PE machine type, and copies only DLLs that are missing
    or have the wrong architecture.

    When the build machine has no matching retail CRT (notably ARM64 on x64 CI
    runners), the DLLs come from Microsoft's official VC++ Redistributable
    package instead: the Burn bundle's attached CAB is located and expanded
    with the Windows built-in expand.exe, and only DLLs with the correct PE
    machine are copied. No Microsoft binaries are stored in this repository.
    Microsoft permits app-local deployment of these redistributable DLLs (see
    "Determine which DLLs to redistribute"); central deployment is preferred
    but would not put the DLLs into the plugin package.

    A missing CRT is always a hard error, never a silently incomplete package.
    Compatible with Windows PowerShell 5.1 and PowerShell 7.

.EXAMPLE
    .\scripts\copy_vc_runtime.ps1 -Arch x64 -Destination "moeKoe-taskbar-lyrics\x64"
    .\scripts\copy_vc_runtime.ps1 -Arch arm64 -Destination "moeKoe-taskbar-lyrics\arm64" -VerifyOnly
    .\scripts\copy_vc_runtime.ps1 -Arch arm64 -Destination "..." -RedistPackage C:\temp\vc_redist.arm64.exe
#>
[CmdletBinding()]
param(
    [ValidateSet('x64', 'arm64')]
    [string]$Arch = 'x64',

    [Parameter(Mandatory = $true)]
    [string]$Destination,

    # Only check, never write. Useful as a CI preflight; skips the download.
    [switch]$VerifyOnly,

    # Offline use: an already downloaded official redistributable package.
    [string]$RedistPackage,

    # Skip local VS Redist lookup and always use the official package. Useful
    # for reproducible CRT versions and for exercising the fallback locally.
    [switch]$OfficialOnly
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

# aka.ms permalinks always serve the latest supported v14 redistributable, so
# the payload is newer than (or equal to) any VS 2015-2026 toolset and stays
# binary compatible. The trade-off is that the exact CRT version is not pinned.
$redistUrls = @{
    x64   = 'https://aka.ms/vc14/vc_redist.x64.exe'
    arm64 = 'https://aka.ms/vc14/vc_redist.arm64.exe'
}

function Test-CabFile {
    param([string]$Path)
    try {
        $stream = [System.IO.File]::OpenRead($Path)
        try {
            $buf = New-Object byte[] 4
            if ($stream.Read($buf, 0, 4) -ne 4) { return $false }
            return ([System.Text.Encoding]::ASCII.GetString($buf) -eq 'MSCF')
        } finally { $stream.Dispose() }
    } catch { return $false }
}

function Invoke-CabExpand {
    param([string]$CabPath, [string]$OutDir)
    $expand = Join-Path $env:WINDIR 'System32\expand.exe'
    if (-not (Test-Path -LiteralPath $expand -PathType Leaf)) {
        Say "[WARN] expand.exe not found: $expand"
        return $false
    }
    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
    & $expand '-F:*' $CabPath $OutDir | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Say "[WARN] expand.exe failed ($LASTEXITCODE) for $CabPath"
        return $false
    }
    return $true
}

function Get-LargestEmbeddedCab {
    param([string]$ExePath, [string]$OutPath)
    $bytes = [System.IO.File]::ReadAllBytes($ExePath)
    # ISO-8859-1 is a 1:1 byte<->char mapping, so string offsets equal file
    # offsets. It exists on both .NET Framework (PS 5.1) and .NET (PS 7).
    $text = [System.Text.Encoding]::GetEncoding(28591).GetString($bytes)
    $bestOffset = -1
    $bestSize = 0
    $pos = 0
    while ($pos -lt $text.Length -and ($idx = $text.IndexOf('MSCF', $pos, [StringComparison]::Ordinal)) -ge 0) {
        if ($idx + 16 -le $bytes.Length) {
            $size = [BitConverter]::ToInt32($bytes, $idx + 8)
            if ($size -gt 0 -and ($idx + $size) -le $bytes.Length -and $size -gt $bestSize) {
                $bestOffset = $idx
                $bestSize = $size
            }
        }
        $pos = $idx + 4
    }
    if ($bestOffset -lt 0) {
        Say "[WARN] no embedded CAB container found in $ExePath"
        return $null
    }
    $cab = New-Object byte[] $bestSize
    [Array]::Copy($bytes, $bestOffset, $cab, 0, $bestSize)
    [System.IO.File]::WriteAllBytes($OutPath, $cab)
    return $OutPath
}

function Add-CrtFromOfficialPackage {
    param(
        [string[]]$Dlls,
        [int]$Machine,
        [string]$Arch,
        [string]$Dest,
        [string]$LocalPackage
    )
    $work = Join-Path ([System.IO.Path]::GetTempPath()) ('mkk-crt-' + [guid]::NewGuid().ToString('N'))
    # Invoke-WebRequest draws a full progress bar into the CI log otherwise.
    $oldProgress = $ProgressPreference
    $ProgressPreference = 'SilentlyContinue'
    try {
        New-Item -ItemType Directory -Path $work -Force | Out-Null
        $package = Join-Path $work 'vc_redist.exe'
        if ($LocalPackage) {
            if (-not (Test-Path -LiteralPath $LocalPackage -PathType Leaf)) {
                Say "[WARN] -RedistPackage not found: $LocalPackage"
                return $false
            }
            Copy-Item -LiteralPath $LocalPackage -Destination $package
            Say "Official redist package (local): $LocalPackage"
        } else {
            $url = $redistUrls[$Arch]
            if (-not $url) { Say "[WARN] no official redist URL for $Arch"; return $false }
            Say "Downloading official VC++ redistributable: $url"
            try {
                Invoke-WebRequest -Uri $url -OutFile $package -UseBasicParsing -TimeoutSec 180 -ErrorAction Stop
            } catch {
                Say "[WARN] download failed: $($_.Exception.Message)"
                return $false
            }
        }
        if (-not (Test-Path -LiteralPath $package -PathType Leaf) -or (Get-Item -LiteralPath $package).Length -lt 1MB) {
            Say '[WARN] redist package missing or implausibly small'
            return $false
        }
        $head = New-Object byte[] 2
        $fs = [System.IO.File]::OpenRead($package)
        try { [void]$fs.Read($head, 0, 2) } finally { $fs.Dispose() }
        if ([System.Text.Encoding]::ASCII.GetString($head) -ne 'MZ') {
            Say '[WARN] redist package is not a PE executable'
            return $false
        }

        # Burn bundle layout: PE stub -> UX container (CAB) -> attached
        # container (CAB) holding the MSI and the payload CAB. The attached
        # container is the largest CAB in the file.
        $attached = Join-Path $work 'attached.cab'
        if (-not (Get-LargestEmbeddedCab -ExePath $package -OutPath $attached)) { return $false }
        $stage1 = Join-Path $work 'stage1'
        if (-not (Invoke-CabExpand -CabPath $attached -OutDir $stage1)) { return $false }
        # The x64 bundle carries several payload CABs (x64, ARM64, ...); the
        # largest one is not necessarily the target architecture. Expand them
        # all and let PE machine checks pick the right files.
        $innerCabs = @(Get-ChildItem -LiteralPath $stage1 -File -ErrorAction SilentlyContinue |
            Where-Object { Test-CabFile $_.FullName })
        if ($innerCabs.Count -eq 0) { Say '[WARN] payload CAB not found in attached container'; return $false }
        $payload = New-Object System.Collections.Generic.List[string]
        $index = 0
        foreach ($cab in $innerCabs) {
            $stage = Join-Path $work "payload_$index"
            if (Invoke-CabExpand -CabPath $cab.FullName -OutDir $stage) {
                foreach ($f in (Get-ChildItem -LiteralPath $stage -File -ErrorAction SilentlyContinue)) {
                    $payload.Add($f.FullName)
                }
            }
            $index++
        }

        foreach ($dll in $Dlls) {
            # MSI payload names are suffixed, e.g. msvcp140.dll_arm64. Pick by
            # prefix AND PE machine: vcruntime140_1.dll_arm64 in the ARM64
            # package is in fact an x64 binary, and the x64 bundle contains
            # both x64 and ARM64 payloads.
            $src = $null
            foreach ($path in $payload) {
                $name = [System.IO.Path]::GetFileName($path)
                if (-not $name.StartsWith($dll, [StringComparison]::OrdinalIgnoreCase)) { continue }
                if ((Get-PeMachine $path) -eq $Machine) { $src = $path; break }
            }
            if (-not $src) {
                Say ("[WARN] official package has no {0} with machine 0x{1:X4}" -f $dll, $Machine)
                return $false
            }
            $to = Join-Path $Dest $dll
            Copy-Item -LiteralPath $src -Destination $to -Force
            $actual = Get-PeMachine $to
            if ($actual -ne $Machine) {
                Say ("[WARN] copied {0} but PE machine is 0x{1:X4}" -f $dll, $actual)
                return $false
            }
            Say ("  copied {0,-24} machine=0x{1:X4}  (official redist)" -f $dll, $actual)
        }
        return $true
    } catch {
        Say "[WARN] official redist fallback failed: $($_.Exception.Message)"
        return $false
    } finally {
        $ProgressPreference = $oldProgress
        Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
    }
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
    Fail "CRT DLLs missing/wrong in $dest (VerifyOnly; no files written, no package downloaded)"
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
if ($OfficialOnly) {
    Say "Local VS Redist lookup skipped (-OfficialOnly)"
    $ordered = @()
}

Say "Candidate Redist directories searched:"
foreach ($c in $ordered) { Say ("  - {0}" -f $c.Path) }
if ($ordered.Count -eq 0) { Say "  (none found)" }

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

if ($source) {
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
} else {
    if ($VerifyOnly) {
        Fail "CRT DLLs missing/wrong in $dest, and no local VS CRT matched (VerifyOnly; no files written)"
    }
    Say "No local VS CRT matched; falling back to Microsoft's official redistributable."
    if (-not (Add-CrtFromOfficialPackage -Dlls $missing -Machine $expectedMachine -Arch $Arch -Dest $dest -LocalPackage $RedistPackage)) {
        $msg = @"
No VC++ CRT for $Arch could be resolved.
Local Visual Studio Redist candidates: $($ordered.Count) (listed above).
Official redistributable fallback also failed: $($redistUrls[$Arch]).
The package is fetched from aka.ms and expanded with the Windows built-in
expand.exe; check network access, or pass -RedistPackage with a local copy.
"@
        Fail $msg
    }
}

Say "OK: VC++ CRT ready in $dest"
$global:LASTEXITCODE = 0
