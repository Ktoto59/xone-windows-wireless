# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param([switch]$Stop)
$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'Setup-Helpers.ps1')
if($Stop){
    [void][IO.Directory]::CreateDirectory((Join-Path $taskRoot 'work'))
    $taskMemoryHost=Get-XoneMemoryHost $taskRoot
    if($taskMemoryHost){& $taskMemoryHost --stop | Out-Null}else{
        foreach($taskMarker in @('maintenance.stop','usb-gip.stop')){[IO.File]::WriteAllText((Join-Path $taskRoot ('work/'+$taskMarker)),'stop')}
    }
    $taskState=Read-XoneJson (Join-Path $taskRoot 'work/maintenance-status.json')
    $taskDeadline=[datetime]::UtcNow.AddSeconds(35)
    while($taskState -and (Test-XoneProcess $taskState.Process) -and [datetime]::UtcNow -lt $taskDeadline){Start-Sleep -Milliseconds 100}
    if($taskState -and (Test-XoneProcess $taskState.Process)){throw 'The owned C++ host has not stopped; recovery files were preserved.'}
    Stop-XoneBridge $taskRoot
    return
}
$taskHost=Join-Path $taskRoot 'outputs/xone-background.exe'
if(-not [IO.File]::Exists($taskHost)){throw 'The C++ runtime is missing. Reinstall the bridge.'}
Start-Process -FilePath $taskHost -WorkingDirectory $taskRoot -WindowStyle Hidden | Out-Null
