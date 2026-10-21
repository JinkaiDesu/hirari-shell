#Requires -Version 7.2
<#
.SYNOPSIS
  hirari's release package: hirari-<version>-windows-x64.zip, the release folder `cmake --install` writes, with
  release.receipt.json naming every file's size and SHA-256. Files go in a fixed order with fixed timestamps, so the
  same build gives the same bytes. The archive is then checked on its own (tests/release.tests.ps1).

.EXAMPLE
  cmake -S . -B build; cmake --build build --config Release
  pwsh tools/package.ps1 -Build build -Out out
#>
param(
    [string]$Build = 'build',
    [string]$Out = 'out'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$Build = [IO.Path]::GetFullPath($Build, $repo)
$Out = [IO.Path]::GetFullPath($Out, $repo)
$cmake = Get-Content -LiteralPath (Join-Path $repo 'CMakeLists.txt') -Raw
if ($cmake -notmatch 'project\(Hirari VERSION ([0-9.]+)') { throw 'CMakeLists.txt names no hirari version' }
$version = $Matches[1]

$folder = Join-Path $Build 'package'
if (Test-Path -LiteralPath $folder) { Remove-Item -LiteralPath $folder -Recurse -Force }
& cmake --install $Build --config Release --component hirari --prefix $folder
if ($LASTEXITCODE) { throw "cmake --install exited with $LASTEXITCODE" }

$files = @(Get-ChildItem -LiteralPath $folder -Recurse -File | ForEach-Object {
        [ordered]@{ path = [IO.Path]::GetRelativePath($folder, $_.FullName) -replace '\\', '/'; size = $_.Length; sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
    } | Sort-Object { $_.path } -CaseSensitive)
[ordered]@{ schemaVersion = 1; name = 'hirari'; version = $version; files = $files } |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $folder 'release.receipt.json') -Encoding utf8NoBOM

New-Item -ItemType Directory -Force $Out | Out-Null
$zip = Join-Path $Out "hirari-$version-windows-x64.zip"
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip }
Add-Type -AssemblyName System.IO.Compression
$entries = @(Get-ChildItem -LiteralPath $folder -Recurse -File | Sort-Object { [IO.Path]::GetRelativePath($folder, $_.FullName) -replace '\\', '/' } -CaseSensitive)
$stamp = [DateTimeOffset]::new(2000, 1, 1, 0, 0, 0, [TimeSpan]::Zero)
$stream = [IO.File]::Open($zip, [IO.FileMode]::CreateNew)
try {
    $archive = [IO.Compression.ZipArchive]::new($stream, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($file in $entries) {
            $entry = $archive.CreateEntry(([IO.Path]::GetRelativePath($folder, $file.FullName) -replace '\\', '/'), [IO.Compression.CompressionLevel]::Optimal)
            $entry.LastWriteTime = $stamp
            $writer = $entry.Open()
            try { $reader = [IO.File]::OpenRead($file.FullName); try { $reader.CopyTo($writer) } finally { $reader.Dispose() } } finally { $writer.Dispose() }
        }
    } finally { $archive.Dispose() }
} finally { $stream.Dispose() }

& (Join-Path $repo 'tests/release.tests.ps1') -Archive $zip -Version $version
"packaged $zip"

