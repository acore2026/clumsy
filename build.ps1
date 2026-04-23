<#
.SYNOPSIS
Builds clumsy on Windows using a MinGW-compatible GCC toolchain.

.DESCRIPTION
This script compiles the resource file, builds the executable, and bundles the
runtime files needed to run clumsy from the output directory.

It is designed to be a practical repo-level entrypoint for local development:
- x64 and x86 support
- Debug, Release, and Ship configurations
- WinDivert sign A/B/C selection
- optional clean and run steps
- clear dependency checks and command logging

.EXAMPLE
./build.ps1

.EXAMPLE
./build.ps1 -Arch x64 -Configuration Release -VerboseCommands

.EXAMPLE
./build.ps1 -Arch x86 -Configuration Debug -Clean
#>

[CmdletBinding()]
param(
    [ValidateSet("x64", "x86")]
    [string]$Arch = "x64",

    [ValidateSet("Debug", "Release", "Ship")]
    [string]$Configuration = "Debug",

    [ValidateSet("A", "B", "C")]
    [string]$Sign = "A",

    [string]$OutputRoot = "out",

    [switch]$Clean,

    [switch]$Run,

    [switch]$VerboseCommands
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Write-Section {
    param([string]$Message)
    Write-Host ""
    Write-Host "== $Message ==" -ForegroundColor Cyan
}

function Resolve-RepoPath {
    param([string]$RelativePath)
    return [System.IO.Path]::GetFullPath((Join-Path $script:RepoRoot $RelativePath))
}

function Resolve-CommandPath {
    param(
        [string[]]$Candidates
    )

    foreach ($candidate in $Candidates) {
        $command = Get-Command $candidate -ErrorAction SilentlyContinue
        if ($command) {
            return $command.Source
        }
    }

    return $null
}

function Resolve-SiblingCommandPath {
    param(
        [string]$ReferenceCommandPath,
        [string[]]$Candidates
    )

    $referenceDir = Split-Path -Parent $ReferenceCommandPath
    foreach ($candidate in $Candidates) {
        $candidatePath = Join-Path $referenceDir ($candidate + ".exe")
        if (Test-Path $candidatePath) {
            return $candidatePath
        }
    }

    return $null
}

function Invoke-External {
    param(
        [string]$FilePath,
        [string[]]$Arguments
    )

    if ($VerboseCommands) {
        $rendered = @($FilePath) + $Arguments | ForEach-Object {
            if ($_ -match "\s") { '"{0}"' -f $_ } else { $_ }
        }
        Write-Host ($rendered -join " ") -ForegroundColor DarkGray
    }

    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $FilePath"
    }
}

function Get-BuildFolderName {
    $name = "{0}-{1}" -f $Configuration.ToLowerInvariant(), $Arch
    if ($Sign -ne "A") {
        $name = "{0}-{1}" -f $name, $Sign
    }
    return $name
}

function Get-Toolchain {
    $gccCandidates = if ($Arch -eq "x64") {
        @("x86_64-w64-mingw32-gcc", "gcc")
    } else {
        @("i686-w64-mingw32-gcc", "gcc")
    }

    $windresCandidates = if ($Arch -eq "x64") {
        @("x86_64-w64-mingw32-windres", "windres")
    } else {
        @("i686-w64-mingw32-windres", "windres")
    }

    $gccPath = Resolve-CommandPath -Candidates $gccCandidates
    if (-not $gccPath) {
        throw "Unable to find a GCC compiler. Install MinGW-w64/TDM-GCC and ensure gcc is on PATH."
    }

    $windresPath = Resolve-SiblingCommandPath -ReferenceCommandPath $gccPath -Candidates $windresCandidates
    if (-not $windresPath) {
        $windresPath = Resolve-CommandPath -Candidates $windresCandidates
    }
    if (-not $windresPath) {
        throw "Unable to find windres. Install a MinGW-compatible binutils package and ensure windres is on PATH."
    }

    $useM32 = $false
    if ($Arch -eq "x86" -and [System.IO.Path]::GetFileName($gccPath).ToLowerInvariant() -eq "gcc.exe") {
        $useM32 = $true
    }

    return @{
        Gcc = $gccPath
        Windres = $windresPath
        UseM32 = $useM32
    }
}

$RepoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$toolchain = Get-Toolchain

$windivertDir = Resolve-RepoPath ("external/WinDivert-2.2.0-{0}" -f $Sign)
$iupRelativeDir = if ($Arch -eq "x64") { "external/iup-3.30_Win64_mingw6_lib" } else { "external/iup-3.30_Win32_mingw6_lib" }
$windivertArchDir = if ($Arch -eq "x64") { "x64" } else { "x86" }
$windivertSysName = if ($Arch -eq "x64") { "WinDivert64.sys" } else { "WinDivert32.sys" }
$iupDir = Resolve-RepoPath $iupRelativeDir
$outputDir = Resolve-RepoPath (Join-Path $OutputRoot (Get-BuildFolderName))
$objectDir = Join-Path $outputDir "obj"
$resourceObject = Join-Path $objectDir "clumsy_res.o"
$executablePath = Join-Path $outputDir "clumsy.exe"

