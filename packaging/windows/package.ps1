<#
.SYNOPSIS
    Builds SSH Manager in Release and packages it for Windows.

.DESCRIPTION
    1. configures and builds a Release build (MinGW, Ninja);
    2. installs it into a staging folder (sshmanager.exe, libqtermwidget6.dll,
       libcrypto, kb-layouts, color-schemes) and adds Qt with windeployqt;
    3. checks that every DLL the executables need is in the folder and that
       the application starts;
    4. writes dist\SSHManager-<version>-win64.zip (portable: unzip and run);
    5. if Inno Setup 6 (ISCC.exe) is installed, also builds
       dist\SSHManager-<version>-setup.exe from sshmanager.iss.

    The toolchain layout is the one from the README (aqt): <Root>\6.10.3\mingw_64,
    <Root>\Tools\{mingw1310_64,CMake_64,Ninja,OpenSSLv3}, <Root>\deps\prefix.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File packaging\windows\package.ps1
#>
param(
    # Folder that contains 6.10.3, Tools and deps (default: the parent of the source tree).
    [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path,
    [string]$QtVersion = '6.10.3',
    [string]$BuildDir,
    [string]$OutDir,
    [switch]$NoInstaller
)

$ErrorActionPreference = 'Stop'
$source = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $BuildDir) { $BuildDir = Join-Path $Root 'build-release' }
if (-not $OutDir) { $OutDir = Join-Path $Root 'dist' }

$qt = Join-Path $Root "$QtVersion\mingw_64"
$prefix = Join-Path $Root 'deps\prefix'
$openssl = Join-Path $Root 'Tools\OpenSSLv3\Win_x64'
$mingw = Join-Path $Root 'Tools\mingw1310_64\bin'
foreach ($path in @($qt, $prefix, $openssl, $mingw)) {
    if (-not (Test-Path $path)) { throw "Missing $path (see README, Windows section)" }
}
$env:PATH = "$mingw;$Root\Tools\CMake_64\bin;$Root\Tools\Ninja;$qt\bin;$env:PATH"

# Version from project(... VERSION x.y.z) in CMakeLists.txt.
$cmakeLists = Get-Content (Join-Path $source 'CMakeLists.txt') -Raw
if ($cmakeLists -notmatch 'project\(\s*sshmanager\s+VERSION\s+([0-9.]+)') { throw 'Version not found in CMakeLists.txt' }
$version = $Matches[1]
Write-Host "SSH Manager $version" -ForegroundColor Cyan

function Invoke-Native([string]$exe, [string[]]$arguments) {
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) { throw "$exe failed with exit code $LASTEXITCODE" }
}

# 1. Release build.
Write-Host '== Build' -ForegroundColor Cyan
Invoke-Native cmake @('-S', $source, '-B', $BuildDir, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
    "-DCMAKE_PREFIX_PATH=$qt;$prefix", "-DOPENSSL_ROOT_DIR=$openssl")
Invoke-Native cmake @('--build', $BuildDir)

# 2. Staging folder: install rules, then Qt.
Write-Host '== Staging' -ForegroundColor Cyan
$installRoot = Join-Path $BuildDir 'package-root'
if (Test-Path $installRoot) { Remove-Item $installRoot -Recurse -Force }
Invoke-Native cmake @('--install', $BuildDir, '--prefix', $installRoot, '--strip')
$stage = Join-Path $installRoot 'bin'
$exe = Join-Path $stage 'sshmanager.exe'

Invoke-Native (Join-Path $qt 'bin\windeployqt.exe') @('--release', '--compiler-runtime',
    '--no-opengl-sw', '--no-system-d3d-compiler', '--translations', 'it,en', $exe)

# Licences: the project, QTermWidget, and a note on the other components.
$licenses = Join-Path $stage 'licenses'
New-Item -ItemType Directory -Force $licenses | Out-Null
Copy-Item (Join-Path $source 'LICENSE') (Join-Path $stage 'LICENSE.txt')
$qtwSource = Join-Path $Root 'qtermwidget'
foreach ($name in @('LICENSE', 'LICENSE.BSD-3-clause', 'LICENSE.LGPL2+')) {
    $file = Join-Path $qtwSource $name
    if (Test-Path $file) { Copy-Item $file (Join-Path $licenses "qtermwidget-$name.txt") }
}
@"
SSH Manager $version - componenti di terze parti / third-party components

- Qt $QtVersion (LGPL v3): https://www.qt.io/licensing/
  Le librerie Qt sono collegate dinamicamente (Qt6*.dll) e possono essere sostituite.
