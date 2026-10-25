# Harden a public repository so that only the owner can change it.
#
# Transport: curl.exe (Schannel), deliberately NOT Invoke-RestMethod.
# Reason: this machine has a WinINET proxy configured (http://127.0.0.1:65532) that .NET's
# WebRequest honours; connections to github.com then time out while curl.exe (which ignores
# WinINET and reads only https_proxy) works fine. Using curl keeps the script working no
# matter what is in the system proxy settings.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File scripts/harden-repo.ps1 -Token "github_pat_..."
#
# Token: fine-grained PAT scoped to this repo with
#   Administration: Read and write   (rulesets)
#   Actions: Read and write          (workflow permissions)
# or a classic PAT with the "repo" scope.
#
# What it enforces (idempotent, safe to re-run):
#   1) branch ruleset on the default branch: block deletions + force pushes, admin keeps a bypass
#   2) tag ruleset for v*: block tag deletion + moving a published tag
#   3) GITHUB_TOKEN inside Actions becomes read-only
#
# Manual, one-time (a repo setting cannot replace these):
#   - Settings > Collaborators: keep the list empty (only you)
#   - Settings > Actions > General: require approval for all outside contributors
#   - Account: enable 2FA
param(
    [string]$Owner = "carrybb10",
    [string]$Repo = "learn1",
    [string]$Token = $env:GITHUB_TOKEN
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($Token)) {
    throw "No token. Pass -Token `"github_pat_...`" or set `$env:GITHUB_TOKEN in THE SAME window."
}
$Token = $Token.Trim().Trim('"').Trim("'")
if ($Token -match '\s') { throw "Token contains whitespace - it was pasted with a line break." }

$curl = Join-Path $env:SystemRoot "System32\curl.exe"
if (-not (Test-Path $curl)) { throw "curl.exe not found; Windows 10 1803+ ships it." }

$api = "https://api.github.com/repos/$Owner/$Repo"

# Invoke-Gh <method> <url> [jsonBody]  ->  parsed object, or $null on failure.
# Uses a temp file for the body so quoting can never corrupt the JSON.
function Invoke-Gh($method, $url, $body) {
    $tmp = $null
    $curlArgs = @("-sS", "--max-time", "45", "-X", $method,
              "-H", "Authorization: Bearer $Token",
              "-H", "Accept: application/vnd.github+json",
              "-H", "X-GitHub-Api-Version: 2022-11-28",
              "-H", "User-Agent: mediaplayer-harden",
              "-w", "`n%{http_code}")
    if ($null -ne $body) {
        $tmp = [IO.Path]::Combine($env:TEMP, "ghbody-$([guid]::NewGuid().ToString('N')).json")
        [IO.File]::WriteAllText($tmp, ($body | ConvertTo-Json -Depth 10), (New-Object Text.UTF8Encoding($false)))
        $curlArgs += @("--data-binary", "@$tmp", "-H", "Content-Type: application/json")
    }
    $curlArgs += $url

    $out = & $curl @curlArgs 2>&1
    if ($tmp) { Remove-Item $tmp -ErrorAction SilentlyContinue }

    $text = ($out | ForEach-Object { "$_" }) -join "`n"
    $lines = $text -split "`n"
    $code = $lines[-1].Trim()
    $payload = ($lines[0..([Math]::Max(0, $lines.Count - 2))]) -join "`n"

    if ($code -match '^\d{3}$') {
        $status = [int]$code
        if ($status -ge 200 -and $status -lt 300) {
            if ([string]::IsNullOrWhiteSpace($payload)) { return @{} }
            return ($payload | ConvertFrom-Json)
        }
        Write-Host "  ! $method $url -> HTTP $status : $payload" -ForegroundColor Yellow
        switch ($status) {
            401 { Write-Host "    401: the token itself was rejected (wrong value / expired / pasted with extra characters)." -ForegroundColor Yellow }
            403 { Write-Host "    403: token is valid but lacks Administration (read+write) on this repo." -ForegroundColor Yellow }
            404 { Write-Host "    404: token cannot see this repo (repository selection of the PAT)." -ForegroundColor Yellow }
        }
        return $null
    }

    Write-Host "  ! $method $url -> transport failure: $payload" -ForegroundColor Red
    Write-Host "    Check: curl.exe -sS -o NUL -w ""%{http_code}"" https://api.github.com/rate_limit" -ForegroundColor Yellow
    return $null
}

# --- preflight ---------------------------------------------------------------
Write-Host "preflight:"
$me = Invoke-Gh "GET" "https://api.github.com/user"
if (-not $me) { throw "Cannot authenticate. Fix the token first; nothing was changed." }
Write-Host "  authenticated as: $($me.login)"

$repoInfo = Invoke-Gh "GET" $api
if (-not $repoInfo) { throw "Cannot read $Owner/$Repo with this token." }
Write-Host "  repo: $($repoInfo.full_name) private=$($repoInfo.private) default=$($repoInfo.default_branch) admin=$($repoInfo.permissions.admin)"
if (-not $repoInfo.permissions.admin) {
    throw "Token can read the repo but not administer it: grant Administration: Read and write."
}

$bypass = @(@{ actor_id = 5; actor_type = "RepositoryRole"; bypass_mode = "always" })

function Ensure-Ruleset($name, $target, $includes, $rules) {
    $existing = (Invoke-Gh "GET" "$api/rulesets") | Where-Object { $_.name -eq $name } | Select-Object -First 1
    $payload = @{
        name          = $name
        target        = $target
        enforcement   = "active"
        conditions    = @{ ref_name = @{ include = $includes; exclude = @() } }
        rules         = $rules
        bypass_actors = $bypass
    }
    if ($existing) {
        $r = Invoke-Gh "PUT" "$api/rulesets/$($existing.id)" $payload
        if ($r) { Write-Host "  updated ruleset: $name (id $($r.id))" }
    } else {
        $r = Invoke-Gh "POST" "$api/rulesets" $payload
        if ($r) { Write-Host "  created ruleset: $name (id $($r.id))" }
    }
}

Write-Host "1/3 branch ruleset for the default branch"
Ensure-Ruleset "protect-main" "branch" @("~DEFAULT_BRANCH") `
    @(@{ type = "deletion" }, @{ type = "non_fast_forward" })

Write-Host "2/3 tag ruleset for v*"
Ensure-Ruleset "protect-tags" "tag" @("refs/tags/v*") `
    @(@{ type = "deletion" }, @{ type = "non_fast_forward" })

Write-Host "3/3 GITHUB_TOKEN inside Actions -> read-only"
$r = Invoke-Gh "PUT" "$api/actions/permissions/workflow" @{
    default_workflow_permissions     = "read"
    can_approve_pull_request_reviews = $false
}
if ($null -ne $r) { Write-Host "  workflow token permissions: read-only" }

Write-Host ""
Write-Host "current rulesets:"
(Invoke-Gh "GET" "$api/rulesets") | ForEach-Object { "  - $($_.name) [$($_.target)/$($_.enforcement)]" }
