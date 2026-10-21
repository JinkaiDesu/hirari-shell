#Requires -Version 7.2
<#
.SYNOPSIS
  Publishes hirari's release archive to the public downloads bucket, then its emergence.manifest/1, and checks them
  live. Runs from a clean checkout of a commit on origin/main, with the archive tools/package.ps1 built there.

.DESCRIPTION
  The bucket is emergence-releases (https://downloads.emergencetech.co). Each version has its own folder:
    hirari/<version>/hirari-<version>-windows-x64.zip
    hirari/<version>/manifest.json      the version's manifest, immutable
    hirari/manifest.json                the latest pointer: a byte-identical copy of the newest version's manifest
  They are written in that order, so the pointer never names anything that isn't there yet.

  A published version is never overwritten. If this version's manifest is already up and the pointer is its exact
  copy, nothing is uploaded and the last line is UNCHANGED. A run that stopped part-way is finished from what is
  already live: a missing versioned manifest is copied from the pointer's exact bytes, and a pointer still on an
  older version is moved to this one. Otherwise the last line is PUBLISHED.
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
$key = "hirari/$version/$file"
$url = "$Base/$key"
$versionedKey = "hirari/$version/manifest.json"
$latestKey = 'hirari/manifest.json'

# Only a merged commit is published, from a checkout that holds exactly it.
git -C $repo fetch -q origin main
if ($LASTEXITCODE) { throw 'git fetch origin main failed' }
$commit = "$(git -C $repo rev-parse HEAD)".Trim()
if (git -C $repo status --porcelain --untracked-files=no) { throw "$repo has local changes; publish only a clean checkout" }
git -C $repo merge-base --is-ancestor $commit origin/main
if ($LASTEXITCODE) { throw "$($commit.Substring(0, 9)) is not on origin/main" }

function Get-Public([string]$Uri) {
    try { Invoke-WebRequest -Uri "$Uri`?v=$([guid]::NewGuid().ToString('N'))" -Headers @{ 'Cache-Control' = 'no-cache' } -TimeoutSec 60 } catch {
        if ($_.Exception.Response.StatusCode -eq 404) { return $null }; throw
    }
}
function Get-Bytes($Response) { if ($Response) { , $Response.RawContentStream.ToArray() } else { $null } }
function Get-Sha256([byte[]]$Data) { [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($Data)).ToLowerInvariant() }
function Test-Same([byte[]]$A, [byte[]]$B) { $null -ne $A -and $null -ne $B -and [Linq.Enumerable]::SequenceEqual($A, $B) }

function Use-Token {
    if (-not $env:CLOUDFLARE_API_TOKEN) { $env:CLOUDFLARE_API_TOKEN = [Environment]::GetEnvironmentVariable('CLOUDFLARE_API_TOKEN', 'User') }
    if (-not $env:CLOUDFLARE_API_TOKEN) { throw 'no CLOUDFLARE_API_TOKEN in the process or user environment' }
}
function Send-Object([string]$Key, [string]$Path, [string]$Type, [string]$Cache) {
    $out = npx --yes $Wrangler r2 object put "$Bucket/$Key" --file $Path --remote --content-type $Type --cache-control $Cache 2>&1
    if ($LASTEXITCODE) { throw "wrangler r2 object put $Key failed: $(($out | Select-Object -Last 6) -join ' ')" }
}
# Uploads exactly these manifest bytes to $Key and checks they are served back unchanged.
function Send-Manifest([string]$Key, [byte[]]$Data, [string]$Cache) {
    $path = Join-Path ([IO.Path]::GetTempPath()) "hirari-manifest-$([guid]::NewGuid().ToString('N')).json"
    [IO.File]::WriteAllBytes($path, $Data)
    try { Send-Object $Key $path 'application/json' $Cache } finally { Remove-Item -LiteralPath $path }
    if (-not (Test-Same (Get-Bytes (Get-Public "$Base/$Key")) $Data)) { throw "$Base/$Key does not serve the manifest just written" }
    Write-Host "wrote $Base/$Key"
}
$Immutable = 'public, max-age=31536000, immutable'

# The repository's own record of a release: the annotated tag v<version> on the commit its manifest names.
function Set-Tag([byte[]]$Manifest) {
    $named = [Text.Encoding]::UTF8.GetString($Manifest) | ConvertFrom-Json
    $tag = "v$($named.version)"
    $remote = git -C $repo ls-remote --tags origin "refs/tags/$tag^{}"
    if ($LASTEXITCODE) { throw "git ls-remote origin $tag failed" }
    if ($remote) {
        if ("$remote".Split("`t")[0] -ne $named.commit) { throw "tag $tag is on $("$remote".Substring(0, 9)), not on the published $($named.commit.Substring(0, 9))" }
        Write-Host "tag $tag is on $($named.commit.Substring(0, 9))"
        return
    }
    $message = Join-Path ([IO.Path]::GetTempPath()) "hirari-tag-$([guid]::NewGuid().ToString('N')).txt"
    [IO.File]::WriteAllText($message, "hirari $($named.version) ($($named.edition))`n`nManifest $Base/hirari/$($named.version)/manifest.json, sha256 $(Get-Sha256 $Manifest).`nArchive $($named.artifact.url), sha256 $($named.artifact.sha256).`n")
    try {
        git -C $repo tag -f -a $tag $named.commit -F $message | Out-Null
        if ($LASTEXITCODE) { throw "git tag $tag failed" }
    } finally { Remove-Item -LiteralPath $message }
    git -C $repo push -q origin "refs/tags/$tag" 2>&1 | Out-Null
    if ($LASTEXITCODE) { throw "git push origin $tag failed" }
    Write-Host "tagged $tag on $($named.commit.Substring(0, 9)) and pushed it"
}

$versioned = Get-Bytes (Get-Public "$Base/$versionedKey")
$latest = Get-Bytes (Get-Public "$Base/$latestKey")
$latestVersion = if ($latest) { ([Text.Encoding]::UTF8.GetString($latest) | ConvertFrom-Json).version }

if ($versioned) {
    if (Test-Same $latest $versioned) {
        Set-Tag $versioned
        Write-Host "UNCHANGED: $Base/$versionedKey is up and $Base/$latestKey is its exact copy"
        exit 0
    }
    # This version is up but the pointer isn't on it: move the pointer forward, never back.
    if ($latest -and [version]$latestVersion -ge [version]$version) { throw "$Base/$latestKey names hirari $latestVersion, not older than $version; it is left as it is" }
    Use-Token
    Send-Manifest $latestKey $versioned 'no-cache'
    $manifest = $versioned
} elseif ($latest -and $latestVersion -eq $version) {
    # An earlier publish wrote only the pointer: its exact bytes become this version's manifest.
    Use-Token
    Send-Manifest $versionedKey $latest $Immutable
    $manifest = $latest
} else {
    if (-not $Archive) { $Archive = Join-Path $repo "out/$file" }
    $Archive = [IO.Path]::GetFullPath($Archive, $repo)
    if (-not (Test-Path -LiteralPath $Archive -PathType Leaf)) { throw "no archive at $Archive; run tools/package.ps1 first" }
    $bytes = (Get-Item -LiteralPath $Archive).Length
    $sha256 = (Get-FileHash -LiteralPath $Archive -Algorithm SHA256).Hash.ToLowerInvariant()
    Use-Token

    # The versioned archive is immutable: an earlier, unfinished publish of the same bytes is reused, other bytes refused.
    $existing = Get-Bytes (Get-Public $url)
    if ($existing) {
        if ((Get-Sha256 $existing) -ne $sha256) { throw "$url already holds different bytes; a published version is never overwritten" }
        Write-Host "archive already at $url with the same bytes"
    } else {
        Send-Object $key $Archive 'application/zip' $Immutable
        $served = Get-Bytes (Get-Public $url)
        if (-not $served -or $served.Length -ne $bytes -or (Get-Sha256 $served) -ne $sha256) { throw "$url does not serve the uploaded bytes" }
        Write-Host "uploaded $url ($bytes bytes, sha256 $sha256)"
    }

    $manifest = [Text.UTF8Encoding]::new($false).GetBytes(([ordered]@{
        schema = 'emergence.manifest/1'; id = 'hirari'; name = 'hirari'; edition = 'pre-alpha'; platform = 'windows-x64'
        version = $version; commit = $commit; built = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
        artifact = [ordered]@{ file = $file; key = $key; url = $url; bytes = $bytes; sha256 = $sha256 }
    } | ConvertTo-Json -Depth 5))
    Send-Manifest $versionedKey $manifest $Immutable
    Send-Manifest $latestKey $manifest 'no-cache'
}

$named = [Text.Encoding]::UTF8.GetString($manifest) | ConvertFrom-Json
for ($i = 0; $i -lt 12; $i++) {
    try { $answer = Invoke-RestMethod -Uri "$Site/api/latest/hirari" -TimeoutSec 30 } catch { $answer = $null }
    if ($answer.version -eq $named.version -and $answer.commit -eq $named.commit) { break }
    Start-Sleep 5
}
if ($answer.version -ne $named.version -or $answer.commit -ne $named.commit) { throw "$Site/api/latest/hirari does not answer hirari $($named.version) at $($named.commit.Substring(0, 9))" }
Set-Tag $manifest
Write-Host "PUBLISHED hirari $($named.version)-pre-alpha at $($named.commit.Substring(0, 9)): $Base/$versionedKey, and $Base/$latestKey is its exact copy; $Site/api/latest/hirari answers it"
