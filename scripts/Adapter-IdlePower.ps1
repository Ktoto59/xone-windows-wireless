# SPDX-License-Identifier: GPL-2.0-or-later
# Device Parameters (the device hardware key opened by SetupDiOpenDevRegKey).
# Never change hub/global power settings.
function Get-XoneIdlePowerNames { @('DeviceIdleEnabled','DeviceIdleIgnoreWakeEnable','DefaultIdleState') }
function Save-XoneIdlePower([string]$RegistryPath) {
    $taskSaved=[ordered]@{}
    $taskKey=Get-Item -LiteralPath $RegistryPath -ErrorAction Stop
    foreach($taskName in (Get-XoneIdlePowerNames)) {
        $taskPresent=$taskKey.GetValueNames() -contains $taskName
        if($taskPresent -and $taskKey.GetValueKind($taskName) -ne [Microsoft.Win32.RegistryValueKind]::DWord){throw "Unexpected idle policy type: $taskName"}
        $taskSaved[$taskName]=@{Present=$taskPresent;Value=if($taskPresent){$taskKey.GetValue($taskName)}else{0}}
    }
    return [pscustomobject]$taskSaved
}
function Enable-XoneIdlePower([string]$RegistryPath) {
    New-ItemProperty -LiteralPath $RegistryPath -Name DeviceIdleEnabled -Value 1 -PropertyType DWord -Force | Out-Null
    New-ItemProperty -LiteralPath $RegistryPath -Name DeviceIdleIgnoreWakeEnable -Value 0 -PropertyType DWord -Force | Out-Null
    # A closed/crashed bridge must not leave an unprepared radio in USB sleep.
    New-ItemProperty -LiteralPath $RegistryPath -Name DefaultIdleState -Value 0 -PropertyType DWord -Force | Out-Null
}
function Restore-XoneIdlePower([string]$RegistryPath,$Saved) {
    foreach($taskName in (Get-XoneIdlePowerNames)) {
        $taskProperty=$Saved.PSObject.Properties[$taskName]
        if(-not $taskProperty){throw "Missing saved idle policy: $taskName"}
        if($taskProperty.Value.Present){
            New-ItemProperty -LiteralPath $RegistryPath -Name $taskName -Value $taskProperty.Value.Value -PropertyType DWord -Force | Out-Null
        }elseif((Get-ItemProperty -LiteralPath $RegistryPath).PSObject.Properties[$taskName]){
            Remove-ItemProperty -LiteralPath $RegistryPath -Name $taskName
        }
    }
}
