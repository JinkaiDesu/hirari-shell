#Requires -Version 7.2
<#
.SYNOPSIS
  The Linux half of the Checks. Sends exactly this checkout's files to the Linux build machine, then builds, tests and
  packages them there in its Ubuntu 24.04 chroot, tries the archive in its clean Debian 13 chroot, and brings the
  archive back to -Out. The last line is HIRARI_LINUX_OK with the test counts.

.DESCRIPTION
  The machine comes from HIRARI_LINUX_HOST (user@host) and HIRARI_LINUX_KEY (an SSH key's path), in the process or user
  environment. It holds /srv/ubuntu-24.04 (build-essential, cmake, ninja-build, git, curl) and /srv/debian-13.
  Releases are built on the older system so that they run on it and on anything newer.

.EXAMPLE
  pwsh -NoProfile -File tools/linux.ps1 -Out out
#>
param(
    [string]$Source = (Split-Path $PSScriptRoot -Parent),
    [string]$Out = 'out'
)
$ErrorActionPreference = 'Stop'
function Get-Setting([string]$Name) {
    $value = [Environment]::GetEnvironmentVariable($Name)
    if (-not $value) { $value = [Environment]::GetEnvironmentVariable($Name, 'User') }
    if (-not $value) { throw "the Linux check needs $Name in the process or user environment" }
    $value
}
$hostName = Get-Setting 'HIRARI_LINUX_HOST'
$key = Get-Setting 'HIRARI_LINUX_KEY'
$known = Join-Path (Split-Path $key -Parent) 'known_hosts'
$ssh = @('-i', $key, '-o', 'IdentitiesOnly=yes', '-o', 'BatchMode=yes', '-o', "UserKnownHostsFile=$known")

$Source = [IO.Path]::GetFullPath($Source)
$Out = [IO.Path]::GetFullPath($Out, $Source)
$id = [guid]::NewGuid().ToString('N').Substring(0, 12)
$list = Join-Path ([IO.Path]::GetTempPath()) "hirari-linux-$id.list"
$tar = Join-Path ([IO.Path]::GetTempPath()) "hirari-linux-$id.tar"
try {
    # Tracked and new files, as the checkout has them right now.
    $files = @(git -C $Source ls-files --cached --others --exclude-standard | Where-Object { Test-Path -LiteralPath (Join-Path $Source $_) -PathType Leaf })
    if ($LASTEXITCODE) { throw 'git ls-files failed' }
    [IO.File]::WriteAllLines($list, [string[]]$files)
    & "$env:SystemRoot\System32\tar.exe" -cf $tar -C $Source -T $list
    if ($LASTEXITCODE) { throw 'tar failed' }
    scp @ssh -q $tar "${hostName}:/tmp/hirari-$id.tar"
    if ($LASTEXITCODE) { throw "cannot reach $hostName" }

    $remote = @'
set -e
ID=__ID__
BUILDER=/srv/ubuntu-24.04
CLEAN=/srv/debian-13
WORK=$BUILDER/work/$ID
cleanup() { rm -rf "$WORK" "$CLEAN/work/$ID" "/tmp/hirari-$ID.tar"; }
trap cleanup EXIT
mkdir -p "$WORK" "$CLEAN/work/$ID"
tar -xf "/tmp/hirari-$ID.tar" -C "$WORK"
find "$WORK" -name '*.sh' -o -name '*.seed' | xargs -r sed -i 's/\r$//'
unshare --mount sh -e -c '
for ROOT in "$0" "$1"; do
    mount --rbind /dev "$ROOT/dev"
    mount -t proc proc "$ROOT/proc"
    mount -t tmpfs tmpfs "$ROOT/tmp"
done
chroot "$0" /usr/bin/env -i HOME=/root PATH=/usr/bin:/bin TMPDIR=/tmp HIRARI_STATE=- sh -e -c "
    cd /work/$2
    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
    ninja -C build >/dev/null
    ctest --test-dir build --output-on-failure --timeout 200
    build/hirari tools/package.seed build out
"
ARCHIVE=$(ls "$0/work/$2/out/"hirari-*-linux-x64.tar.gz)
VERSION=$(basename "$ARCHIVE" | sed "s/hirari-//;s/-linux-x64.tar.gz//")
mkdir "$1/work/$2/hirari"
tar -xf "$ARCHIVE" -C "$1/work/$2/hirari"
cp "$0/work/$2/tests/release.tests.seed" "$ARCHIVE" "$1/work/$2/"
chroot "$1" /usr/bin/env -i HOME=/root PATH=/work/$2/hirari:/usr/bin:/bin TMPDIR=/tmp HIRARI_STATE=- sh -e -c "
    cd /work/$2
    hirari release.tests.seed /work/$2/$(basename "$ARCHIVE") $VERSION
" | tail -1 | sed "s/^/CLEAN_SYSTEM /"
cp "$ARCHIVE" "/tmp/hirari-$2.tar.gz"
' "$BUILDER" "$CLEAN" "$ID"
'@.Replace('__ID__', $id).Replace("`r`n", "`n")
    $script = Join-Path ([IO.Path]::GetTempPath()) "hirari-linux-$id.sh"
    [IO.File]::WriteAllText($script, $remote, [Text.UTF8Encoding]::new($false))
    try {
        scp @ssh -q $script "${hostName}:/tmp/hirari-$id.sh"
        if ($LASTEXITCODE) { throw "cannot reach $hostName" }
    } finally { Remove-Item -LiteralPath $script -ErrorAction SilentlyContinue }
    $output = & ssh @ssh $hostName "sh /tmp/hirari-$id.sh; status=`$?; rm -f /tmp/hirari-$id.sh; exit `$status" 2>&1 | ForEach-Object { "$_" }
    if ($LASTEXITCODE) { throw "the Linux check failed:`n$(($output | Select-Object -Last 40) -join "`n")" }
    $tests = ($output | Select-String '\d+% tests passed.*' | Select-Object -Last 1).Matches.Value
    $release = ($output | Select-String '^HIRARI_RELEASE_OK tests=\d+').Matches.Value | Select-Object -Last 1
    $clean = ($output | Select-String '^CLEAN_SYSTEM (HIRARI_RELEASE_OK tests=\d+)').Matches.Value | Select-Object -Last 1
    if (-not $tests -or -not $release -or -not $clean) { throw "the Linux check gave no result:`n$(($output | Select-Object -Last 20) -join "`n")" }

    $cmake = Get-Content -LiteralPath (Join-Path $Source 'CMakeLists.txt') -Raw
    if ($cmake -notmatch 'project\(Hirari VERSION ([0-9.]+)') { throw 'CMakeLists.txt names no hirari version' }
    New-Item -ItemType Directory -Force $Out | Out-Null
    $archive = Join-Path $Out "hirari-$($Matches[1])-linux-x64.tar.gz"
    scp @ssh -q "${hostName}:/tmp/hirari-$id.tar.gz" $archive
    if ($LASTEXITCODE) { throw 'cannot bring the Linux archive back' }
    & ssh @ssh $hostName "rm -f /tmp/hirari-$id.tar.gz" | Out-Null
    "packaged $archive"
    "HIRARI_LINUX_OK $tests, $release, $clean"
} finally {
    Remove-Item -LiteralPath $list, $tar -ErrorAction SilentlyContinue
}