- QTermWidget (GPL v2 o successiva, con parti BSD-3-Clause e LGPL 2+), con le modifiche
  per Windows in patches/qtermwidget del codice sorgente: vedi qtermwidget-*.txt.
- OpenSSL 3 (Apache License 2.0): https://www.openssl.org/source/license.html
- Runtime MinGW-w64 / GCC (libgcc, libstdc++, winpthreads): GPL con eccezione per il runtime.

Il client OpenSSH non e' incluso: viene usato quello di Windows
(C:\Windows\System32\OpenSSH\ssh.exe, funzionalita' facoltativa "Client OpenSSH").
"@ | Out-File -Encoding utf8 (Join-Path $licenses 'THIRD-PARTY.txt')

# 3. Checks: every imported DLL is either in the folder or part of Windows,
#    and the application starts (offscreen, so nothing appears on screen).
Write-Host '== Checks' -ForegroundColor Cyan
$objdump = Join-Path $mingw 'objdump.exe'
$present = @{}
Get-ChildItem $stage -Recurse -Filter *.dll | ForEach-Object { $present[$_.Name.ToLower()] = $true }
$missing = @()
foreach ($binary in Get-ChildItem $stage -Recurse -Include *.exe, *.dll) {
    foreach ($line in & $objdump -p $binary.FullName) {
        if ($line -match 'DLL Name:\s*(\S+)') {
            $dll = $Matches[1].ToLower()
            if ($present.ContainsKey($dll)) { continue }
            if (Test-Path (Join-Path $env:SystemRoot "System32\$dll")) { continue }
            if ($dll -like 'api-ms-win-*' -or $dll -like 'ext-ms-*') { continue }
            $missing += "$($binary.Name) -> $dll"
        }
    }
}
if ($missing) { throw "Missing DLLs:`n$($missing -join "`n")" }

# Startup probe with only Windows on the PATH, so that every DLL comes from
# the package. The offscreen platform plugin (not shipped) is taken from Qt
# just for this run, so that no window appears; the probe must write its
# marker file, proving that the application reached its event loop.
$savedPath = $env:PATH
$marker = Join-Path $env:TEMP "sshmanager-probe-$PID.txt"
Remove-Item $marker -ErrorAction SilentlyContinue
$env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
$env:QT_QPA_PLATFORM = 'offscreen'
$env:QT_QPA_PLATFORM_PLUGIN_PATH = Join-Path $qt 'plugins\platforms'
$env:SSHMANAGER_STARTUP_PROBE = $marker
try {
    $probe = Start-Process -FilePath $exe -PassThru -WorkingDirectory $stage
    $deadline = (Get-Date).AddSeconds(20)
    while (-not (Test-Path $marker) -and -not $probe.HasExited -and (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 250
    }
    if (-not $probe.HasExited) { Stop-Process -Id $probe.Id -Force }
} finally {
    $env:PATH = $savedPath
    Remove-Item Env:QT_QPA_PLATFORM, Env:QT_QPA_PLATFORM_PLUGIN_PATH, Env:SSHMANAGER_STARTUP_PROBE -ErrorAction SilentlyContinue
}
if (-not (Test-Path $marker)) { throw 'sshmanager.exe did not start correctly from the package folder' }
Get-Content $marker | ForEach-Object { Write-Host "  $_" }
Remove-Item $marker
Write-Host 'DLLs complete, startup OK'

# 4. Portable zip, with a top-level "SSH Manager" folder.
Write-Host '== Zip' -ForegroundColor Cyan
New-Item -ItemType Directory -Force $OutDir | Out-Null
$appFolder = Join-Path $installRoot 'SSH Manager'
if (Test-Path $appFolder) { Remove-Item $appFolder -Recurse -Force }
Rename-Item $stage 'SSH Manager'
$zip = Join-Path $OutDir "SSHManager-$version-win64.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path $appFolder -DestinationPath $zip
Write-Host $zip

# 5. Installer, when Inno Setup is available.
if (-not $NoInstaller) {
    $iscc = @((Get-Command ISCC.exe -ErrorAction SilentlyContinue).Source,
              "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
              "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
              "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe") |
        Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
    if ($iscc) {
        Write-Host '== Installer' -ForegroundColor Cyan
        Invoke-Native $iscc @("/DAppVersion=$version", "/DSourceDir=$appFolder", "/DOutputDir=$OutDir",
            "/DIconFile=$(Join-Path $source 'resources\sshmanager.ico')", (Join-Path $PSScriptRoot 'sshmanager.iss'))
    } else {
        Write-Host 'Inno Setup 6 not found: installer skipped (the zip is complete).' -ForegroundColor Yellow
    }
}
Write-Host 'Done.' -ForegroundColor Green
