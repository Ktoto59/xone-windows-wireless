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
foreach($taskFile in $taskManifest.Files){
    if((Get-FileHash -LiteralPath (Join-Path $taskInstall $taskFile.Path) -Algorithm SHA256).Hash.ToLowerInvariant() -ne $taskFile.SHA256){throw ('Installed payload differs: '+$taskFile.Path)}
}
if(Test-Path -LiteralPath (Join-Path $taskInstall 'work')){throw 'Smoke test executed runtime/driver hooks'}
$taskUninstaller=Join-Path $taskInstall 'unins000.exe'
$taskProcess=Start-Process -FilePath $taskUninstaller -ArgumentList '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART' -WindowStyle Hidden -PassThru -Wait
if($taskProcess.ExitCode -ne 0){throw 'Smoke uninstallation failed'}
$taskDeadline=[datetime]::UtcNow.AddSeconds(10)
while((Test-Path -LiteralPath (Join-Path $taskInstall 'outputs/xone-background.exe')) -and [datetime]::UtcNow -lt $taskDeadline){Start-Sleep -Milliseconds 100}
if((Test-Path $taskKey) -or (Test-Path -LiteralPath (Join-Path $taskInstall 'outputs/xone-background.exe'))){throw 'Smoke registration or payload survived removal'}
$taskResult=@{Passed=$true;HardwareHooksExecuted=$false;Version=$taskManifest.Version;PayloadHashesChecked=$taskManifest.Files.Count;InstallPath=$taskInstall;CheckedUtc=[datetime]::UtcNow.ToString('o')}
$taskResult | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $taskOutput 'result.json')
Write-Output 'PASS payload hashes/privacy, Windows Installed apps registration and removal; hardware hooks intentionally not executed'
