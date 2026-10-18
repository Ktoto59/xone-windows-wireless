# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param([Parameter(Mandatory=$true)][ValidateSet('Start','Stop')][string]$Mode)
$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent $PSScriptRoot
$taskLock=$null
$taskFailure=$null
try{
    . (Join-Path $PSScriptRoot 'Setup-Helpers.ps1')
    . (Join-Path $PSScriptRoot 'Mode-Helpers.ps1')
    [void][IO.Directory]::CreateDirectory((Join-Path $taskRoot 'work'))
    try{$taskLock=[IO.FileStream]::new((Join-Path $taskRoot 'work/mode-switch.lock'),[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)}
    catch [IO.IOException]{throw 'Another Start/Stop operation is running. Wait for it to finish and retry.'}
    $taskResult=Invoke-XoneModeSwitch $taskRoot $Mode
    if($taskResult.Changed){
        Add-Type -AssemblyName System.Windows.Forms
        $taskMessage=if($Mode -eq 'Stop'){'The bridge is stopped and the Microsoft driver is selected. Automatic bridge startup is paused until you use Start xone Wireless.'}else{'WinUSB is selected and the bridge start was requested. If the headset does not reconnect, switch it off, unplug the adapter for 10 seconds, reconnect it and turn the headset on.'}
        [void][Windows.Forms.MessageBox]::Show($taskMessage,'xone Wireless',[Windows.Forms.MessageBoxButtons]::OK,[Windows.Forms.MessageBoxIcon]::Information)
    }
}catch{
    $taskFailure=$_.Exception.Message
    try{
        Add-Type -AssemblyName System.Windows.Forms
        [void][Windows.Forms.MessageBox]::Show($taskFailure,'xone Wireless',[Windows.Forms.MessageBoxButtons]::OK,[Windows.Forms.MessageBoxIcon]::Error)
    }catch{Write-Error $taskFailure -ErrorAction Continue}
}finally{if($taskLock){$taskLock.Dispose()}}
if($taskFailure){exit 1}
