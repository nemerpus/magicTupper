$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$portable = Get-ChildItem (Join-Path $projectRoot '.tools') -Filter node.exe -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
if ($portable) { $script:Node = $portable.FullName } else { $script:Node = (Get-Command node -ErrorAction Stop).Source }
$version = & $script:Node --version
if ([int]($version.TrimStart('v').Split('.')[0]) -lt 22) { throw 'Instala Node.js 24 LTS desde https://nodejs.org y vuelve a ejecutar este archivo.' }
