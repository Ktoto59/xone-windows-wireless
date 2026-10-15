# SPDX-License-Identifier: GPL-2.0-or-later
$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent $PSScriptRoot
function Check($Condition,[string]$Message){if(-not $Condition){throw $Message}}
$fixture=Join-Path $taskRoot ('work/action-test-'+[guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory((Join-Path $fixture 'scripts'))
$helper=Join-Path $fixture 'scripts/Setup-Helpers.ps1'
$action=Join-Path $fixture 'scripts/Setup-Action.ps1'
Copy-Item -LiteralPath (Join-Path $taskRoot 'scripts/Setup-Action.ps1') -Destination $action
$powershell=Join-Path $env:windir 'System32/WindowsPowerShell/v1.0/powershell.exe'
try{
    [IO.File]::WriteAllText($helper,'function Initialize-XonePrerequisites($Root){$script:XoneSetupStage="FirmwareDownload";throw "Simulated download failure"}')
    & $powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $action -Action Prepare | Out-Null
    Check ($LASTEXITCODE -eq 1) 'Failed action returned success'
    $result=Join-Path $fixture 'work/setup-Prepare.json'
    $state=Get-Content -LiteralPath $result -Raw | ConvertFrom-Json
    Check (-not $state.Success -and $state.Stage -eq 'FirmwareDownload' -and $state.Error -eq 'Simulated download failure') 'Failure JSON lost its reason/stage'
    Check ([IO.File]::ReadAllText($result+'.error.txt').Contains('Simulated download failure')) 'Installer error text is missing'
    Check ([IO.File]::Exists($result+'.log')) 'Local transcript is missing'
    [IO.File]::WriteAllText($helper,'function Initialize-XonePrerequisites($Root){}')
    & $powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $action -Action Prepare | Out-Null
    Check ($LASTEXITCODE -eq 0) 'Successful action failed'
    Check ((Get-Content -LiteralPath $result -Raw | ConvertFrom-Json).Success) 'Success JSON not written'
    Check (-not [IO.File]::Exists($result+'.error.txt')) 'Stale error survived successful retry'
    [IO.File]::WriteAllText($helper,'throw "Simulated helper load failure"')
    $custom=Join-Path $fixture 'custom/result.json'
    & $powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $action -Action Prepare -ResultPath $custom | Out-Null
    Check ($LASTEXITCODE -eq 1) 'Helper load failure returned success'
    Check ((Get-Content -LiteralPath $custom -Raw | ConvertFrom-Json).Error -eq 'Simulated helper load failure') 'Helper load failure was not preserved'
    [IO.File]::WriteAllText($helper,'function Initialize-XonePrerequisites($Root){if(-not [Environment]::Is64BitProcess){throw "Maintenance stayed in WOW64"}}')
    $wow=Join-Path $env:windir 'SysWOW64/WindowsPowerShell/v1.0/powershell.exe'
    $custom=Join-Path $fixture 'a folder with spaces/native result.json'
    & $wow -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $action -Action Prepare -ResultPath $custom
    Check ($LASTEXITCODE -eq 0) 'WOW64 maintenance bootstrap failed'
    $state=Get-Content -LiteralPath $custom -Raw | ConvertFrom-Json
    Check ($state.Success -and $state.ProcessBitness -eq 64) 'Maintenance did not switch to x64 before loading helpers'
    [IO.File]::WriteAllText($helper,'throw "Native bootstrap preserves failure"')
    & $wow -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $action -Action Prepare -ResultPath $custom | Out-Null
    Check ($LASTEXITCODE -eq 1) 'WOW64 bootstrap lost the native child failure exit code'
    Check ((Get-Content -LiteralPath $custom -Raw | ConvertFrom-Json).Error -eq 'Native bootstrap preserves failure') 'WOW64 bootstrap lost diagnostics'
    Write-Output 'PASS actual WOW64-to-x64 maintenance bootstrap, spaced paths and failure propagation'
    Write-Output 'PASS maintenance diagnostics, helper-load failure, custom path and successful retry'
}finally{
    $resolved=[IO.Path]::GetFullPath($fixture)
    $boundary=[IO.Path]::GetFullPath((Join-Path $taskRoot 'work'))+[IO.Path]::DirectorySeparatorChar
    if(-not $resolved.StartsWith($boundary,[StringComparison]::OrdinalIgnoreCase)){throw 'Cleanup outside work refused'}
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
