param([string]$NodePath = '')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path $PSScriptRoot -Parent
Set-Location -LiteralPath $root
$version = ([xml](Get-Content -LiteralPath 'pc-client/MagicTupper-PC-Client.csproj' -Raw)).Project.PropertyGroup.Version
if ($version -notmatch '^\d+\.\d+\.\d+-rc\.\d+$') { throw 'This pipeline only publishes candidates. Complete RELEASE.md before a stable release.' }
if (!$NodePath) { $NodePath = Join-Path $root '.tools/node-v24.20.0-win-x64/node.exe' }
if (!(Test-Path -LiteralPath $NodePath)) { throw 'Pass -NodePath with a Node.js 24 executable.' }
function Invoke-Checked([string]$Tool, [string[]]$Arguments) {
    & $Tool @Arguments | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "$Tool failed with exit code $LASTEXITCODE" }
}
$qa = Join-Path $root ".tools/release-$version"
$package = Join-Path $root "dist/MagicTupper-$version"
if (Test-Path -LiteralPath $package) { throw 'Package already exists. Use a new candidate version; do not overwrite published artifacts.' }
New-Item -ItemType Directory -Path $qa -Force | Out-Null
Start-Transcript -Path (Join-Path $qa 'validation.log') -Force | Out-Null
try {
    Invoke-Checked dotnet @('restore', 'tests/pc-client/MagicTupper.Tests.csproj', '--locked-mode')
    Invoke-Checked powershell @('-NoProfile', '-File', 'scripts/Test-Dependencies.ps1')
    Invoke-Checked dotnet @('build', 'tests/pc-client/MagicTupper.Tests.csproj', '-c', 'Release', '--no-restore', "-p:OutputPath=$qa/bin/")
    Invoke-Checked dotnet @((Join-Path $qa 'bin/MagicTupper.Tests.dll'), (Join-Path $qa 'qa'))
    Invoke-Checked $NodePath @('server/test-all.js')
    $linuxRoot = (& wsl -d Ubuntu -u root --exec wslpath -a $root).Trim()
    if ($LASTEXITCODE -ne 0 -or !$linuxRoot.StartsWith('/mnt/')) { throw 'WSL workspace resolution failed.' }
    Invoke-Checked wsl @('-d', 'Ubuntu', '-u', 'root', '--exec', '/bin/sh', "$linuxRoot/scripts/test-install-wsl.sh", $linuxRoot)
    Invoke-Checked wsl @('-d', 'Ubuntu', '-u', 'root', '--exec', '/bin/sh', "$linuxRoot/scripts/compile-wsl.sh", $linuxRoot)
    Invoke-Checked dotnet @('publish', 'pc-client/MagicTupper-PC-Client.csproj', '-c', 'Release', '-r', 'win-x64', '--self-contained', 'true', '-p:RestoreLockedMode=true', '-p:NuGetLockFilePath=packages.publish.lock.json', '-p:PublishSingleFile=true', '-p:IncludeNativeLibrariesForSelfExtract=true', "-p:OutputPath=$qa/publish/", '-o', "$package/PC")
    New-Item -ItemType Directory -Path "$package/switch" -Force | Out-Null
    Copy-Item -LiteralPath 'dist/switch/magictupper' -Destination "$package/switch" -Recurse
    Copy-Item -LiteralPath 'pc-client/libusb-1.0.dll' -Destination "$package/PC/libusb-1.0.dll" -Force
    foreach ($doc in @('SECURITY.md', 'RELEASE.md', 'AUDITORIA-0.4.0.md', 'NOVEDADES-0.4.0-rc.2.md')) { Copy-Item -LiteralPath $doc -Destination $package }
    $manifest = @(Get-ChildItem -LiteralPath $package -Recurse -File | Sort-Object FullName | ForEach-Object {
        [ordered]@{ path = $_.FullName.Substring($package.Length + 1).Replace('\','/'); bytes = $_.Length; sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }
    })
    $manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath "$package/checksums.json" -Encoding UTF8
    [ordered]@{ version=$version; channel='candidate'; automatedChecks='passed'; hardwareValidated=$false; timestampUtc=[DateTime]::UtcNow.ToString('o'); node=(& $NodePath --version); dotnet=(& dotnet --version) } |
        ConvertTo-Json | Set-Content -LiteralPath "$package/verification.json" -Encoding UTF8
    Compress-Archive -LiteralPath $package -DestinationPath "$package.zip"
    Get-FileHash -LiteralPath "$package.zip" -Algorithm SHA256
} finally { Stop-Transcript | Out-Null }
