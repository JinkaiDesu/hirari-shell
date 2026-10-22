#!/usr/bin/env pwsh
#Requires -Version 7.2
<#
.SYNOPSIS
  hirari's pipeline: start a topic, stage small changes onto its pull request, or ship pull requests to live.

.DESCRIPTION
  start <topic>                     Make (or reuse) .worktrees/<topic> on agent/<topic>: resumed from
                                    origin/agent/<topic> if it exists, else new from origin/main. Change files only there.
  stage <topic> -Message <text>     Accumulate: run the checks in the topic's worktree (configure, build, ctest,
                                    tools/package.ps1), commit everything in it, push, and open or update its pull
                                    request. Nothing is merged or published, so repeated stages pile up on one PR.
  ship <topic|number>... [-Message <text>] [-DryRun]
                                    The whole pipeline, now. With -Message, the one topic named is staged first. Then,
                                    for each pull request in order:
                                      1. check its exact head in a throwaway checkout (.worktrees/.ship-<n>);
                                      2. gate it: head unchanged, GitHub reports it mergeable, nothing reported pending
                                         or failing;
                                      3. merge exactly that head (squash, --match-head-commit).
                                    Then it fast-forwards the main checkout and checks it again, publishes through
                                    tools/publish.ps1 when the merged files change the shipped payload (a version
                                    already live is left alone), and retires each topic's worktree and local branch.
                                    -DryRun stops after the gates.

  Exit 0 ends with STAGED, SHIPPED or DRY RUN; exit 1 prints BLOCKED with the stage and why, and nothing after a blocked
  step runs. The Cloudflare token is CLOUDFLARE_API_TOKEN (process, else user environment) and is never printed.

.EXAMPLE
  pwsh -NoProfile -File tools/pipeline.ps1 start readme-wording
  pwsh -NoProfile -File tools/pipeline.ps1 stage readme-wording -Message "README: tighten the build section"
  pwsh -NoProfile -File tools/pipeline.ps1 ship readme-wording
  pwsh -NoProfile -File tools/pipeline.ps1 ship completion-fix -Message "Shell: fix completion of quoted paths"
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory, Position = 0)][ValidateSet('start', 'stage', 'ship')][string]$Command,
    [Parameter(Mandatory, Position = 1, ValueFromRemainingArguments)][string[]]$Target,
    [string]$Message,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$env:GIT_TERMINAL_PROMPT = '0'
$env:GH_PROMPT_DISABLED = '1'

$Repo = 'JinkaiDesu/hirari-shell'
# Files that end up in the release archive; a merge touching none of them publishes nothing.
$Payload = '^(hirari/|seed/|library/|cmake/|Licenses/|CMakeLists\.txt$|LICENSE$|NOTICE$|README\.md$)'
# The main checkout, wherever this copy of the script lives (a topic worktree included).
$Root = Split-Path -Parent (git -C $PSScriptRoot rev-parse --path-format=absolute --git-common-dir)
$Clock = [Diagnostics.Stopwatch]::StartNew()

# To the console, so that a function's return value stays its own.
function Log([string]$Text) { Write-Host ('[{0,5:n0}s] {1}' -f $Clock.Elapsed.TotalSeconds, $Text) }
function Block([string]$Stage, [string]$Why) { Log "BLOCKED at ${Stage}: $Why"; exit 1 }
function Invoke-Git([string]$Dir) { $out = git -C $Dir @args 2>&1; if ($LASTEXITCODE) { throw "git $($args -join ' '): $out" }; $out }
function Get-Pull($Ref) { gh pr view $Ref -R $Repo --json number,state,headRefOid,headRefName,mergeable,statusCheckRollup,files,mergeCommit | ConvertFrom-Json }
function Get-TopicPath([string]$Topic) { Join-Path $Root ".worktrees\$Topic" }

function Assert-Topic([string]$Topic) {
    if ($Topic -cnotmatch '^[a-z0-9][a-z0-9-]{0,47}$') { Block 'topic' "a topic is lowercase words joined by '-': '$Topic'" }
}

