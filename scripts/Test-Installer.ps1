# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$IsccPath,
      [Parameter(Mandatory=$true)][string]$StageDir)
$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent $PSScriptRoot
$taskStage=[IO.Path]::GetFullPath($StageDir)
$taskOutput=Join-Path $taskRoot 'work/installer-smoke'
$taskInstall=Join-Path $taskOutput ('app-'+[guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($taskOutput)
$taskManifest=Get-Content -LiteralPath (Join-Path $taskStage 'release-manifest.json') -Raw | ConvertFrom-Json
foreach($taskFile in $taskManifest.Files){
    $taskPath=Join-Path $taskStage $taskFile.Path
    if((Get-FileHash -LiteralPath $taskPath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $taskFile.SHA256){throw ('Payload hash mismatch: '+$taskFile.Path)}
}
$taskForbidden=@(Get-ChildItem -LiteralPath $taskStage -Recurse -File | Where-Object {$_.Extension -in @('.py','.pyc','.bin','.log') -or $_.Name -match '\.local\.'})
if($taskForbidden.Count){throw 'Private/research payload was included'}
if(@(Get-ChildItem -LiteralPath (Join-Path $taskStage 'outputs') -File -Filter '*.exe').Count -ne 3){throw 'Unexpected runtime executable count'}
Get-ChildItem -LiteralPath (Join-Path $taskStage 'scripts') -Filter '*.ps1' | ForEach-Object {
    $taskTokens=$null;$taskErrors=$null
    [void][Management.Automation.Language.Parser]::ParseFile($_.FullName,[ref]$taskTokens,[ref]$taskErrors)
    if($taskErrors.Count){throw ('PowerShell syntax error: '+$_.Name)}
}
& $IsccPath '/DInstallerSmokeTest=1' ('/DAppVersion='+$taskManifest.Version) ('/DStageDir='+$taskStage) ('/O'+$taskOutput) (Join-Path $taskRoot 'installer/xone-windows-wireless.iss')
if($LASTEXITCODE -ne 0){throw 'Smoke installer compilation failed'}
$taskKey='HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{40E8C37A-1A80-41D6-A4BB-0578AD2B7A19}_is1'
if(Test-Path $taskKey){throw 'A previous smoke installation exists; inspect it before rerunning'}
$taskInstaller=Join-Path $taskOutput 'xone-installer-smoke.exe'
$taskProcess=Start-Process -FilePath $taskInstaller -ArgumentList ('/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP- /DIR="'+$taskInstall+'"') -WindowStyle Hidden -PassThru -Wait
if($taskProcess.ExitCode -ne 0){throw 'Smoke installation failed'}
$taskRegistered=Get-ItemProperty -LiteralPath $taskKey
if([IO.Path]::GetFullPath($taskRegistered.InstallLocation).TrimEnd('\') -ne [IO.Path]::GetFullPath($taskInstall).TrimEnd('\')){throw 'Unexpected smoke uninstall registration'}
if(-not $taskRegistered.DisplayIcon -or [IO.Path]::GetFullPath($taskRegistered.DisplayIcon.Trim('"')) -ne [IO.Path]::GetFullPath((Join-Path $taskInstall 'outputs/xone-background.exe'))){throw 'Installed apps icon does not reference the branded host'}
$taskStartMenu=Join-Path ([Environment]::GetFolderPath('Programs')) 'xone-windows-wireless-smoke'
$taskStartShortcut=Join-Path $taskStartMenu 'Start xone Wireless.lnk'
$taskStopShortcut=Join-Path $taskStartMenu 'Stop xone Wireless.lnk'
if(-not (Test-Path -LiteralPath $taskStartShortcut) -or -not (Test-Path -LiteralPath $taskStopShortcut)){throw 'Start/stop shortcuts are missing'}
$taskShell=New-Object -ComObject WScript.Shell
try{
    $taskLink=$taskShell.CreateShortcut($taskStartShortcut)
    if([IO.Path]::GetFullPath($taskLink.TargetPath) -ne [IO.Path]::GetFullPath((Join-Path $taskInstall 'outputs/xone-background.exe'))){throw 'Start shortcut targets another installation'}
    $taskLink=$taskShell.CreateShortcut($taskStopShortcut)
    if(-not $taskLink.Arguments.Contains((Join-Path $taskInstall 'scripts\Setup-Action.ps1')) -or -not $taskLink.Arguments.Contains('-Action Stop') -or -not $taskLink.Arguments.Contains('-WindowStyle Hidden')){throw 'Stop shortcut does not invoke owned hidden maintenance'}
}finally{[void][Runtime.InteropServices.Marshal]::ReleaseComObject($taskLink);[void][Runtime.InteropServices.Marshal]::ReleaseComObject($taskShell)}
foreach($taskFile in $taskManifest.Files){
    if((Get-FileHash -LiteralPath (Join-Path $taskInstall $taskFile.Path) -Algorithm SHA256).Hash.ToLowerInvariant() -ne $taskFile.SHA256){throw ('Installed payload differs: '+$taskFile.Path)}
}
if(Test-Path -LiteralPath (Join-Path $taskInstall 'work')){throw 'Smoke test executed runtime/driver hooks'}
if($taskRegistered.SmokeStartup -ne 'on' -or $taskRegistered.SmokeStartRequested -ne 1){throw 'Default startup/start-now selections were not applied'}
$taskCases=@(@{Options='/TASKS="startnow"';Startup='off';StartNow=1},
    @{Options='/TASKS="autostart"';Startup='on';StartNow=0},
    @{Options='/TASKS=""';Startup='off';StartNow=0},
    @{Options='';Startup='off';StartNow=0})
foreach($taskCase in $taskCases){
    $taskProcess=Start-Process -FilePath $taskInstaller -ArgumentList ('/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP- /DIR="'+$taskInstall+'" '+$taskCase.Options) -WindowStyle Hidden -PassThru -Wait
    if($taskProcess.ExitCode -ne 0){throw 'Smoke upgrade failed'}
    $taskRegistered=Get-ItemProperty -LiteralPath $taskKey
    if($taskRegistered.SmokeStartup -ne $taskCase.Startup -or $taskRegistered.SmokeStartRequested -ne $taskCase.StartNow){throw 'Independent startup options were ignored or lost on upgrade'}
    if(Test-Path -LiteralPath (Join-Path $taskInstall 'work')){throw 'Smoke upgrade executed runtime/driver hooks'}
}
$taskUninstaller=Join-Path $taskInstall 'unins000.exe'
$taskProcess=Start-Process -FilePath $taskUninstaller -ArgumentList '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART' -WindowStyle Hidden -PassThru -Wait
if($taskProcess.ExitCode -ne 0){throw 'Smoke uninstallation failed'}
$taskDeadline=[datetime]::UtcNow.AddSeconds(10)
while((Test-Path -LiteralPath (Join-Path $taskInstall 'outputs/xone-background.exe')) -and [datetime]::UtcNow -lt $taskDeadline){Start-Sleep -Milliseconds 100}
if((Test-Path $taskKey) -or (Test-Path -LiteralPath (Join-Path $taskInstall 'outputs/xone-background.exe'))){throw 'Smoke registration or payload survived removal'}
if((Test-Path -LiteralPath $taskStartShortcut) -or (Test-Path -LiteralPath $taskStopShortcut)){throw 'Smoke start/stop shortcuts survived removal'}
$taskResult=@{Passed=$true;HardwareHooksExecuted=$false;Version=$taskManifest.Version;PayloadHashesChecked=$taskManifest.Files.Count;InstallPath=$taskInstall;CheckedUtc=[datetime]::UtcNow.ToString('o')}
$taskResult | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $taskOutput 'result.json')
Write-Output 'PASS payload hashes/privacy, Installed apps icon, startup/start-now opt-out and remembered upgrade choices, removal; hardware hooks intentionally not executed'
