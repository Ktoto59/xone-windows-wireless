param([string]$Label = 'reference')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if ($Label -notmatch '^[a-zA-Z0-9-]{1,40}$') { throw 'Use a short alphanumeric label.' }
$tool = Join-Path $root 'outputs/usb-reference-dump.exe'
if (-not (Test-Path -LiteralPath $tool)) { throw 'Build scripts/Build-UsbReferenceDump.cmd first.' }
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$prefix = Join-Path $root "work/stock-usb-$Label-$stamp"
$keys = @(
    'DEVPKEY_Device_Service', 'DEVPKEY_Device_Parent',
    'DEVPKEY_Device_HardwareIds', 'DEVPKEY_Device_CompatibleIds',
    'DEVPKEY_Device_DriverInfPath', 'DEVPKEY_Device_DriverVersion',
    'DEVPKEY_Device_DriverProvider', 'DEVPKEY_Device_UpperFilters',
    'DEVPKEY_Device_LowerFilters', 'DEVPKEY_Device_ProblemCode'
)
$devices = @(Get-PnpDevice -PresentOnly | Where-Object {
    $_.InstanceId -match '^USB\\VID_045E|XBOX|0B32' -or
    $_.FriendlyName -match 'Xbox' -or $_.Class -eq 'AudioEndpoint'
} | ForEach-Object {
    $properties = @{}
    Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName $keys -ErrorAction SilentlyContinue |
        Where-Object Type -ne Empty | ForEach-Object { $properties[$_.KeyName] = $_.Data }
    [pscustomobject]@{
        Status = $_.Status; Class = $_.Class; Name = $_.FriendlyName
        InstanceId = $_.InstanceId; Properties = $properties
    }
})
$devices | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath "$prefix-pnp.json" -Encoding utf8
# Only the helper's three documented hub query IOCTLs; no device writes.
$dumpProcess = Start-Process -FilePath $tool -WindowStyle Hidden -Wait -PassThru -RedirectStandardOutput "$prefix-descriptors.jsonl" -RedirectStandardError "$prefix-descriptors.log"
$descriptorExit = $dumpProcess.ExitCode
$result = [pscustomobject]@{
    CapturedAt = (Get-Date).ToString('o'); Label = $Label
    Pnp = "$prefix-pnp.json"; Descriptors = "$prefix-descriptors.jsonl"
    QueryLog = "$prefix-descriptors.log"; DescriptorExitCode = $descriptorExit
    DriverBindingChanged = $false; FirmwareUpdated = $false
}
$result | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $root 'work/stock-usb-reference-result.json') -Encoding utf8
$result | ConvertTo-Json
if ($descriptorExit -ne 0) { throw "Some USB queries failed; inspect $prefix-descriptors.log" }
