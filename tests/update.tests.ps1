#Requires -Version 7.2
<#
.SYNOPSIS
  `hirari --update` replaces a real installation with a real newer shell, read from local manifests, and leaves the
  installation exactly as it was whenever anything about the release is wrong.
#>
param(
    [Parameter(Mandatory)][string]$Current,
    [Parameter(Mandatory)][string]$Fixture
)
$ErrorActionPreference = 'Stop'
$root = Join-Path ([IO.Path]::GetTempPath()) ("hirari update " + [guid]::NewGuid().ToString('N').Substring(0, 8))
$passed = 0
function Check([bool]$Okay, [string]$Name) { if (-not $Okay) { throw "FAIL: $Name" }; $script:passed++; Write-Host "PASS $Name" }
function Hash([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
function Text([string]$Path, [string]$Value) { New-Item -ItemType Directory -Force (Split-Path $Path) | Out-Null; [IO.File]::WriteAllText($Path, $Value) }
function Write-Receipt([string]$Folder, [string]$Version, [string[]]$Extra = @()) {
    $files = @(Get-ChildItem -LiteralPath $Folder -Recurse -File | Where-Object Name -ne 'release.receipt.json' | ForEach-Object {
            [ordered]@{ path = [IO.Path]::GetRelativePath($Folder, $_.FullName) -replace '\\', '/'; size = $_.Length; sha256 = Hash $_.FullName }
        })
    foreach ($path in $Extra) { $files += [ordered]@{ path = $path; size = 0; sha256 = '' } }
    [ordered]@{ schemaVersion = 1; name = 'hirari'; version = $Version; files = $files } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $Folder 'release.receipt.json') -Encoding utf8NoBOM
}
function New-Install([string]$Name, [string]$Version) {
    $folder = Join-Path $root $Name
    New-Item -ItemType Directory -Force $folder | Out-Null
    Copy-Item -LiteralPath $Current (Join-Path $folder 'hirari.exe')
    Text (Join-Path $folder 'Commands/old.seed') '"old"'
    Text (Join-Path $folder 'Gone/only.txt') 'the old release alone has this folder'
    Write-Receipt $folder $Version
    Text (Join-Path $folder 'mine.txt') 'not part of any release'
    $folder
}
# Makes an archive holding $Binary as hirari.exe, and a manifest for it. $Change edits the manifest; $Prepare the folder.
function New-Release([string]$Name, [string]$Version, [string]$Binary, [scriptblock]$Prepare, [scriptblock]$Change, [string]$Platform = 'windows-x64') {
    $folder = Join-Path $root "$Name-content"
    New-Item -ItemType Directory -Force $folder | Out-Null
    Copy-Item -LiteralPath $Binary (Join-Path $folder 'hirari.exe')
    Text (Join-Path $folder 'Commands/new.seed') '"new"'
    Write-Receipt $folder $Version
    if ($Prepare) { & $Prepare $folder }
    $zip = Join-Path $root "$Name.zip"
    Compress-Archive -Path (Join-Path $folder '*') -DestinationPath $zip
    $manifest = [ordered]@{ schema = 'emergence.manifest/1'; id = 'hirari'; name = 'hirari'; edition = 'pre-alpha'; platform = $Platform; version = $Version
        artifact = [ordered]@{ url = [Uri]::new($zip).AbsoluteUri; bytes = (Get-Item -LiteralPath $zip).Length; sha256 = Hash $zip } }
    if ($Change) { & $Change $manifest }
    $path = Join-Path $root "$Name.json"
    $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $path -Encoding utf8NoBOM
    [Uri]::new($path).AbsoluteUri
}
function Invoke-Update([string]$Folder, [string]$Manifest, [string[]]$Options = @()) {
    $env:HIRARI_UPDATE_MANIFEST = $Manifest
    # Each run identifies its own installation, as a shell started from PATH does.
    Remove-Item Env:HIRARI_HOME -ErrorAction SilentlyContinue
    $script:output = (& (Join-Path $Folder 'hirari.exe') --update @Options 2>&1 | Out-String).Trim()
    $script:status = $LASTEXITCODE
}
function Get-State([string]$Folder) { (Get-ChildItem -LiteralPath $Folder -Recurse -File -Force | Sort-Object FullName | ForEach-Object { "$($_.FullName) $(Hash $_.FullName)" }) -join "`n" }
function Check-Refused([string]$Name, [string]$Folder, [string]$Manifest, [string]$Why) {
    $before = Get-State $Folder
    Invoke-Update $Folder $Manifest
    Check ($status -eq 1 -and $output -match $Why) "$Name is refused ($status; $output)"
    Check ((Get-State $Folder) -ceq $before) "$Name leaves the installation exactly as it was"
}

New-Item -ItemType Directory -Force $root | Out-Null
try {
    $Current = [IO.Path]::GetFullPath($Current); $Fixture = [IO.Path]::GetFullPath($Fixture)
    $version = "$(@(& $Current --version)[0])".Split(' ')[1]
    $install = New-Install 'installed hirari' $version
    $good = New-Release 'good' '99.0.0' $Fixture

    Invoke-Update $install (New-Release 'same' $version $Current)
    Check ($status -eq 0 -and $output -ceq "UP-TO-DATE $version") "the latest version is up to date ($output)"
    Invoke-Update $install (New-Release 'older' '0.0.1' $Current)
    Check ($status -eq 0 -and $output -ceq "UP-TO-DATE $version") "an older latest is never installed ($output)"
    $before = Get-State $install
    Invoke-Update $install $good @('--check')
    Check ($status -eq 10 -and $output -ceq "UPDATE-AVAILABLE $version 99.0.0") "--check reports the update with status 10 ($status; $output)"
    Check ((Get-State $install) -ceq $before) '--check changes nothing'
    Invoke-Update $install $good @('--nonsense')
    Check ($status -eq 2) 'an unknown option is a usage error'

    Check-Refused 'a wrong sha256' $install (New-Release 'hash' '99.0.0' $Fixture -Change { param($m) $m.artifact.sha256 = '0' * 64 }) 'does not match'
    Check-Refused 'a wrong size' $install (New-Release 'size' '99.0.0' $Fixture -Change { param($m) $m.artifact.bytes = 1 }) 'does not match'
    Check-Refused 'another platform' $install (New-Release 'platform' '99.0.0' $Fixture -Platform 'linux-x64') 'not windows-x64'
    Check-Refused 'a missing archive' $install (New-Release 'missing' '99.0.0' $Fixture -Change { param($m) $m.artifact.url = $m.artifact.url + '.absent' }) 'cannot download'
    Check-Refused 'a changed file' $install (New-Release 'tamper' '99.0.0' $Fixture -Prepare { param($f) Text (Join-Path $f 'Commands/new.seed') 'tampered' }) 'does not match its receipt'
    Check-Refused 'an unlisted file' $install (New-Release 'extra' '99.0.0' $Fixture -Prepare { param($f) Text (Join-Path $f 'extra.txt') 'x' }) 'does not name'
    Check-Refused 'a path out of the installation' $install (New-Release 'escape' '99.0.0' $Fixture -Prepare { param($f) Write-Receipt $f '99.0.0' @('../outside.txt') }) 'unsafe path'
    Check-Refused 'a shell that lies about its version' $install (New-Release 'lying' '99.0.0' $Current) 'does not report 99.0.0'
    Check-Refused 'a file already here that no release owns' $install (New-Release 'collide' '99.0.0' $Fixture -Prepare { param($f) Text (Join-Path $f 'mine.txt') 'x'; Write-Receipt $f '99.0.0' }) 'left as it is'

    $bare = Join-Path $root 'bare'
    New-Item -ItemType Directory -Force $bare | Out-Null
    Copy-Item -LiteralPath $Current (Join-Path $bare 'hirari.exe')
    Check-Refused 'a build folder with no receipt' $bare $good 'not a release installation'
    Check-Refused 'a folder pinned to its version' (New-Install $version $version) $good 'pinned'

    # The real thing, with another copy of the old shell still running from the installation.
    $running = Start-Process -FilePath (Join-Path $install 'hirari.exe') -ArgumentList '--eval', '"sleep 120"' -PassThru -WindowStyle Hidden
    try {
        Start-Sleep -Milliseconds 500
        Check (-not $running.HasExited) 'another shell is running from the installation'
        Invoke-Update $install $good
        Check ($status -eq 0 -and $output -ceq "UPDATED $version 99.0.0") "the installation updates ($status; $output)"
    } finally { Stop-Process -Id $running.Id -Force -ErrorAction SilentlyContinue; $running.WaitForExit() }
    Check ("$(@(& (Join-Path $install 'hirari.exe') --version)[0])" -ceq 'hirari 99.0.0') 'the installation runs the new shell'
    Check (Test-Path -LiteralPath (Join-Path $install 'Commands/new.seed')) 'the new release is in place'
    Check (-not (Test-Path -LiteralPath (Join-Path $install 'Commands/old.seed'))) 'what the old release alone had is gone'
    Check (-not (Test-Path -LiteralPath (Join-Path $install 'Gone'))) 'a folder the old release emptied is gone'
    Check ((Get-Content -LiteralPath (Join-Path $install 'mine.txt') -Raw) -ceq 'not part of any release') 'a file no release owns stays'
    Check (((Get-Content -LiteralPath (Join-Path $install 'release.receipt.json') -Raw) | ConvertFrom-Json).version -eq '99.0.0') 'the receipt names the new version'

    Invoke-Update $install $good
    Check ($status -eq 0 -and $output -ceq 'UP-TO-DATE 99.0.0') "the update is not repeated ($output)"
    Invoke-Update $install $good
    Check ($status -eq 0) 'the updated shell still updates'
} finally {
    Remove-Item Env:HIRARI_UPDATE_MANIFEST -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
"HIRARI_UPDATE_OK tests=$passed"
