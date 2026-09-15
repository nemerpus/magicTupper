$ErrorActionPreference = 'Stop'
$taskResultPath = Join-Path (Split-Path $PSScriptRoot -Parent) '.tools\wsl-restart-result.txt'
try {
    Restart-Service -Name WslService -Force -ErrorAction Stop
    'OK: WslService reiniciado.' | Set-Content -LiteralPath $taskResultPath -Encoding UTF8
} catch {
    ('ERROR: ' + $_.Exception.Message) | Set-Content -LiteralPath $taskResultPath -Encoding UTF8
    exit 1
}
