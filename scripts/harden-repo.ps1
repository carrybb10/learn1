# Harden a public repository so that only the owner can change it.
#
# Usage (recommended: pass the token explicitly so a stale env var cannot break it):
#   powershell -ExecutionPolicy Bypass -File scripts/harden-repo.ps1 -Token "github_pat_..."
#
# Or set it for the current window only:
#   $env:GITHUB_TOKEN = "github_pat_..."
#   powershell -ExecutionPolicy Bypass -File scripts/harden-repo.ps1
#
# Token needs: fine-grained PAT scoped to the repo with
#   Administration: Read and write  (rulesets)  +  Actions: Read and write (workflow permissions)
# or a classic PAT with the "repo" scope.
#
# What it enforces (all idempotent, safe to re-run):
#   1) branch ruleset on the default branch: block deletions + force pushes, owner keeps a bypass
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
try { [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 } catch {}

if ([string]::IsNullOrWhiteSpace($Token)) {
    throw "No token. Pass -Token `"github_pat_...`" or set `$env:GITHUB_TOKEN in THE SAME window."
}
$Token = $Token.Trim().Trim('"').Trim("'")
if ($Token -match '\s') { throw "Token contains whitespace - it was probably pasted with a line break." }
if (-not ($Token -match '^(ghp_[A-Za-z0-9]{36}|github_pat_[A-Za-z0-9_]+|gho_|ghu_|ghs_|ghr_)')) {
    Write-Host "warning: token does not look like a GitHub PAT (expected ghp_... or github_pat_...)" -ForegroundColor Yellow
}

$headers = @{
    Authorization          = "Bearer $Token"
    Accept                 = "application/vnd.github+json"
    "X-GitHub-Api-Version" = "2022-11-28"
    "User-Agent"           = "mediaplayer-harden"
}
$api = "https://api.github.com/repos/$Owner/$Repo"

function Invoke-Api($method, $uri, $body) {
    $params = @{ Method = $method; Uri = $uri; Headers = $headers }
    if ($body) {
        $params.Body = ($body | ConvertTo-Json -Depth 10)
        $params.ContentType = "application/json"
    }
    try {
        return Invoke-RestMethod @params
    } catch {
        $status = $null
        if ($_.Exception.Response) { $status = [int]$_.Exception.Response.StatusCode }
        Write-Host "  ! $method $uri -> HTTP $status $($_.Exception.Message)" -ForegroundColor Yellow
        if ($status -eq 401) {
            Write-Host "    401 means the token itself was rejected (wrong value, expired, or copied with extra characters)." -ForegroundColor Yellow
            Write-Host "    Verify with: curl -H `"Authorization: Bearer <token>`" https://api.github.com/user" -ForegroundColor Yellow
        }
        if ($status -eq 403) {
            Write-Host "    403 means the token is valid but lacks permission (Administration: write) or the repo." -ForegroundColor Yellow
        }
        return $null
    }
}

# --- preflight: who am I, and can I administer this repo? -------------------
Write-Host "preflight:"
$me = Invoke-Api "GET" "https://api.github.com/user"
if (-not $me) { throw "Token rejected by GitHub - fix the token before running the rest." }
Write-Host "  authenticated as: $($me.login)"

$repoInfo = Invoke-Api "GET" $api
if (-not $repoInfo) { throw "Token cannot read $Owner/$Repo - check the repository selection of the PAT." }
Write-Host "  repo: $($repoInfo.full_name) (private=$($repoInfo.private), default=$($repoInfo.default_branch), admin=$($repoInfo.permissions.admin))"
if (-not $repoInfo.permissions.admin) {
    throw "Token can read the repo but not administer it. Grant Administration: Read and write (fine-grained) or the repo scope (classic)."
}

$bypass = @(@{ actor_id = 5; actor_type = "RepositoryRole"; bypass_mode = "always" })

function Ensure-Ruleset($name, $target, $includes, $rules) {
    $existing = (Invoke-Api "GET" "$api/rulesets") | Where-Object { $_.name -eq $name } | Select-Object -First 1
    $payload = @{
        name          = $name
        target        = $target
        enforcement   = "active"
        conditions    = @{ ref_name = @{ include = $includes; exclude = @() } }
        rules         = $rules
        bypass_actors = $bypass
    }
    if ($existing) {
        $r = Invoke-Api "PUT" "$api/rulesets/$($existing.id)" $payload
        if ($r) { Write-Host "  updated ruleset: $name (id $($r.id))" }
    } else {
        $r = Invoke-Api "POST" "$api/rulesets" $payload
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
$r = Invoke-Api "PUT" "$api/actions/permissions/workflow" @{
    default_workflow_permissions     = "read"
    can_approve_pull_request_reviews = $false
}
if ($r -ne $null) { Write-Host "  workflow token permissions: read-only" }

Write-Host ""
Write-Host "current rulesets:"
(Invoke-Api "GET" "$api/rulesets") | ForEach-Object { "  - $($_.name) [$($_.target)/$($_.enforcement)]" }
