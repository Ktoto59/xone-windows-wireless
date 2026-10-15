# SPDX-License-Identifier: GPL-2.0-or-later
$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent $PSScriptRoot
$taskTokens=$null;$taskErrors=$null
$taskAst=[Management.Automation.Language.Parser]::ParseFile((Join-Path $taskRoot 'scripts/Package-Release.ps1'),[ref]$taskTokens,[ref]$taskErrors)
if($taskErrors.Count){throw 'Packager syntax failed'}
$taskFunction=$taskAst.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Assert-PublicReleaseBinary'},$true)
if(-not $taskFunction){throw 'Missing release binary guard'}
Invoke-Expression $taskFunction.Extent.Text
$taskDirectory=Join-Path $taskRoot ('work/binary-privacy-test-'+[guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($taskDirectory)
$taskFixture=Join-Path $taskDirectory 'fixture.exe'
try{
    $taskPrivatePath='C:'+([char]92)+'Users'+([char]92)+'ExampleUser'+([char]92)+'source.cpp'
    foreach($taskEncoding in @([Text.Encoding]::UTF8,[Text.Encoding]::Unicode)){
        foreach($taskOffset in @(0,1)){
            $taskBytes=$taskEncoding.GetBytes($taskPrivatePath)
            if($taskOffset){$taskBytes=[byte[]]@(0)+$taskBytes}
            [IO.File]::WriteAllBytes($taskFixture,$taskBytes)
            $taskRejected=$false;try{Assert-PublicReleaseBinary $taskFixture}catch{$taskRejected=$true}
            if(-not $taskRejected){throw 'Personal build path accepted'}
        }
    }
    $taskSid='S-1-5-21-'+('1234-'*3)+'5678'
    [IO.File]::WriteAllBytes($taskFixture,[Text.Encoding]::UTF8.GetBytes($taskSid))
    $taskRejected=$false;try{Assert-PublicReleaseBinary $taskFixture}catch{$taskRejected=$true}
    if(-not $taskRejected){throw 'User SID accepted'}
    [IO.File]::WriteAllBytes($taskFixture,[Text.Encoding]::UTF8.GetBytes('045E:02FE 40E8C37A-1A80-41D6-A4BB-0578AD2B7A18'))
    Assert-PublicReleaseBinary $taskFixture
    Write-Output 'PASS release binary privacy: ASCII/UTF-16 paths, odd alignment, SID rejection and public constants'
}finally{
    [IO.File]::Delete($taskFixture)
    [IO.Directory]::Delete($taskDirectory)
}