# hirari's own semver, from CMakeLists.txt text; and the version its latest manifest names now (none before the first).
function Get-Version([string]$Text) { if ($Text -match 'project\(Hirari VERSION ([0-9]+\.[0-9]+\.[0-9]+)') { [version]$Matches[1] } else { throw 'CMakeLists.txt names no hirari version' } }
function Get-LiveVersion {
    try { [version](Invoke-RestMethod -Uri "https://downloads.emergencetech.co/hirari/manifest.json?v=$([guid]::NewGuid().ToString('N'))" -TimeoutSec 30).version } catch {
        if ($_.Exception.Response.StatusCode -eq 404) { return $null }; throw
    }
}

# A topic that changes the shipped payload ships a new version. If main's version is already live and the topic hasn't
# moved past it, bump the patch here; a minor or major bump is a deliberate edit of CMakeLists.txt.
function Update-Version([string]$Topic, [string]$Dir) {
    Invoke-Git $Dir fetch -q origin main | Out-Null
    $changed = @(Invoke-Git $Dir diff --name-only origin/main) + @(Invoke-Git $Dir ls-files --others --exclude-standard)
    if (-not @($changed | Where-Object { $_ -match $Payload }).Count) { return }
    $cmakeFile = Join-Path $Dir 'CMakeLists.txt'
    $text = [IO.File]::ReadAllText($cmakeFile)
    $mine = Get-Version $text
    $main = Get-Version ((Invoke-Git $Dir show origin/main:CMakeLists.txt) -join "`n")
    if ($mine -gt $main) { Log "version ${Topic}: $mine (past main's $main)"; return }
    $live = Get-LiveVersion
    if (-not $live -or $live -lt $main) { Log "version ${Topic}: $main (not published yet)"; return }
    $next = [version]::new($main.Major, $main.Minor, $main.Build + 1)
    [IO.File]::WriteAllText($cmakeFile, ($text -replace 'project\(Hirari VERSION [0-9.]+', "project(Hirari VERSION $next"))
    Log "version ${Topic}: $main is live, bumped to $next"
}

# The Checks from CLAUDE.md in $Dir (build/ and out/ inside it); returns their summary or throws.
function Test-Checkout([string]$Dir) {
    Push-Location -LiteralPath $Dir
    try {
        $out = cmake -S . -B build 2>&1; if ($LASTEXITCODE) { throw "cmake configure failed: $(($out | Select-Object -Last 5) -join ' ')" }
        $out = cmake --build build --config Release 2>&1
        if ($LASTEXITCODE) { throw "build failed: $(($out | Select-String ': (fatal )?error ' | Select-Object -First 5) -join '; ')" }
        $out = ctest --test-dir build -C Release --output-on-failure 2>&1
        $tests = ($out | Select-String '\d+% tests passed.*' | Select-Object -Last 1).Matches.Value
        if ($LASTEXITCODE) {
            # --output-on-failure prints each failed test's own output right after its status line; keep that evidence.
            $evidence = [Collections.Generic.List[string]]::new(); $inside = $false
            foreach ($line in $out) {
                if ("$line" -match '\*\*\*(Failed|Exception|Timeout)') { $inside = $true; $evidence.Add("$line".Trim()); continue }
                if ("$line" -match '^\s*(Start\s+\d+:|\d+/\d+ Test|\d+% tests passed)') { $inside = $false; continue }
                if ($inside -and "$line".Trim()) { $evidence.Add("$line".Trim()) }
            }
            throw "tests failed ($tests): $(($evidence | Select-Object -First 12) -join ' | ')"
        }
        $out = pwsh -NoProfile -File tools/package.ps1 -Build build -Out out 2>&1
        $release = ($out | Select-String 'HIRARI_RELEASE_OK tests=\d+').Matches.Value
        if ($LASTEXITCODE -or -not $release) { throw "package failed: $(($out | Select-Object -Last 5) -join ' ')" }
        "$tests, $release"
    } finally { Pop-Location }
}

# Reported checks that block a merge: anything not passed.
function Get-Blocking($Pull) {
    @($Pull.statusCheckRollup | ForEach-Object {
        $result = if ($_.conclusion) { $_.conclusion } elseif ($_.state) { $_.state } else { $_.status }
        if ($result -notin 'SUCCESS', 'NEUTRAL', 'SKIPPED') { "$(if ($_.context) { $_.context } else { $_.name })=$result" }
    })
}

