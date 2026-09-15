$ErrorActionPreference = 'Stop'
$taskResultPath = Join-Path (Split-Path $PSScriptRoot -Parent) '.tools\wsl-recovery-result.txt'
try {
    $taskService = Get-CimInstance Win32_Service -Filter "Name='WslService'"
    if ($taskService.State -eq 'Stop Pending' -and $taskService.ProcessId -gt 0) {
        $taskProcess = Get-Process -Id $taskService.ProcessId -ErrorAction Stop
        if ($taskProcess.ProcessName -ne 'wslservice') { throw 'El proceso no corresponde a WslService.' }
        Stop-Process -Id $taskProcess.Id -Force -ErrorAction Stop
        Start-Sleep -Seconds 2
    }
    Start-Service WslService -ErrorAction Stop
    'OK: WslService recuperado.' | Set-Content -LiteralPath $taskResultPath -Encoding UTF8
} catch {
    ('ERROR: ' + $_.Exception.Message) | Set-Content -LiteralPath $taskResultPath -Encoding UTF8
    exit 1
}
