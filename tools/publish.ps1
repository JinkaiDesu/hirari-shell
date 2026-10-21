#Requires -Version 7.2
<#
.SYNOPSIS
  Publishes hirari's release archive to the public downloads bucket, then its emergence.manifest/1 last, and checks
  both live. Runs from a clean checkout of a commit on origin/main, with the archive tools/package.ps1 built there.

.DESCRIPTION
  The archive goes to hirari/<version>/hirari-<version>-windows-x64.zip in the R2 bucket emergence-releases
  (https://downloads.emergencetech.co). A published version is never overwritten: if the live manifest already names
  this version, nothing is uploaded and the last line is UNCHANGED. Otherwise the last line is PUBLISHED.
  The Cloudflare token comes from CLOUDFLARE_API_TOKEN (process, else user environment) and is never printed.

.EXAMPLE
  pwsh -NoProfile -File tools/package.ps1 -Build build -Out out
  pwsh -NoProfile -File tools/publish.ps1
#>
param(
    [string]$Archive
)
$ErrorActionPreference = 'Stop'
$Bucket = 'emergence-releases'
$Base = 'https://downloads.emergencetech.co'
$Site = 'https://emergencetech.co'
$Wrangler = 'wrangler@4.149.0'
$env:CLOUDFLARE_ACCOUNT_ID = '7e05861211be8c45fef9de909a25f833'

$repo = Split-Path $PSScriptRoot -Parent
$cmake = Get-Content -LiteralPath (Join-Path $repo 'CMakeLists.txt') -Raw
if ($cmake -notmatch 'project\(Hirari VERSION ([0-9.]+)') { throw 'CMakeLists.txt names no hirari version' }
$version = $Matches[1]
$file = "hirari-$version-windows-x64.zip"
if (-not $Archive) { $Archive = Join-Path $repo "out/$file" }
$Archive = [IO.Path]::GetFullPath($Archive, $repo)
if (-not (Test-Path -LiteralPath $Archive -PathType Leaf)) { throw "no archive at $Archive; run tools/package.ps1 first" }

# Only a merged commit is published, from a checkout that holds exactly it.
git -C $repo fetch -q origin main
if ($LASTEXITCODE) { throw 'git fetch origin main failed' }
$commit = "$(git -C $repo rev-parse HEAD)".Trim()
if (git -C $repo status --porcelain --untracked-files=no) { throw "$repo has local changes; publish only a clean checkout" }
git -C $repo merge-base --is-ancestor $commit origin/main
if ($LASTEXITCODE) { throw "$($commit.Substring(0, 9)) is not on origin/main" }

$key = "hirari/$version/$file"
$url = "$Base/$key"
$bytes = (Get-Item -LiteralPath $Archive).Length
$sha256 = (Get-FileHash -LiteralPath $Archive -Algorithm SHA256).Hash.ToLowerInvariant()

function Get-Public([string]$Uri) {
    try { Invoke-WebRequest -Uri "$Uri`?v=$([guid]::NewGuid().ToString('N'))" -Headers @{ 'Cache-Control' = 'no-cache' } -TimeoutSec 60 } catch {
        if ($_.Exception.Response.StatusCode -eq 404) { return $null }; throw
    }
}
function Get-Sha256([byte[]]$Data) { [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($Data)).ToLowerInvariant() }

$live = Get-Public "$Base/hirari/manifest.json"
if ($live -and (($live.Content | ConvertFrom-Json).version -eq $version)) {
    Write-Host "UNCHANGED: $Base/hirari/manifest.json already names hirari $version"
    exit 0
}

if (-not $env:CLOUDFLARE_API_TOKEN) { $env:CLOUDFLARE_API_TOKEN = [Environment]::GetEnvironmentVariable('CLOUDFLARE_API_TOKEN', 'User') }
if (-not $env:CLOUDFLARE_API_TOKEN) { throw 'no CLOUDFLARE_API_TOKEN in the process or user environment' }

function Send-Object([string]$Key, [string]$Path, [string]$Type, [string]$Cache) {
    $out = npx --yes $Wrangler r2 object put "$Bucket/$Key" --file $Path --remote --content-type $Type --cache-control $Cache 2>&1
    if ($LASTEXITCODE) { throw "wrangler r2 object put $Key failed: $(($out | Select-Object -Last 6) -join ' ')" }
}

# The versioned archive is immutable: an earlier, unfinished publish of the same bytes is reused, other bytes refused.
$existing = Get-Public $url
if ($existing) {
    if ((Get-Sha256 $existing.RawContentStream.ToArray()) -ne $sha256) { throw "$url already holds different bytes; a published version is never overwritten" }
    Write-Host "archive already at $url with the same bytes"
} else {
    Send-Object $key $Archive 'application/zip' 'public, max-age=31536000, immutable'
    $served = Get-Public $url
    if (-not $served) { throw "$url is not served after the upload" }
    $data = $served.RawContentStream.ToArray()
    if ($data.Length -ne $bytes -or (Get-Sha256 $data) -ne $sha256) { throw "$url does not serve the uploaded bytes" }
    Write-Host "uploaded $url ($bytes bytes, sha256 $sha256)"
}

$manifest = [ordered]@{
    schema = 'emergence.manifest/1'; id = 'hirari'; name = 'hirari'; edition = 'pre-alpha'; platform = 'windows-x64'
    version = $version; commit = $commit; built = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    artifact = [ordered]@{ file = $file; key = $key; url = $url; bytes = $bytes; sha256 = $sha256 }
}
$manifestPath = Join-Path ([IO.Path]::GetTempPath()) "hirari-manifest-$([guid]::NewGuid().ToString('N')).json"
[IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json -Depth 5))
try { Send-Object 'hirari/manifest.json' $manifestPath 'application/json' 'no-cache' } finally { Remove-Item -LiteralPath $manifestPath }

$served = (Get-Public "$Base/hirari/manifest.json").Content | ConvertFrom-Json
if ($served.version -ne $version -or $served.commit -ne $commit -or $served.artifact.sha256 -ne $sha256) { throw "$Base/hirari/manifest.json does not serve the manifest just written" }
for ($i = 0; $i -lt 12; $i++) {
    try { $latest = Invoke-RestMethod -Uri "$Site/api/latest/hirari" -TimeoutSec 30 } catch { $latest = $null }
    if ($latest.version -eq $version -and $latest.commit -eq $commit) { break }
    Start-Sleep 5
}
if ($latest.version -ne $version -or $latest.commit -ne $commit) { throw "$Site/api/latest/hirari does not answer hirari $version at $($commit.Substring(0, 9))" }
Write-Host "PUBLISHED hirari $version-pre-alpha at $($commit.Substring(0, 9)): $url; $Site/api/latest/hirari answers it"
