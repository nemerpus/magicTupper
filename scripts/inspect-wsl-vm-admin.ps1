$taskResultPath = Join-Path (Split-Path $PSScriptRoot -Parent) '.tools\wsl-vm-list.json'
& "$env:SystemRoot\System32\hcsdiag.exe" list -raw | Set-Content -LiteralPath $taskResultPath -Encoding UTF8
