#Requires -Version 7.2
<#
.SYNOPSIS
  A hirari release archive works on its own: unpacked far from any source or build folder, every file
  matches its receipt, and the packaged shell runs Seed, its library and ordinary programs.
#>
param(
    [Parameter(Mandatory)][string]$Archive,
    [Parameter(Mandatory)][string]$Version
)
$ErrorActionPreference = 'Stop'
$core = ($Version -replace '[-+].*$', '')
$root = Join-Path ([IO.Path]::GetTempPath()) ("hirari release " + [guid]::NewGuid().ToString('N').Substring(0, 8))
$passed = 0
function Check([bool]$Okay, [string]$Name) { if (-not $Okay) { throw "FAIL: $Name" }; $script:passed++; Write-Host "PASS $Name" }
try {
    Expand-Archive -LiteralPath $Archive -DestinationPath $root
    $receipt = Get-Content -LiteralPath (Join-Path $root 'release.receipt.json') -Raw | ConvertFrom-Json
    Check ($receipt.version -eq $Version) "the receipt names $Version"
    $bad = @($receipt.files | Where-Object { (Get-FileHash -LiteralPath (Join-Path $root $_.path) -Algorithm SHA256).Hash.ToLowerInvariant() -ne $_.sha256 })
    Check ($bad.Count -eq 0 -and @($receipt.files).Count -eq @(Get-ChildItem -LiteralPath $root -Recurse -File).Count - 1) 'every file matches the receipt, and nothing else is there'
    foreach ($file in 'hirari.exe', 'LICENSE', 'NOTICE', 'Commands/regex.seed', 'Licenses/regex-library.lock.json') {
        Check (Test-Path -LiteralPath (Join-Path $root $file) -PathType Leaf) "ships $file"
    }
    $hirari = Join-Path $root 'hirari.exe'
    Push-Location -LiteralPath ([IO.Path]::GetTempPath())
    try {
        Check ((@(& $hirari --version)[0]) -match "^hirari $([regex]::Escape($core))\b") "reports version $core"
        Check (("$(@(& $hirari --eval '(1 + 2)')[-1])").Trim() -eq '3') 'evaluates Seed'
        Check (((& $hirari --eval 'help -regex') -join "`n") -match '(?i)regex') 'finds its packaged Seed library'
        Check (((& $hirari --eval 'hostname') -join "`n") -match [regex]::Escape($env:COMPUTERNAME)) 'runs an ordinary program from PATH'
    } finally { Pop-Location }
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
"HIRARI_RELEASE_OK tests=$passed"
