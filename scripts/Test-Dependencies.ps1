$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath (Split-Path $PSScriptRoot -Parent)
$raw = & dotnet list pc-client/MagicTupper-PC-Client.csproj package --vulnerable --include-transitive --format json
if ($LASTEXITCODE -ne 0) { throw 'NuGet vulnerability query failed.' }
$report = ($raw -join "`n") | ConvertFrom-Json
if ($report.version -ne 1 -or @($report.projects).Count -ne 1 -or !($report.sources -contains 'https://api.nuget.org/v3/index.json')) { throw 'Unexpected NuGet advisory report.' }
if ($report.PSObject.Properties['problems'] -and @($report.problems).Count -gt 0) { throw 'Incomplete NuGet advisory report.' }
foreach ($project in $report.projects) {
    foreach ($framework in $project.frameworks) {
        foreach ($group in @('topLevelPackages', 'transitivePackages')) {
            $property = $framework.PSObject.Properties[$group]
            if (!$property) { continue }
            foreach ($package in $property.Value) {
                $vulnerabilities = $package.PSObject.Properties['vulnerabilities']
                if ($vulnerabilities -and @($vulnerabilities.Value).Count -gt 0) { throw "Vulnerable dependency: $($package.id)" }
            }
        }
    }
}
Write-Output 'PASS: NuGet advisory query, direct and transitive dependencies.'
