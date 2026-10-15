[CmdletBinding()]
param([Parameter(Mandatory)][string]$Lake,[Parameter(Mandatory)][string]$Reference,
      [string]$Optimized,[Parameter(Mandatory)][string]$EvidenceDirectory)
$ErrorActionPreference='Stop'
$PSNativeCommandUseErrorActionPreference=$false
$Lake=[IO.Path]::GetFullPath($Lake);$Reference=[IO.Path]::GetFullPath($Reference)
$EvidenceDirectory=[IO.Path]::GetFullPath($EvidenceDirectory)
[IO.Directory]::CreateDirectory($EvidenceDirectory) | Out-Null
$summaryPath=Join-Path $EvidenceDirectory 'result.json'
if (Test-Path -LiteralPath $summaryPath) { Remove-Item -LiteralPath $summaryPath }
$oldPath=$env:PATH
function Run-Logged([string]$Executable,[string[]]$Arguments,[string]$Log,[bool]$ExpectFailure=$false) {
    $output=@(& $Executable @Arguments 2>&1);$code=$LASTEXITCODE;$text=($output | Out-String)
    [IO.File]::WriteAllText((Join-Path $EvidenceDirectory $Log),$text,[Text.UTF8Encoding]::new($false))
    if($ExpectFailure){if($code -eq 0){throw "Negative control unexpectedly passed: $Log"}}
    elseif($code -ne 0){throw "$Log failed ($code): $text"}
    Write-Host $text.TrimEnd()
    return $text
}
Push-Location $PSScriptRoot
try {
    $env:PATH=[IO.Path]::GetDirectoryName($Lake)+';'+$oldPath
    $lean=Join-Path ([IO.Path]::GetDirectoryName($Lake)) 'lean.exe'
    $version=Run-Logged $lean @('--version') 'lean-version.log'
    if($version -notmatch 'version 4\.19\.0\b'){throw 'Use the pinned formal/lean-toolchain.'}
    [void](Run-Logged $Lake @('build') 'lean-build.log')
    $audit=Run-Logged $Lake @('env','lean','-DwarningAsError=true','Audit.lean') 'lean-axioms.log'
    if($audit -match 'sorryAx'){throw 'A proof contains an unproved placeholder.'}
    foreach($item in [regex]::Matches($audit,'depends on axioms:\s*\[([^\]]*)\]')){
        foreach($name in ($item.Groups[1].Value -split ',')){
            if($name.Trim() -notin @('propext','Quot.sound','Classical.choice')){throw "Unexpected theorem axiom: $name"}
        }
    }
    if(([regex]::Matches($audit,'SeedSpec\.')).Count -lt 12){throw 'Incomplete theorem audit.'}
    $corpus=Join-Path $EvidenceDirectory 'conformance.tsv'
    [void](Run-Logged $Lake @('env','lean','-DwarningAsError=true','--run','Main.lean',$corpus) 'lean-corpus.log')
    $referenceResult=Run-Logged $Reference @($corpus) 'reference.log'
    $caseMatch=[regex]::Match($referenceResult,'LEAN_CPP_CONFORMANCE_OK (\d+) cases')
    if(!$caseMatch.Success){throw 'Missing reference conformance result.'}
    $caseCount=[int]$caseMatch.Groups[1].Value
    if($caseCount -lt 1000){throw 'Unexpectedly small conformance corpus.'}
    if($Optimized){[void](Run-Logged ([IO.Path]::GetFullPath($Optimized)) @($corpus) 'optimized.log')}
    $negativeProof=Join-Path $EvidenceDirectory 'WrongCapture.lean'
    [IO.File]::WriteAllText($negativeProof,@'
import SeedSpec
open SeedSpec
example : subst "x" (.symbol "y") (.abs (some "y") (.symbol "x")) =
    .abs (some "z") (.symbol "y") := by decide
'@,[Text.UTF8Encoding]::new($false))
    $negativeResult=Run-Logged $Lake @('env','lean','-DwarningAsError=true',$negativeProof) 'negative-proof.log' $true
    if($negativeResult -notmatch '(?s)decide.*false'){throw 'Negative proof failed for an unexpected reason.'}
    $wrongCorpus=Join-Path $EvidenceDirectory 'wrong-answer.tsv'
    [IO.File]::WriteAllText($wrongCorpus,"# seed-lean-conformance-v1`nR`t`t((x() x) -actual)`t`t-incorrect`n",[Text.UTF8Encoding]::new($false))
    $negativeCpp=Run-Logged $Reference @($wrongCorpus) 'negative-reference.log' $true
    if($negativeCpp -notmatch 'Lean conformance mismatch'){throw 'Native negative control failed for an unexpected reason.'}
    [IO.File]::WriteAllText((Join-Path $EvidenceDirectory 'result.json'),([ordered]@{
        toolchain=$version.Trim();casesPerReducer=$caseCount;reference=$Reference;optimized=$Optimized
        corpusSha256=(Get-FileHash -LiteralPath $corpus -Algorithm SHA256).Hash.ToLowerInvariant()
        negativeControls='incorrect capture theorem and incorrect native expected answer rejected'
        scope='Finite canonical abstraction/substitution model and C++ conformance; not complete C++ or host verification.'
    } | ConvertTo-Json -Depth 5),[Text.UTF8Encoding]::new($false))
    Write-Host "SEED_LEAN_VERIFICATION_OK $caseCount cases per reducer"
}finally{Pop-Location;$env:PATH=$oldPath}
