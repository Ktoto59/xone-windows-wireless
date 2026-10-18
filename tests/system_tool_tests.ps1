# SPDX-License-Identifier: GPL-2.0-or-later
$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent $PSScriptRoot
$fixture=Join-Path $taskRoot ('work/system-tool-test-'+[guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($fixture)
$probe=Join-Path $fixture 'probe.ps1'
$configuration=Join-Path $taskRoot 'scripts/Device-Config.ps1'
[IO.File]::WriteAllText($probe,@'
param([string]$Configuration)
$ErrorActionPreference='Stop'
. $Configuration
$env:PATH=''
$tool=Get-XoneSystemTool 'pnputil.exe'
& $tool /? | Out-Null
if($LASTEXITCODE -ne 0){throw 'Native PnPUtil help failed with empty PATH'}
if(-not [IO.File]::Exists((Get-XoneSystemTool 'expand.exe'))){throw 'Native expand is missing'}
$text=& (Get-XoneSystemTool 'WindowsPowerShell/v1.0/powershell.exe') -NoProfile -NonInteractive -Command '[Environment]::Is64BitProcess'
if($LASTEXITCODE -ne 0 -or $text -ne 'True'){throw 'Resolved PowerShell is not native x64'}
Write-Output ('PASS native system tools with empty PATH from '+([IntPtr]::Size*8)+'-bit PowerShell')
'@)
try{
    foreach($directory in @('System32','SysWOW64')){
        $shell=Join-Path $env:windir ($directory+'/WindowsPowerShell/v1.0/powershell.exe')
        & $shell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $probe -Configuration $configuration
        if($LASTEXITCODE -ne 0){throw ('System tool test failed from '+$directory)}
    }
    # Check every installed maintenance script for bare executable invocations.
    # Cmdlets are resolved normally; external tools must have an explicit path.
    foreach($name in @('Setup-Action','Setup-Helpers','Device-Config','Switch-WinUSB','Import-Firmware','Set-AutomaticBridge','Run-MaintainedBridge','Mode-Helpers','Set-XoneMode')){
        $tokens=$null;$errors=$null
        $ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $taskRoot ('scripts/'+$name+'.ps1')),[ref]$tokens,[ref]$errors)
        if($errors.Count){throw ('Maintenance syntax error: '+$name)}
        $bare=$ast.FindAll({param($node) $node -is [Management.Automation.Language.CommandAst] -and $node.GetCommandName() -match '^[a-z0-9_-]+\.(exe|cmd|bat)$'},$true)
        if($bare.Count){throw ('Bare system executable in installed maintenance script: '+$name)}
    }
    Write-Output 'PASS installed maintenance scripts have no bare external commands'
}finally{
    $resolved=[IO.Path]::GetFullPath($fixture)
    $boundary=[IO.Path]::GetFullPath((Join-Path $taskRoot 'work'))+[IO.Path]::DirectorySeparatorChar
    if(-not $resolved.StartsWith($boundary,[StringComparison]::OrdinalIgnoreCase)){throw 'Cleanup outside work refused'}
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