function Start-Topic([string]$Topic) {
    Assert-Topic $Topic
    $dir = Get-TopicPath $Topic
    if (Test-Path -LiteralPath $dir) { Log "topic ${Topic}: $dir (existing, agent/$Topic)"; return }
    Invoke-Git $Root fetch -q origin main | Out-Null
    $remote = Invoke-Git $Root ls-remote --heads origin "refs/heads/agent/$Topic"
    if ($remote) {
        Invoke-Git $Root fetch -q origin "+refs/heads/agent/${Topic}:refs/remotes/origin/agent/$Topic" | Out-Null
        Invoke-Git $Root worktree add -q -B "agent/$Topic" $dir "origin/agent/$Topic" | Out-Null
        Log "topic ${Topic}: $dir (resumed from origin/agent/$Topic)"
    } else {
        Invoke-Git $Root worktree add -q -b "agent/$Topic" $dir origin/main | Out-Null
        Log "topic ${Topic}: $dir (new from origin/main)"
    }
}

# Checks, commits and pushes the topic's worktree, and opens its pull request if it has none. Returns its number.
function Push-Topic([string]$Topic, [string]$Text) {
    Assert-Topic $Topic
    $dir = Get-TopicPath $Topic
    if (-not (Test-Path -LiteralPath $dir)) { Block 'stage' "no worktree for $Topic; run start $Topic and make the change there" }
    try { Update-Version $Topic $dir } catch { Block "stage $Topic version" $_.Exception.Message }
    try { $summary = Test-Checkout $dir } catch { Block "stage $Topic checks" $_.Exception.Message }
    Log "checked ${Topic}: $summary"
    if (Invoke-Git $dir status --porcelain --untracked-files=all) {
        if (-not $Text) { Block 'stage' "$Topic has uncommitted changes; give -Message to commit them" }
        Invoke-Git $dir add -A | Out-Null
        $file = Join-Path ([IO.Path]::GetTempPath()) "commit-$Topic.txt"
        [IO.File]::WriteAllText($file, "$Text`n`nCo-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`n")
        try { Invoke-Git $dir commit -q -F $file | Out-Null } finally { Remove-Item -LiteralPath $file }
        Log "committed: $Text"
    }
    $ahead = Invoke-Git $dir rev-list --count origin/main..HEAD
    if ([int]"$ahead" -eq 0) { Block 'stage' "$Topic has no commits beyond origin/main" }
    try { Invoke-Git $dir push -q -u origin "agent/$Topic" | Out-Null } catch { Block 'push' $_.Exception.Message }
    $open = gh pr list -R $Repo --head "agent/$Topic" --state open --json number --jq '.[0].number'
    if ($open) { Log "pushed agent/$Topic; PR #$open updated"; return [int]$open }
    $log = (Invoke-Git $dir log --no-merges --format='- %s' origin/main..HEAD) -join "`n"
    $title = (Invoke-Git $dir log --no-merges --format=%s -1 HEAD) | Select-Object -First 1
    $body = "Topic ``$Topic``.`n`n$log`n`nDependencies: none. Verification: this repository has no CI; the checks passed on this head ($summary).`n`n🤖 Generated with [Claude Code](https://claude.com/claude-code)`n"
    $file = Join-Path ([IO.Path]::GetTempPath()) "pr-$Topic.md"
    [IO.File]::WriteAllText($file, $body)
    try { $url = gh pr create -R $Repo --base main --head "agent/$Topic" --title $title --body-file $file 2>&1 } finally { Remove-Item -LiteralPath $file }
    if ($LASTEXITCODE) { Block 'pull request' "$($url -join ' ')" }
    $url = "$($url | Select-Object -Last 1)".Trim()
    Log "pushed agent/$Topic; opened $url"
    [int]($url -split '/')[-1]
}

