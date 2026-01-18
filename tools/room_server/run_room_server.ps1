param(
    [int]$Port = 8787,
    [Alias('Host')]
    [string]$BindHost = "0.0.0.0",
    [int]$DefaultTtl = 600,
    [int]$MaxTtl = 86400,
    [string]$ApiKey = ""
)

$serverJs = Join-Path $PSScriptRoot "server.js"

$argsList = @(
    $serverJs,
    "--host", $BindHost,
    "--port", "$Port",
    "--default-ttl", "$DefaultTtl",
    "--max-ttl", "$MaxTtl"
)

if ($ApiKey -and $ApiKey.Trim().Length -gt 0) {
    $argsList += @("--api-key", $ApiKey)
}

Write-Host "Starting room server on http://$BindHost`:$Port" -ForegroundColor Cyan
Write-Host "Connection logging: enabled" -ForegroundColor Cyan
Write-Host "Press Ctrl+C to stop." -ForegroundColor Cyan

$env:SNO_LOG_CONNECTIONS = "1"

node @argsList
