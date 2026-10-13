# SPDX-License-Identifier: GPL-2.0-or-later
. (Join-Path $PSScriptRoot 'Setup-Helpers.ps1')
function Read-XoneNativeConfig([string]$Path){
    $value=Read-XoneJson $Path
    if(-not $value -or $value.Version -ne 1){throw 'Unsupported or missing native GIP configuration.'}
    $adapter=[string]$value.AdapterInstanceId
    if($adapter -notmatch '^USB\\VID_045E&PID_02FE\\[^\\\r\n"]+$'){throw 'Native GIP requires an external 045E:02FE adapter instance.'}
    $seen=@{};$peers=@()
    foreach($entry in @($value.Peers)){
        $peer=([string]$entry).ToLowerInvariant()
        if($peer -notmatch '^[0-9a-f]{12}$' -or $peer -eq '000000000000' -or $seen.ContainsKey($peer)){throw 'Native peers must be unique nonzero six-byte addresses.'}
        $seen[$peer]=$true;$peers+=,$peer
    }
    if($peers.Count -gt 16){throw 'At most 16 saved native peers are supported.'}
    return [pscustomobject]@{Version=1;AdapterInstanceId=$adapter;Peers=$peers}
}
function Initialize-XoneNativeConfig([string]$Root,[string]$Adapter){
    $path=Join-Path $Root 'config/native.local.json'
    if([IO.File]::Exists($path)){
        $value=Read-XoneNativeConfig $path
        if($value.AdapterInstanceId -ne $Adapter){throw 'Native configuration belongs to a different adapter.'}
        return $value
    }
    $peers=@();$legacy=Join-Path $Root 'config/devices.local.json'
    if([IO.File]::Exists($legacy)){
        $previous=Read-XoneDeviceConfig $legacy -AllowEmpty
        if($previous.AdapterInstanceId -and $previous.AdapterInstanceId -ne $Adapter){throw 'Saved USB configuration belongs to a different adapter.'}
        $peers=@($previous.Devices | ForEach-Object {$_.Mac})
    }
    Write-XoneJson $path @{Version=1;AdapterInstanceId=$Adapter;Peers=$peers}
    return (Read-XoneNativeConfig $path)
}
function Save-XoneNativePeers([string]$Root,$Config,$Snapshot){
    if(-not $Snapshot -or $Snapshot.Backend -ne 'native-gip' -or $null -eq $Snapshot.KnownPeers){return}
    $path=Join-Path $Root 'config/native.local.json'
    $candidate=[pscustomobject]@{Version=1;AdapterInstanceId=$Config.AdapterInstanceId;Peers=@($Snapshot.KnownPeers)}
    # Validate in memory before any write. Invalid status cannot damage pairing data.
    $seen=@{}
    foreach($entry in $candidate.Peers){if($entry -notmatch '^[0-9a-f]{12}$' -or $entry -eq '000000000000' -or $seen.ContainsKey($entry)){throw 'Invalid native peer snapshot.'};$seen[$entry]=$true}
    if($candidate.Peers.Count -gt 16 -or @($Config.Peers | Where-Object {$_ -notin $candidate.Peers}).Count){throw 'Native status lost a saved peer; existing pairing data was preserved.'}
    if(($candidate.Peers -join ',') -ne ($Config.Peers -join ',')){Write-XoneJson $path $candidate;$Config.Peers=$candidate.Peers}
}
