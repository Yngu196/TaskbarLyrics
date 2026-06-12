# ============================================================
#  MoeKoeMusic Taskbar Lyrics - Release Package Script
#
#  Packages built artifacts + plugin source into a zip that
#  matches MoeKoeMusic-Plugins review workflow expectations.
#
#  Required output structure (public/ root prefix):
#
#     moeKoe-taskbar-lyrics.zip
#     `-- public/
#         |-- manifest.json
#         |-- background.js
#         |-- popup.js
#         |-- popup.html
#         |-- native-bridge.html      (Native Host 桥接页)
#         |-- native-bridge.js        (Native Host 桥接逻辑)
#         |-- icons/
#         |   `-- icon256.png
#         `-- MoeKoeTaskbarLyrics.exe
#
#  Usage:
#     .\scripts\package.ps1 [-ExeDir <dir>] [-Output <path>]
# ============================================================

param(
    [string]$ExeDir = "",
    [string]$Output = ""
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$PluginDir  = Join-Path $ProjectRoot "moeKoe-taskbar-lyrics"

if ([string]::IsNullOrWhiteSpace($ExeDir)) {
    $ExeDir = Join-Path $ProjectRoot "build\Release"
}
if ([string]::IsNullOrWhiteSpace($Output)) {
    $Output = Join-Path $ProjectRoot "moeKoe-taskbar-lyrics.zip"
}

$exePath = Join-Path $ExeDir "MoeKoeTaskbarLyrics.exe"
if (-not (Test-Path $exePath)) {
    Write-Host "[ERROR] Build artifact not found: $exePath" -ForegroundColor Red
    Write-Host "        Run build.cmd release first, or specify -ExeDir" -ForegroundColor Red
    exit 1
}
if (-not (Test-Path $PluginDir)) {
    Write-Host "[ERROR] Plugin directory not found: $PluginDir" -ForegroundColor Red
    exit 1
}

Write-Host "============================================"
Write-Host "  Taskbar Lyrics - Release Packaging"
Write-Host "============================================"
Write-Host ""
Write-Host "[INFO] Project : $ProjectRoot"
Write-Host "[INFO] Exe dir  : $ExeDir"
Write-Host "[INFO] Output   : $Output"
Write-Host ""

$TempDir = Join-Path $env:TEMP ("moekoe-package-" + [guid]::NewGuid().ToString("N"))

try {
    $PublicDir = Join-Path $TempDir "public"
    New-Item -ItemType Directory -Force -Path $PublicDir | Out-Null
    New-Item -ItemType Directory -Force (Join-Path $PublicDir "icons") | Out-Null

    # Copy plugin source files
    $pluginFiles = @("manifest.json", "background.js", "popup.js", "popup.html",
                      "native-bridge.html", "native-bridge.js")
    foreach ($f in $pluginFiles) {
        $src = Join-Path $PluginDir $f
        if (Test-Path $src) {
            Copy-Item -Force $src (Join-Path $PublicDir $f)
            Write-Host "  [OK] $f"
        } else {
            Write-Host "  [WARN] Missing: $f, skipping" -ForegroundColor Yellow
        }
    }

    # Copy icons
    $iconSrc = Join-Path $PluginDir "icons"
    if (Test-Path $iconSrc) {
        Copy-Item -Recurse -Force $iconSrc (Join-Path $PublicDir "icons")
        Write-Host "  [OK] icons/"
    } else {
        Write-Host "  [WARN] Missing: icons/" -ForegroundColor Yellow
    }

    # Copy compiled exe
    Copy-Item -Force $exePath (Join-Path $PublicDir "MoeKoeTaskbarLyrics.exe")
    Write-Host "  [OK] MoeKoeTaskbarLyrics.exe"

    # Copy DLLs if present
    foreach ($dll in @("WebView2Loader.dll", "z.dll")) {
        $dllPath = Join-Path $ExeDir $dll
        if (Test-Path $dllPath) {
            Copy-Item -Force $dllPath (Join-Path $PublicDir $dll)
            Write-Host "  [OK] $dll"
        }
    }

    # Create zip
    Write-Host ""
    Write-Host "[INFO] Compressing..."

    $OutParent = Split-Path -Parent $Output
    if ($OutParent -and -not (Test-Path $OutParent)) {
        New-Item -ItemType Directory -Force $OutParent | Out-Null
    }

    Compress-Archive -Path "$PublicDir\*" -DestinationPath $Output -Force

    if (Test-Path $Output) {
        $sizeKB = [math]::Round((Get-Item $Output).Length / 1KB, 1)
        Write-Host ""
        Write-Host "[SUCCESS] Package created!" -ForegroundColor Green
        Write-Host "          File : $Output"
        Write-Host "          Size : ${sizeKB} KB"
        Write-Host ""
        Write-Host "  Zip contents:"
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        [System.IO.Compression.ZipFile]::OpenRead($Output).Entries |
            ForEach-Object { Write-Host ("            " + $_.FullName) }
    } else {
        Write-Host "[ERROR] Compression failed" -ForegroundColor Red
        exit 1
    }
} finally {
    if (Test-Path $TempDir) {
        Remove-Item -Recurse -Force $TempDir | Out-Null
    }
}
