$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent $PSScriptRoot
$taskErrors=$null
$taskAst=[Management.Automation.Language.Parser]::ParseFile((Join-Path $taskRoot 'scripts/Run-UsbGip.ps1'),[ref]$null,[ref]$taskErrors)
if($taskErrors){throw 'Launcher parse failed'}
foreach($taskName in @('Save-TaskState','Read-TaskRadioState')){
    $taskFunction=$taskAst.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $taskName},$true)
    if(-not $taskFunction){throw "Missing function $taskName"}
    Invoke-Expression $taskFunction.Extent.Text
}
function Check($Condition,[string]$Message){if(-not $Condition){throw $Message}}
$taskDirectory=Join-Path $taskRoot ('work/snapshot-test-'+[guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($taskDirectory)
$taskStatePath=Join-Path $taskDirectory 'state.json'
$taskState=[ordered]@{Running=$true;Version=1;Server=$null;Port=0}
$taskDevices=@(@{Server=@{Pid=42};Port=1})
$taskLock=$null
try{
    Save-TaskState
    Check ((Read-TaskRadioState $taskStatePath).Version -eq 1) 'Initial snapshot missing'
    $taskLock=[IO.FileStream]::new($taskStatePath,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::None)
    $taskResult=Read-TaskRadioState $taskStatePath
    Check ($null -eq $taskResult) 'Locked status should be skipped after bounded retry'
    $taskState.Version=2
    Save-TaskState
    $taskLock.Dispose();$taskLock=$null
    Check ((Read-TaskRadioState $taskStatePath).Version -eq 1) 'Failed replacement damaged the complete previous snapshot'
    Save-TaskState
    Check ((Read-TaskRadioState $taskStatePath).Version -eq 2) 'Snapshot did not recover after lock release'
    $taskLock=[IO.FileStream]::new($taskStatePath,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    $taskState.Version=3
    Save-TaskState
    Check ((Read-TaskRadioState $taskStatePath).Version -eq 3) 'Share-delete reader blocked atomic replacement'
    $taskLock.Dispose();$taskLock=$null
    Check ($null -eq (Read-TaskRadioState (Join-Path $taskDirectory 'missing.json'))) 'Missing status should not stop the bridge'
    Write-Output 'PASS snapshot locking, atomic replacement, retry and recovery'
}finally{
    if($taskLock){$taskLock.Dispose()}
    foreach($taskFile in @($taskStatePath,($taskStatePath+'.tmp'))){if([IO.File]::Exists($taskFile)){[IO.File]::Delete($taskFile)}}
    [IO.Directory]::Delete($taskDirectory)
}