function Ship([object[]]$Refs) {
    $files = [Collections.Generic.List[string]]::new()
    $topics = [Collections.Generic.List[string]]::new()
    $merged = [Collections.Generic.List[string]]::new()

    foreach ($ref in $Refs) {
        $p = if ("$ref" -match '^\d+$') { Get-Pull $ref } else { Get-Pull "agent/$ref" }
        if (-not $p) { Block "PR $ref" 'no pull request found' }
        $n = $p.number
        if ($p.state -ne 'OPEN') { Block "PR #$n" "it is $($p.state)" }
        $head = $p.headRefOid
        Log "PR #$n $($p.headRefName) at $($head.Substring(0, 9))"

        $scratch = Join-Path $Root ".worktrees\.ship-$n"
        Invoke-Git $Root fetch -q origin "+refs/heads/$($p.headRefName):refs/remotes/origin/$($p.headRefName)" | Out-Null
        if (Test-Path -LiteralPath $scratch) { Invoke-Git $Root worktree remove --force $scratch | Out-Null }
        Invoke-Git $Root worktree add -q --detach $scratch $head | Out-Null
        try { $summary = Test-Checkout $scratch } catch { Block "PR #$n checks" $_.Exception.Message } finally { Invoke-Git $Root worktree remove --force $scratch | Out-Null }
        Log "  checked: $summary"

        for ($i = 0; ($p = Get-Pull $n).mergeable -eq 'UNKNOWN' -and $i -lt 20; $i++) { Start-Sleep 3 }
        if ($p.headRefOid -ne $head) { Block "PR #$n gate" "the head moved to $($p.headRefOid) after it was checked" }
        if ($p.mergeable -ne 'MERGEABLE') { Block "PR #$n gate" "GitHub reports it $($p.mergeable); bring origin/main into $($p.headRefName) and resolve" }
        $blocking = Get-Blocking $p
        if ($blocking) { Block "PR #$n gate" "reported checks not passed: $($blocking -join ', ')" }
        if (@($p.files | Where-Object { $_.path -match $Payload }).Count) {
            $version = Get-Version ((Invoke-Git $Root show "${head}:CMakeLists.txt") -join "`n")
            $live = Get-LiveVersion
            if ($live -and $version -le $live) { Block "PR #$n gate" "it changes the shipped payload at version $version, but $live is live; stage it again to bump the version" }
        }
        $p.files | ForEach-Object { $files.Add($_.path) }
        if ($p.headRefName -match '^agent/(.+)$') { $topics.Add($Matches[1]) }

        if ($DryRun) { Log '  gate passed (dry run: not merged)'; continue }
        $out = gh pr merge $n -R $Repo --squash --match-head-commit $head 2>&1
        if ($LASTEXITCODE) { Block "PR #$n merge" "$($out -join ' ')" }
        $merged.Add("#$n")
        Log "  merged as $((Get-Pull $n).mergeCommit.oid.Substring(0, 9))"
    }
    if ($DryRun) { Log "DRY RUN passed for $($Refs -join ', '): every head checked and gated"; return }

    if (git -C $Root status --porcelain --untracked-files=all) { Block 'main checkout' "$Root has local changes; it was not updated or published" }
    Invoke-Git $Root fetch -q origin main | Out-Null
    try { Invoke-Git $Root merge -q --ff-only origin/main | Out-Null } catch { Block 'main checkout' $_.Exception.Message }
    $main = "$(Invoke-Git $Root rev-parse HEAD)".Trim()
    try { $summary = Test-Checkout $Root } catch { Block 'main checks' $_.Exception.Message }
    Log "main at $($main.Substring(0, 9)): $summary"

    $what = 'nothing to publish'
    if (@($files | Where-Object { $_ -match $Payload }).Count) {
        $out = pwsh -NoProfile -File (Join-Path $Root 'tools\publish.ps1') 2>&1
        if ($LASTEXITCODE) { Block 'publish' (($out | Select-Object -Last 3) -join ' ') }
        $what = "$($out | Select-Object -Last 1)".Trim()
        Log $what
    }

    foreach ($t in $topics) {
        $dir = Get-TopicPath $t
        if (Test-Path -LiteralPath $dir) {
            try { Invoke-Git $Root worktree remove $dir | Out-Null } catch { Log "kept .worktrees\${t}: $($_.Exception.Message)"; continue }
        }
        git -C $Root branch -q -D "agent/$t" 2>$null
        git -C $Root push -q origin --delete "agent/$t" 2>$null
        Log "retired $t"
    }
    Log "SHIPPED $($merged -join ', ') -> main $($main.Substring(0, 9)); $what"
}

switch ($Command) {
    'start' { foreach ($t in $Target) { Start-Topic $t } }
    'stage' {
        if (-not $Message) { Block 'stage' 'give -Message for the commit' }
        foreach ($t in $Target) { $n = Push-Topic $t $Message; Log "STAGED $t on PR #$n (not merged, not published)" }
    }
    'ship' {
        if ($Message) {
            if ($Target.Count -ne 1 -or $Target[0] -match '^\d+$') { Block 'ship' '-Message stages one topic, named by its topic' }
            Push-Topic $Target[0] $Message | Out-Null
        }
        Ship $Target
    }
}
