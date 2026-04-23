<#
.SYNOPSIS
Packages a clumsy build output directory into a versioned ZIP archive.

.EXAMPLE
./scripts/package-release.ps1 -BuildOutputDir out/ship-x64 -Version v0.3.0 -Arch x64
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$BuildOutputDir,

    [Parameter(Mandatory = $true)]
    [string]$Version,

    [Parameter(Mandatory = $true)]
    [ValidateSet("x64", "x86")]
    [string]$Arch,

    [string]$DistRoot = "dist"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-FullPath {
    param([string]$Path)

    return [System.IO.Path]::GetFullPath($Path)
}

$resolvedBuildOutput = Resolve-FullPath $BuildOutputDir
if (-not (Test-Path $resolvedBuildOutput)) {
    throw "Build output directory not found: $resolvedBuildOutput"
}

$distDir = Resolve-FullPath $DistRoot
$stagingDir = Join-Path $distDir "staging"
$packageName = "clumsy-{0}-windows-{1}" -f $Version, $Arch
$packageDir = Join-Path $stagingDir $packageName
$zipPath = Join-Path $distDir ($packageName + ".zip")

New-Item -ItemType Directory -Force -Path $distDir | Out-Null

if (Test-Path $packageDir) {
    Remove-Item -Recurse -Force $packageDir
}
New-Item -ItemType Directory -Force -Path $packageDir | Out-Null

Get-ChildItem -Path $resolvedBuildOutput -File | ForEach-Object {
    Copy-Item $_.FullName -Destination (Join-Path $packageDir $_.Name) -Force
}

if (-not (Test-Path (Join-Path $packageDir "clumsy.exe"))) {
    throw "Expected clumsy.exe in package input: $resolvedBuildOutput"
}

if (Test-Path $zipPath) {
    Remove-Item -Force $zipPath
}

Compress-Archive -Path $packageDir -DestinationPath $zipPath -CompressionLevel Optimal
Write-Host ("Created package: {0}" -f $zipPath)
