# Harden a public repository so that only the owner can change it.
# Run locally with a PAT that has admin rights on the repo (never commit the token):
#   $env:GITHUB_TOKEN = "ghp_..."
#   powershell -ExecutionPolicy Bypass -File scripts/harden-repo.ps1
#
# What it enforces via API (all idempotent, safe to re-run):
#   1) branch ruleset on the default branch: block deletions + force pushes, owner keeps a bypass
#   2) tag ruleset for v*: block tag deletion + moving a published tag
#   3) GITHUB_TOKEN inside Actions becomes read-only (a compromised workflow cannot push)
#
# What it cannot do (manual, one-time, 5 minutes in the repo settings UI):
#   - Settings > Collaborators: keep the list empty (only you)
#   - Settings > Actions > General > Fork pull request workflows: require approval for all outside contributors
#   - Account > Password and authentication: enable 2FA (a repo setting cannot protect a stolen password)
param(
    [string]$Owner = "carrybb10",
    [string]$Repo = "learn1"
)

$ErrorActionPreference = "Stop"
if (-not $env:GITHUB_TOKEN) { throw "GITHUB_TOKEN is not set" }
try { [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 } catch {}

$api = "https://api.github.com/repos/$Owner/$Repo"
$headers = @{
    Authorization          = "token $($env:GITHUB_TOKEN)"
    Accept                 = "application/vnd.github+json"
    "X-GitHub-Api-Version" = "2022-11-28"
    "User-Agent"           = "mediaplayer-harden"
}

function Invoke-Api($method, $uri, $body) {
    $params = @{ Method = $method; Uri = $uri; Headers = $headers; ErrorAction = "Stop" }
    if ($body) {
        $params.Body = ($body | ConvertTo-Json -Depth 10)
        $params.ContentType = "application/json"
    }
    try {
        return Invoke-RestMethod @params
    } catch {
        Write-Host "  ! $method $uri -> $($_.Exception.Message)" -ForegroundColor Yellow
        return $null
    }
}

# Repository role id 5 = admin; only admins (you) can bypass the rules.
$bypass = @(@{ actor_id = 5; actor_type = "RepositoryRole"; bypass_mode = "always" })

function Ensure-Ruleset($name, $target, $includes, $rules, $bypassList) {
    $existing = (Invoke-Api "GET" "$api/rulesets") | Where-Object { $_.name -eq $name } | Select-Object -First 1
    $payload = @{
        name          = $name
        target        = $target
        enforcement   = "active"
        conditions    = @{ ref_name = @{ include = $includes; exclude = @() } }
        rules         = $rules
        bypass_actors = $bypassList
    }
    if ($existing) {
        $r = Invoke-Api "PUT" "$api/rulesets/$($existing.id)" $payload
        if ($r) { Write-Host "updated ruleset: $name (id $($r.id))" }
    } else {
        $r = Invoke-Api "POST" "$api/rulesets" $payload
        if ($r) { Write-Host "created ruleset: $name (id $($r.id))" }
    }
}

Write-Host "1/3 branch ruleset for the default branch"
Ensure-Ruleset "protect-main" "branch" @("~DEFAULT_BRANCH") `
    @(@{ type = "deletion" }, @{ type = "non_fast_forward" }) $bypass

Write-Host "2/3 tag ruleset for v*"
Ensure-Ruleset "protect-tags" "tag" @("refs/tags/v*") `
    @(@{ type = "deletion" }, @{ type = "non_fast_forward" }) $bypass

Write-Host "3/3 GITHUB_TOKEN inside Actions -> read-only"
$r = Invoke-Api "PUT" "$api/actions/permissions/workflow" @{
    default_workflow_permissions     = "read"
    can_approve_pull_request_reviews = $false
}
if ($r -ne $null) { Write-Host "workflow token permissions: read-only" }

Write-Host ""
Write-Host "current rulesets:"
(Invoke-Api "GET" "$api/rulesets") | ForEach-Object { "  - $($_.name) [$($_.target)/$($_.enforcement)]" }
Write-Host "collaborators are not touched by this script; check them in the repo settings UI."