$sourceFiles = @(
    "src/bandwidth.c",
    "src/divert.c",
    "src/drop.c",
    "src/duplicate.c",
    "src/elevate.c",
    "src/lag.c",
    "src/main.c",
    "src/ood.c",
    "src/packet.c",
    "src/remote.c",
    "src/reset.c",
    "src/scenario.c",
    "src/tamper.c",
    "src/throttle.c",
    "src/utils.c"
) | ForEach-Object { Resolve-RepoPath $_ }

if ($Arch -eq "x86") {
    $sourceFiles += Resolve-RepoPath "etc/chkstk.s"
}

$requiredFiles = @(
    (Join-Path $windivertDir "include\windivert.h"),
    (Join-Path $windivertDir (Join-Path $windivertArchDir "WinDivert.dll")),
    (Join-Path $windivertDir (Join-Path $windivertArchDir $windivertSysName)),
    (Join-Path $windivertDir (Join-Path $windivertArchDir "WinDivert.lib")),
    (Join-Path $iupDir "include\iup.h"),
    (Join-Path $iupDir "libiup.a"),
    (Resolve-RepoPath "etc/config.txt"),
    (Resolve-RepoPath "etc/scenarios.ini"),
    (Resolve-RepoPath "etc/clumsy.rc")
)

foreach ($path in $requiredFiles) {
    if (-not (Test-Path $path)) {
        throw "Required build input not found: $path"
    }
}

Write-Section "Build Settings"
Write-Host ("Repo root     : {0}" -f $RepoRoot)
Write-Host ("Architecture  : {0}" -f $Arch)
Write-Host ("Configuration : {0}" -f $Configuration)
Write-Host ("WinDivert sign: {0}" -f $Sign)
Write-Host ("GCC           : {0}" -f $toolchain.Gcc)
Write-Host ("windres       : {0}" -f $toolchain.Windres)
Write-Host ("Output        : {0}" -f $outputDir)

if ($Clean -and (Test-Path $outputDir)) {
    Write-Section "Cleaning"
    Remove-Item -Recurse -Force $outputDir
}

New-Item -ItemType Directory -Force -Path $objectDir | Out-Null

$windresArgs = @("-O", "coff")
if ($Arch -eq "x64") {
    $windresArgs += @("-DX64", "-F", "pe-x86-64")
} else {
    $windresArgs += @("-DX86", "-F", "pe-i386")
}
$windresArgs += @(
    "-i", (Resolve-RepoPath "etc/clumsy.rc"),
    "-o", $resourceObject
)

Write-Section "Compiling Resources"
Invoke-External -FilePath $toolchain.Windres -Arguments $windresArgs

$gccArgs = @(
    "-std=c99",
    "-Wall",
    "-Wno-missing-braces",
    "-Wno-missing-field-initializers",
    "-I", (Join-Path $windivertDir "include"),
    "-I", (Join-Path $iupDir "include")
)

switch ($Configuration) {
    "Debug" {
        $gccArgs += @("-g", "-D_DEBUG")
    }
    "Release" {
        $gccArgs += @("-O2", "-DNDEBUG", "-mwindows")
    }
    "Ship" {
        $gccArgs += @("-O3", "-DNDEBUG", "-s", "-mwindows")
    }
}

if ($toolchain.UseM32) {
    $gccArgs += "-m32"
}

$gccArgs += $sourceFiles
$gccArgs += $resourceObject
$gccArgs += @(
    "-L", (Join-Path $windivertDir $windivertArchDir),
    "-L", $iupDir,
    (Join-Path $iupDir "libiup.a"),
    "-lWinDivert",
    "-lcomctl32",
    "-lwinmm",
    "-lws2_32",
    "-lkernel32",
    "-lgdi32",
    "-lcomdlg32",
    "-luuid",
    "-lole32",
    "-o", $executablePath
)

Write-Section "Building"
Invoke-External -FilePath $toolchain.Gcc -Arguments $gccArgs

Write-Section "Bundling Runtime Files"
$dllSource = Join-Path $windivertDir (Join-Path $windivertArchDir "WinDivert.dll")
$sysSource = Join-Path $windivertDir (Join-Path $windivertArchDir $windivertSysName)
Copy-Item $dllSource (Join-Path $outputDir "WinDivert.dll") -Force
Copy-Item $sysSource (Join-Path $outputDir (Split-Path -Leaf $sysSource)) -Force
Copy-Item (Resolve-RepoPath "etc/config.txt") (Join-Path $outputDir "config.txt") -Force
Copy-Item (Resolve-RepoPath "etc/scenarios.ini") (Join-Path $outputDir "scenarios.ini") -Force
if ($Configuration -eq "Ship") {
    Copy-Item (Resolve-RepoPath "LICENSE") (Join-Path $outputDir "License.txt") -Force
}

Write-Section "Done"
Write-Host ("Built executable: {0}" -f $executablePath) -ForegroundColor Green

if ($Run) {
    Write-Section "Running"
    Start-Process -FilePath $executablePath -WorkingDirectory $outputDir
}
