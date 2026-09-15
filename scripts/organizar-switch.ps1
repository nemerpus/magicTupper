param([string]$Library='Z:\juegos\roms\NINTENDO SWITCH',[string]$ConsoleSwitch='E:\switch',[switch]$Apply)
$ErrorActionPreference='Stop'
$taskRoot=[IO.Path]::GetFullPath((Get-Item -LiteralPath $Library).FullName).TrimEnd('\')
$taskSd=[IO.Path]::GetFullPath((Get-Item -LiteralPath $ConsoleSwitch).FullName).TrimEnd('\')
$taskReport=Join-Path (Split-Path $PSScriptRoot -Parent) ('.tools\organizacion-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $taskReport | Out-Null
function Assert-Child([string]$Root,[string]$Target) {
    $taskResolved=[IO.Path]::GetFullPath($Target)
    if(!$taskResolved.StartsWith($Root+'\',[StringComparison]::OrdinalIgnoreCase)){throw "Ruta fuera del destino: $taskResolved"}
    return $taskResolved
}
$taskPlan=[Collections.Generic.List[object]]::new()
foreach($taskFile in Get-ChildItem -LiteralPath $taskRoot -File){
    if($taskFile.Extension.ToLowerInvariant() -notin @('.nsp','.xci','.nsz','.xcz','.nro')){continue}
    if($taskFile.Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Se ha encontrado un enlace; revisar antes de organizar.'}
    $taskCategory=if($taskFile.Extension -eq '.nro'){'APPS'}elseif($taskFile.Name -match '(?i)\bDLC\b'){'DLC'}elseif($taskFile.Name -match '(?i)\b(update|patch)\b|\bv\d+(?:\.\d+)+|\[0100[0-9a-f]{9}800\]'){'UPDATES'}else{'ROMS'}
    $taskDestination=Assert-Child $taskRoot (Join-Path (Join-Path $taskRoot $taskCategory) $taskFile.Name)
    $taskPlan.Add([pscustomobject]@{Action='Move';Source=$taskFile.FullName;Destination=$taskDestination;Length=$taskFile.Length})
}
$taskProject=Join-Path $taskRoot 'magicTupper'
if(Test-Path -LiteralPath $taskProject){
    $taskDestination=Assert-Child $taskRoot (Join-Path $taskRoot ('.archivo-magicTupper-'+(Get-Date -Format 'yyyyMMdd-HHmmss')))
    $taskPlan.Add([pscustomobject]@{Action='ArchiveDirectory';Source=(Assert-Child $taskRoot $taskProject);Destination=$taskDestination;Length=0})
}
$taskAllowed=@('.nro','.nacp','.png','.jpg','.jpeg','.bmp','.ttf','.otf','.wav','.ogg','.mp3','.glsl','.vert','.frag','.fsh','.vsh','.pgf','.lang','.mo','.license')
function Add-AppFiles([string]$Directory){
    foreach($taskEntry in Get-ChildItem -LiteralPath $Directory){
        if($taskEntry.Name.StartsWith('.') -or ($taskEntry.Attributes -band [IO.FileAttributes]::ReparsePoint)){continue}
        if($taskEntry.PSIsContainer){if($taskEntry.Name -notin @('downloads','source','tests','cache','saves','logs')){Add-AppFiles $taskEntry.FullName};continue}
        if($taskEntry.Extension.ToLowerInvariant() -notin $taskAllowed){continue}
        $taskRelative=$taskEntry.FullName.Substring($taskSd.Length+1)
        $taskDestination=Assert-Child $taskRoot (Join-Path (Join-Path $taskRoot 'APPS') $taskRelative)
        $taskPlan.Add([pscustomobject]@{Action='Copy';Source=$taskEntry.FullName;Destination=$taskDestination;Length=$taskEntry.Length})
    }
}
Add-AppFiles $taskSd
$taskPlan | Export-Csv -LiteralPath (Join-Path $taskReport 'plan.csv') -NoTypeInformation -Encoding UTF8
$taskPlan | Group-Object Action | Select-Object Name,Count | Format-Table -AutoSize
Write-Output "Manifiesto: $taskReport"
if(!$Apply){Write-Output 'Solo inventario. Usa -Apply para ejecutar este procedimiento.';return}
foreach($taskFolder in @('ROMS','DLC','UPDATES','APPS')){New-Item -ItemType Directory -Path (Assert-Child $taskRoot (Join-Path $taskRoot $taskFolder)) -Force | Out-Null}
foreach($taskOperation in $taskPlan){
    $taskDestination=Assert-Child $taskRoot $taskOperation.Destination
    if(Test-Path -LiteralPath $taskDestination){throw "Destino existente; se conserva: $taskDestination"}
    if($taskOperation.Action -eq 'Copy'){
        $null=Assert-Child $taskSd $taskOperation.Source
        New-Item -ItemType Directory -Path (Split-Path $taskDestination -Parent) -Force | Out-Null
        Copy-Item -LiteralPath $taskOperation.Source -Destination $taskDestination
        if((Get-FileHash -LiteralPath $taskOperation.Source).Hash -ne (Get-FileHash -LiteralPath $taskDestination).Hash){throw "La copia no coincide: $taskDestination"}
    }else{
        $null=Assert-Child $taskRoot $taskOperation.Source
        if($taskOperation.Action -eq 'Move' -and (Get-Item -LiteralPath $taskOperation.Source).Length -ne $taskOperation.Length){throw 'El archivo ha cambiado desde el inventario.'}
        Move-Item -LiteralPath $taskOperation.Source -Destination $taskDestination
        if($taskOperation.Action -eq 'Move' -and (Get-Item -LiteralPath $taskDestination).Length -ne $taskOperation.Length){throw 'Longitud inesperada después del movimiento.'}
    }
    $taskOperation | Export-Csv -LiteralPath (Join-Path $taskReport 'completed.csv') -NoTypeInformation -Encoding UTF8 -Append
}
Write-Output 'Organización terminada; movimientos registrados y copias verificadas con SHA-256.'
