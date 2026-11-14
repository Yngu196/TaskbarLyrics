# Single source of truth for the vcpkg dependency contract.
# ASCII-only output on purpose: Windows PowerShell 5.1 mangles non-ASCII text.
# Works with Windows PowerShell 5.1 and PowerShell 7+.
#
# Dot-source from bootstrap.ps1 / check_build_env.ps1:
#   . (Join-Path $PSScriptRoot 'vcpkg_contract.ps1')
#
# Two different kinds of declaration live here:
#
#   * Package NAMES come from vcpkg.json "dependencies" and nowhere else.
#     Adding/removing a port is a ONE-FILE change. bootstrap.ps1 installs them
#     with --classic (vcpkg runs with the repo as cwd, where vcpkg.json exists;
#     without --classic vcpkg switches to manifest mode and creates
#     vcpkg_installed/, which this project's find_library paths do not read).
#
#   * The REQUIRED FILE list is a file-level contract: vcpkg port names do not
#     tell you the exact header/lib/dll paths their integration produces, so it
#     cannot be derived automatically. It is declared ONCE here instead of
#     being duplicated in the installer verification and the read-only
#     preflight. These are exactly the paths CMakeLists.txt consumes via
#     find_path / find_library / POST_BUILD copy.
#
# Not a declaration, and deliberately NOT moved here:
#   * CMakeLists.txt find_* calls are the consumer side, not a second manifest.
#   * vcpkg.json "version" is a separate sync chain against project(VERSION).
#   * Path resolution order is owned by build.cmd / CMakeLists.txt /
#     bootstrap.ps1 / check_build_env.ps1 (four thin implementations); this
#     module only supplies data, it never picks a tree.
#
# Future manifest-mode caveat: if the project ever switches to manifest mode,
# the "ixwebsocket" dependency must be dropped from vcpkg.json first, because
# CMake add_subdirectory(third_party/ixwebsocket) already defines an
# ixwebsocket::ixwebsocket target and the two would collide.

function Get-VcpkgPackages {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepoRoot
    )

    # Callers may run with $ErrorActionPreference = 'SilentlyContinue'
    # (check_build_env.ps1 does). Force errors to surface for this block only.
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Stop'
    try {
        $manifest = Join-Path $RepoRoot 'vcpkg.json'
        if (-not (Test-Path -LiteralPath $manifest)) {
            throw "vcpkg.json not found at $manifest"
        }
        try {
            $parsed = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
        } catch {
            # ConvertFrom-Json messages are localized by the .NET runtime and
            # would break this script family's ASCII-only output on PS 5.1.
            throw "vcpkg.json is not valid JSON at $manifest (parse failed)."
        }
        $deps = @($parsed.dependencies)
        if ($deps.Count -eq 0) {
            throw 'vcpkg.json declares no dependencies'
        }
        foreach ($d in $deps) {
            # vcpkg.json also allows { "name": ..., "features": [...] } objects.
            # Classic mode installs plain port names only, so reject those
            # explicitly instead of passing a PSCustomObject to vcpkg.exe.
            if ($d -isnot [string] -or [string]::IsNullOrWhiteSpace($d)) {
                throw "vcpkg.json dependencies must be plain port names (got: '$d'). Feature objects are not supported in classic mode."
            }
        }
        return $deps
    } finally {
        $ErrorActionPreference = $previous
    }
}

function Get-VcpkgRequiredFiles {
    # Path is relative to installed/<triplet>. Display/Detail are used by the
    # preflight; the installer only needs Path. Keep the two lists in sync by
    # having exactly one list.
    return @(
        [pscustomobject]@{ Path = 'include\zlib.h';                  Detail = '' }
        [pscustomobject]@{ Path = 'include\nlohmann\json.hpp';       Detail = '' }
        [pscustomobject]@{ Path = 'include\kissfft\kiss_fft.h';      Detail = '' }
        [pscustomobject]@{ Path = 'include\ixwebsocket\IXWebSocket.h'; Detail = '' }
        [pscustomobject]@{ Path = 'lib\z.lib';                       Detail = '' }
        [pscustomobject]@{ Path = 'lib\mbedtls.lib';                 Detail = '' }
        [pscustomobject]@{ Path = 'lib\mbedx509.lib';                Detail = '' }
        [pscustomobject]@{ Path = 'lib\mbedcrypto.lib';              Detail = '' }
        [pscustomobject]@{ Path = 'lib\kissfft-float.lib';           Detail = '' }
        [pscustomobject]@{ Path = 'lib\ixwebsocket.lib';             Detail = '' }
        [pscustomobject]@{ Path = 'bin\z.dll';                       Detail = 'POST_BUILD copy_if_different requires this' }
        [pscustomobject]@{ Path = 'bin\kissfft-float.dll';           Detail = 'POST_BUILD copy_if_different requires this' }
    )
}

function Get-VcpkgOptionalFiles {
    # Missing these is a WARN, not a FAIL: they are only needed for Debug
    # builds or for a specific vcpkg port layout.
    return @(
        [pscustomobject]@{ Path = 'debug\bin\kissfft-float.dll'; Detail = 'needed only for Debug builds' }
    )
}
